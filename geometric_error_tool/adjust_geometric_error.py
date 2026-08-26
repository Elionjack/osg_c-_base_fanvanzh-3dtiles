#!/usr/bin/env python3
"""Recompute 3D Tiles geometricError values without rebuilding GLBs.

The calculation mirrors src1.1:

* PagedLOD direct parents of leaves keep their physical-size-derived error.
* Higher PagedLOD parents use max(child error) * tile factor.
* A level-0 HLOD containing one source cell inherits that cell's error.
* A level-0 HLOD containing multiple cells uses max(cell error) * HLOD factor.
* Higher HLOD parents use max(child error) * HLOD factor.
* Index-only wrappers keep the 1e12 refinement override in JSON, while their
  finite natural error is still used to calculate the parent. This distinction
  matches src1.1's build-then-encode behavior.

External tileset JSON references are processed recursively. GLB files and all
non-geometricError JSON fields are left unchanged.
"""

import argparse
import json
import math
import os
import shutil
import sys
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, NamedTuple, Optional, Set
from urllib.parse import unquote, urlsplit


INDEX_ONLY_GEOMETRIC_ERROR = 1.0e12


class TileResult(NamedTuple):
    natural_ge: float
    output_ge: float
    kind: str
    has_children: bool


class DocumentResult(NamedTuple):
    natural_ge: float
    output_ge: float
    root_kind: str
    root_has_children: bool


class Statistics:
    def __init__(self) -> None:
        self.documents = 0
        self.tiles = 0
        self.changed_tiles = 0
        self.external_references = 0
        self.index_only_tiles = 0


def numeric_ge(value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0.0
    result = float(value)
    return result if math.isfinite(result) and result >= 0.0 else 0.0


def content_uri(tile: Dict[str, Any]) -> str:
    content = tile.get("content")
    if not isinstance(content, dict):
        return ""
    uri = content.get("uri", content.get("url", ""))
    return uri if isinstance(uri, str) else ""


def uri_path(uri: str) -> str:
    return unquote(urlsplit(uri).path).replace("\\", "/")


def is_json_uri(uri: str) -> bool:
    return uri_path(uri).lower().endswith(".json")


def is_hlod_uri(uri: str) -> bool:
    path = uri_path(uri).lower()
    name = path.rsplit("/", 1)[-1]
    return (
        "/hlod/" in path
        or name.startswith("hlod_")
        or name == "root.glb"
    )


def nearly_equal(left: float, right: float) -> bool:
    return math.isclose(left, right, rel_tol=1.0e-12, abs_tol=1.0e-12)


class GeometricErrorProcessor:
    def __init__(
        self,
        root_json: Path,
        factor: float,
        apply_changes: bool,
        backup_dir: Optional[Path],
        hlod_factor: float = 1.55,
    ) -> None:
        self.root_json = root_json.resolve()
        self.dataset_root = self.root_json.parent
        self.tile_factor = factor
        self.hlod_factor = hlod_factor
        self.apply_changes = apply_changes
        self.requested_backup_dir = backup_dir
        self.statistics = Statistics()
        self._cache: Dict[Path, DocumentResult] = {}
        self._processing: Set[Path] = set()
        self._changed_documents: List[Path] = []
        self._stage_dir: Optional[Path] = None

    def run(self) -> DocumentResult:
        if self.apply_changes:
            self._stage_dir = Path(
                tempfile.mkdtemp(
                    prefix=".geometric_error_stage_",
                    dir=str(self.dataset_root),
                )
            )

        try:
            result = self._process_document(self.root_json)
            if self.apply_changes:
                self._commit()
            return result
        finally:
            if self._stage_dir and self._stage_dir.exists():
                shutil.rmtree(self._stage_dir, ignore_errors=True)

    def _relative_path(self, path: Path) -> Path:
        try:
            return path.resolve().relative_to(self.dataset_root)
        except ValueError as exc:
            raise ValueError(
                f"External tileset escapes dataset root: {path}"
            ) from exc

    def _resolve_external(self, source_json: Path, uri: str) -> Path:
        raw_path = uri_path(uri)
        target = (source_json.parent / raw_path).resolve()
        self._relative_path(target)
        if not target.is_file():
            raise FileNotFoundError(
                f"Referenced tileset does not exist: {uri} "
                f"(from {source_json})"
            )
        return target

    def _process_document(self, path: Path) -> DocumentResult:
        path = path.resolve()
        if path in self._cache:
            return self._cache[path]
        if path in self._processing:
            raise ValueError(f"Cyclic external tileset reference: {path}")

        self._relative_path(path)
        self._processing.add(path)
        try:
            with path.open("r", encoding="utf-8-sig") as stream:
                document = json.load(stream)
            if not isinstance(document, dict) or not isinstance(
                document.get("root"), dict
            ):
                raise ValueError(f"Not a 3D Tiles tileset document: {path}")

            self.statistics.documents += 1
            root_hint_hlod = path.name.lower().startswith("hlod_")
            root_result = self._process_tile(
                document["root"], path, root_hint_hlod
            )

            old_envelope_ge = numeric_ge(document.get("geometricError"))
            document["geometricError"] = root_result.output_ge
            envelope_changed = not nearly_equal(
                old_envelope_ge, root_result.output_ge
            )

            result = DocumentResult(
                natural_ge=root_result.natural_ge,
                output_ge=root_result.output_ge,
                root_kind=root_result.kind,
                root_has_children=root_result.has_children,
            )
            self._cache[path] = result

            if envelope_changed or getattr(
                self, "_current_document_changed", {}
            ).pop(path, False):
                self._changed_documents.append(path)
                if self.apply_changes:
                    self._stage_document(path, document)
            return result
        finally:
            self._processing.remove(path)

    def _mark_document_changed(self, path: Path) -> None:
        changed = getattr(self, "_current_document_changed", None)
        if changed is None:
            changed = {}
            self._current_document_changed = changed
        changed[path] = True

    def _set_tile_ge(
        self,
        tile: Dict[str, Any],
        new_ge: float,
        source_json: Path,
    ) -> None:
        old_ge = numeric_ge(tile.get("geometricError"))
        tile["geometricError"] = new_ge
        if not nearly_equal(old_ge, new_ge):
            self.statistics.changed_tiles += 1
            self._mark_document_changed(source_json)

    def _process_tile(
        self,
        tile: Dict[str, Any],
        source_json: Path,
        root_hint_hlod: bool = False,
    ) -> TileResult:
        self.statistics.tiles += 1
        old_ge = numeric_ge(tile.get("geometricError"))
        uri = content_uri(tile)

        # A reference tile represents the natural error of the referenced
        # subtree. The referenced document root may itself carry the 1e12
        # index-only override, which must not propagate into its parent.
        if uri and is_json_uri(uri):
            target = self._resolve_external(source_json, uri)
            external = self._process_document(target)
            self.statistics.external_references += 1
            self._set_tile_ge(tile, external.natural_ge, source_json)
            return TileResult(
                natural_ge=external.natural_ge,
                output_ge=external.natural_ge,
                kind=external.root_kind,
                has_children=external.root_has_children,
            )

        children = tile.get("children")
        child_tiles = (
            [child for child in children if isinstance(child, dict)]
            if isinstance(children, list)
            else []
        )
        child_results = [
            self._process_tile(child, source_json) for child in child_tiles
        ]
        has_children = bool(child_results)

        has_hlod_child = any(
            child.kind == "hlod" for child in child_results
        )
        is_index_only = (
            has_children
            and not uri
            and old_ge >= INDEX_ONLY_GEOMETRIC_ERROR
        )
        kind = (
            "hlod"
            if root_hint_hlod
            or is_hlod_uri(uri)
            or has_hlod_child
            or is_index_only
            else "pagedlod"
        )

        if not has_children:
            natural_ge = old_ge
        elif kind == "hlod":
            hlod_children = [
                child for child in child_results if child.kind == "hlod"
            ]
            if hlod_children:
                natural_ge = max(
                    child.natural_ge for child in hlod_children
                ) * self.hlod_factor
            else:
                max_cell_ge = max(
                    child.natural_ge for child in child_results
                )
                natural_ge = (
                    max_cell_ge
                    if len(child_results) == 1
                    else max_cell_ge * self.hlod_factor
                )
        else:
            direct_parent_of_leaves = all(
                not child.has_children for child in child_results
            )
            if direct_parent_of_leaves:
                # src1.1 derives this value from child bounding boxes * 0.1.
                # Changing the upper-level multiplier does not alter it.
                natural_ge = old_ge
            else:
                natural_ge = max(
                    child.natural_ge for child in child_results
                ) * self.tile_factor

        output_ge = natural_ge
        if is_index_only:
            output_ge = max(natural_ge, INDEX_ONLY_GEOMETRIC_ERROR)
            self.statistics.index_only_tiles += 1

        self._set_tile_ge(tile, output_ge, source_json)
        return TileResult(
            natural_ge=natural_ge,
            output_ge=output_ge,
            kind=kind,
            has_children=has_children,
        )

    def _stage_document(
        self, original_path: Path, document: Dict[str, Any]
    ) -> None:
        assert self._stage_dir is not None
        relative = self._relative_path(original_path)
        staged_path = self._stage_dir / relative
        staged_path.parent.mkdir(parents=True, exist_ok=True)
        with staged_path.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(
                document,
                stream,
                ensure_ascii=False,
                allow_nan=False,
                separators=(",", ":"),
            )
            stream.write("\n")

    def _commit(self) -> None:
        assert self._stage_dir is not None
        if not self._changed_documents:
            return

        if self.requested_backup_dir:
            backup_dir = self.requested_backup_dir.resolve()
        else:
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            backup_dir = (
                self.dataset_root / f"geometric_error_backup_{stamp}"
            )
        if backup_dir.exists():
            raise FileExistsError(f"Backup directory already exists: {backup_dir}")
        backup_dir.mkdir(parents=True)

        # Back up every target before replacing any file.
        for original in self._changed_documents:
            relative = self._relative_path(original)
            backup = backup_dir / relative
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, backup)

        for original in self._changed_documents:
            relative = self._relative_path(original)
            staged = self._stage_dir / relative
            os.replace(staged, original)

        print(f"Backup: {backup_dir}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Recompute src1.1 geometricError values in tileset JSON files "
            "without rebuilding GLBs."
        )
    )
    parser.add_argument(
        "input",
        type=Path,
        help="Dataset directory or its root tileset.json",
    )
    parser.add_argument(
        "--factor",
        type=float,
        default=2.0,
        help="PagedLOD tile geometric-error factor (default: 2.0)",
    )
    parser.add_argument(
        "--hlod-factor",
        type=float,
        default=1.55,
        help="HLOD geometric-error factor (default: 1.55)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Write changes. Without this flag the tool performs a dry run.",
    )
    parser.add_argument(
        "--backup-dir",
        type=Path,
        help="Backup directory used with --apply (default: timestamped folder)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if not math.isfinite(args.factor) or args.factor <= 0.0:
        print("ERROR: --factor must be a finite positive number", file=sys.stderr)
        return 2
    if not math.isfinite(args.hlod_factor) or args.hlod_factor <= 0.0:
        print(
            "ERROR: --hlod-factor must be a finite positive number",
            file=sys.stderr,
        )
        return 2

    root_json = args.input
    if root_json.is_dir():
        root_json = root_json / "tileset.json"
    if not root_json.is_file():
        print(f"ERROR: tileset.json not found: {root_json}", file=sys.stderr)
        return 2

    try:
        processor = GeometricErrorProcessor(
            root_json=root_json,
            factor=args.factor,
            apply_changes=args.apply,
            backup_dir=args.backup_dir,
            hlod_factor=args.hlod_factor,
        )
        old_root_ge = 0.0
        with root_json.open("r", encoding="utf-8-sig") as stream:
            original = json.load(stream)
            old_root_ge = numeric_ge(original.get("geometricError"))

        result = processor.run()
        stats = processor.statistics
        mode = "APPLIED" if args.apply else "DRY RUN"
        print(f"Mode: {mode}")
        print(f"PagedLOD tile factor: {args.factor:g}")
        print(f"HLOD factor: {args.hlod_factor:g}")
        print(f"Root geometricError: {old_root_ge:.15g} -> {result.output_ge:.15g}")
        print(f"Documents visited: {stats.documents}")
        print(f"Tiles visited: {stats.tiles}")
        print(f"Tiles changed: {stats.changed_tiles}")
        print(f"External references: {stats.external_references}")
        print(f"Index-only tiles preserved: {stats.index_only_tiles}")
        print(f"JSON files changed: {len(processor._changed_documents)}")
        if not args.apply:
            print("No files were written. Add --apply to commit the changes.")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
