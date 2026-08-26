#!/usr/bin/env python3
"""Upgrade an existing split HLOD tileset to the converter's current rules.

The tool only rewrites JSON documents. Geometry and texture files are never
modified. Without --apply it performs a dry run.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import unquote, urlsplit


INDEX_ONLY_GE = 1.0e12
HLOD_NAME_RE = re.compile(r"(?:^|/)L(?P<display>\d+)_X[+-]?\d+_Y[+-]?\d+\.glb$", re.I)
TILE_STEM_RE = re.compile(r"^Tile_([+-]?\d+)_([+-]?\d+)$")
SOURCE_LEVEL_RE = re.compile(r"_L(\d+)(?:_|\.|o\.)", re.I)


@dataclass
class Stats:
    documents: int = 0
    missing_content: int = 0
    promoted_nodes: int = 0
    pruned_nodes: int = 0
    source_roots: int = 0
    source_nodes: int = 0
    source_ge_changed: int = 0
    hlod_ge_changed: int = 0
    external_ge_changed: int = 0


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"JSON root is not an object: {path}")
    return value


def content_uri(tile: dict[str, Any]) -> str | None:
    content = tile.get("content")
    if not isinstance(content, dict):
        return None
    uri = content.get("uri")
    return uri if isinstance(uri, str) and uri else None


def local_uri_target(document: Path, uri: str) -> Path | None:
    parts = urlsplit(uri)
    if parts.scheme or parts.netloc:
        return None
    decoded = unquote(parts.path).replace("/", os.sep)
    # abspath/normpath is enough for local file lookup. Path.resolve() performs
    # extra filesystem work and becomes very expensive for 80k+ references.
    return Path(os.path.abspath(os.path.normpath(str(document.parent / decoded))))


def valid_file(path: Path, cache: dict[Path, bool]) -> bool:
    cached = cache.get(path)
    if cached is not None:
        return cached
    try:
        value = path.is_file() and path.stat().st_size > 0
    except OSError:
        value = False
    cache[path] = value
    return value


def box_half_span(tile: dict[str, Any]) -> float:
    volume = tile.get("boundingVolume")
    box = volume.get("box") if isinstance(volume, dict) else None
    if not isinstance(box, list) or len(box) != 12:
        return 0.0
    try:
        # The converter emits axis-aligned boxes. Norms also make this safe for
        # a future oriented box without changing the intended half-span scale.
        axes = (
            math.sqrt(sum(float(box[i + j]) ** 2 for j in range(3)))
            for i in (3, 6, 9)
        )
        value = max(axes)
        return value if math.isfinite(value) and value > 0.0 else 0.0
    except (TypeError, ValueError):
        return 0.0


def set_ge(tile: dict[str, Any], value: float, counter: str, stats: Stats) -> None:
    old = tile.get("geometricError")
    try:
        changed = old is None or not math.isclose(float(old), value, rel_tol=1e-12, abs_tol=1e-9)
    except (TypeError, ValueError):
        changed = True
    tile["geometricError"] = value
    if changed:
        setattr(stats, counter, getattr(stats, counter) + 1)


def repair_references(tile: dict[str, Any], document: Path, stats: Stats,
                      file_cache: dict[Path, bool],
                      is_document_root: bool = False) -> list[dict[str, Any]]:
    """Return zero or more replacement nodes for this tile."""
    children = tile.get("children")
    repaired_children: list[dict[str, Any]] = []
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                repaired_children.extend(
                    repair_references(child, document, stats, file_cache))
        tile["children"] = repaired_children

    uri = content_uri(tile)
    if uri:
        target = local_uri_target(document, uri)
        if target is not None and not valid_file(target, file_cache):
            tile.pop("content", None)
            stats.missing_content += 1
            uri = None

    has_content = content_uri(tile) is not None
    has_children = bool(repaired_children)
    if has_content:
        return [tile]
    if has_children:
        if is_document_root:
            tile["geometricError"] = INDEX_ONLY_GE
            return [tile]
        stats.promoted_nodes += 1
        return repaired_children

    stats.pruned_nodes += 1
    return []


def recalc_source_ge(tile: dict[str, Any], stats: Stats) -> float:
    children = [child for child in tile.get("children", []) if isinstance(child, dict)]
    child_errors = [recalc_source_ge(child, stats) for child in children]
    stats.source_nodes += 1

    if not children:
        value = 0.0
    elif all(not child.get("children") for child in children):
        value = max((box_half_span(child) * 0.1 for child in children), default=0.0)
    else:
        value = max(child_errors, default=0.0) * 2.5

    cap = box_half_span(tile)
    if cap > 0.0:
        value = min(value, cap)
    set_ge(tile, value, "source_ge_changed", stats)
    return value


def walk_tiles(tile: dict[str, Any]) -> Iterable[dict[str, Any]]:
    yield tile
    for child in tile.get("children", []):
        if isinstance(child, dict):
            yield from walk_tiles(child)


def is_hlod_content(uri: str | None) -> bool:
    return bool(uri and "/HLOD/" in uri.replace("\\", "/"))


def discover_source_roots(sub_documents: dict[Path, dict[str, Any]]) -> list[dict[str, Any]]:
    roots: list[dict[str, Any]] = []
    for envelope in sub_documents.values():
        hlod_root = envelope.get("root")
        if not isinstance(hlod_root, dict):
            continue
        for child in hlod_root.get("children", []):
            if isinstance(child, dict) and not is_hlod_content(content_uri(child)):
                roots.append(child)
    return roots


def grid_extent_from_source_roots(source_roots: list[dict[str, Any]]) -> int:
    coords: list[tuple[int, int]] = []
    for root in source_roots:
        uri = content_uri(root)
        if not uri:
            continue
        stem = Path(urlsplit(uri).path).parent.name
        match = TILE_STEM_RE.match(stem)
        if match:
            coords.append((int(match.group(1)), int(match.group(2))))
    if not coords:
        return 1
    width = max(x for x, _ in coords) - min(x for x, _ in coords) + 1
    height = max(y for _, y in coords) - min(y for _, y in coords) + 1
    return max(width, height)


def padded_grid_size(max_extent: int, branch_side: int) -> int:
    size = branch_side
    while size < max_extent:
        size *= branch_side
    return size


def hlod_display_level(uri: str | None) -> int | None:
    if not uri:
        return None
    normalized = urlsplit(uri).path.replace("\\", "/")
    if normalized.lower().endswith("/root.glb"):
        return -1
    match = HLOD_NAME_RE.search(normalized)
    return int(match.group("display")) if match else None


def apply_hlod_ge(tile: dict[str, Any], base_cell_ge: float, padded_size: int,
                  branch_side: int, stats: Stats) -> None:
    uri = content_uri(tile)
    level = hlod_display_level(uri)
    if level is not None:
        grid_size = padded_size if level < 0 else max(
            branch_side, padded_size // (branch_side ** (level + 1)))
        set_ge(tile, base_cell_ge * grid_size, "hlod_ge_changed", stats)
    for child in tile.get("children", []):
        if isinstance(child, dict):
            apply_hlod_ge(child, base_cell_ge, padded_size, branch_side, stats)


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, path)
    except Exception:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def make_backup(root: Path, documents: list[Path]) -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = root / f"_json_backup_before_current_repair_{stamp}"
    for source in documents:
        relative = source.relative_to(root)
        target = backup / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    return backup


def validate_references(documents: dict[Path, dict[str, Any]],
                        file_cache: dict[Path, bool]) -> list[tuple[Path, str]]:
    missing: list[tuple[Path, str]] = []
    for document, envelope in documents.items():
        root = envelope.get("root")
        if not isinstance(root, dict):
            continue
        for tile in walk_tiles(root):
            uri = content_uri(tile)
            if not uri:
                continue
            target = local_uri_target(document, uri)
            if target is not None and not valid_file(target, file_cache):
                missing.append((document, uri))
    return missing


def summarize_hlod_levels(documents: dict[Path, dict[str, Any]]) -> dict[str, set[float]]:
    levels: dict[str, set[float]] = {}
    for envelope in documents.values():
        root = envelope.get("root")
        if not isinstance(root, dict):
            continue
        for tile in walk_tiles(root):
            level = hlod_display_level(content_uri(tile))
            if level is None:
                continue
            label = "root" if level < 0 else f"L{level}"
            try:
                levels.setdefault(label, set()).add(float(tile["geometricError"]))
            except (KeyError, TypeError, ValueError):
                pass
    return levels


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def summarize_source_levels(documents: dict[Path, dict[str, Any]]) -> dict[str, list[float]]:
    groups: dict[str, list[float]] = {}
    for envelope in documents.values():
        root = envelope.get("root")
        if not isinstance(root, dict):
            continue
        for tile in walk_tiles(root):
            uri = content_uri(tile)
            if not uri:
                continue
            normalized = urlsplit(uri).path.replace("\\", "/")
            if "/Data/Tile_" not in normalized or normalized.lower().endswith(".json"):
                continue
            name = Path(normalized).name
            if "_FINE_MERGE_" in name.upper():
                label = "FINE_MERGE"
            else:
                match = SOURCE_LEVEL_RE.search(name)
                label = f"L{int(match.group(1))}" if match else "COARSE"
            try:
                value = float(tile["geometricError"])
            except (KeyError, TypeError, ValueError):
                continue
            groups.setdefault(label, []).append(value)
    return groups


def run(root: Path, apply: bool, branching_factor: int) -> int:
    root = root.resolve()
    root_json = root / "tileset.json"
    if not root_json.is_file():
        raise FileNotFoundError(f"tileset.json not found: {root_json}")
    branch_side = math.isqrt(branching_factor)
    if branch_side * branch_side != branching_factor or branch_side < 2:
        raise ValueError("--branching-factor must be a perfect square >= 4")

    paths = [root_json]
    sub_dir = root / "subtilesets"
    if sub_dir.is_dir():
        paths.extend(sorted(sub_dir.glob("*.json")))
    original = {path: load_json(path) for path in paths}
    documents = copy.deepcopy(original)
    stats = Stats(documents=len(documents))
    file_cache: dict[Path, bool] = {}

    for path, envelope in documents.items():
        tile = envelope.get("root")
        if not isinstance(tile, dict):
            raise ValueError(f"Missing root tile: {path}")
        replacements = repair_references(
            tile, path, stats, file_cache, is_document_root=True)
        if len(replacements) != 1:
            raise ValueError(f"Document root became empty: {path}")
        envelope["root"] = replacements[0]

    sub_documents = {p: value for p, value in documents.items() if p != root_json}
    source_roots = discover_source_roots(sub_documents)
    stats.source_roots = len(source_roots)
    if not source_roots:
        raise ValueError("No source PagedLOD roots found; expected split HLOD output")
    for source_root in source_roots:
        recalc_source_ge(source_root, stats)

    # Phase 3 extends each source root bbox by 20%; half-span therefore grows
    # by 1.2 before build_spatial_grid() derives the common HLOD unit.
    base_cell_ge = max((box_half_span(root_tile) * 1.2 for root_tile in source_roots), default=0.0)
    if not math.isfinite(base_cell_ge) or base_cell_ge <= 0.0:
        raise ValueError("Could not derive a valid base_cell_ge")
    max_extent = grid_extent_from_source_roots(source_roots)
    padded_size = padded_grid_size(max_extent, branch_side)

    main_root = documents[root_json]["root"]
    apply_hlod_ge(main_root, base_cell_ge, padded_size, branch_side, stats)
    for envelope in sub_documents.values():
        apply_hlod_ge(envelope["root"], base_cell_ge, padded_size, branch_side, stats)

    # Synchronize external JSON reference errors with their target root.
    sub_by_name = {path.name: envelope for path, envelope in sub_documents.items()}
    for tile in walk_tiles(main_root):
        uri = content_uri(tile)
        if not uri or not urlsplit(uri).path.lower().endswith(".json"):
            continue
        target = sub_by_name.get(Path(urlsplit(uri).path).name)
        if target and isinstance(target.get("root"), dict):
            value = float(target["root"]["geometricError"])
            set_ge(tile, value, "external_ge_changed", stats)

    documents[root_json]["geometricError"] = float(main_root["geometricError"])
    for envelope in sub_documents.values():
        envelope["geometricError"] = float(envelope["root"]["geometricError"])

    missing_after = validate_references(documents, file_cache)
    levels = summarize_hlod_levels(documents)
    source_levels = summarize_source_levels(documents)

    print(f"Tileset: {root}")
    print(f"Mode: {'APPLY' if apply else 'DRY RUN'}")
    print(f"JSON documents: {stats.documents}")
    print(f"Missing content removed: {stats.missing_content}")
    print(f"Index nodes promoted: {stats.promoted_nodes}")
    print(f"Empty nodes pruned: {stats.pruned_nodes}")
    print(f"Source roots/nodes: {stats.source_roots}/{stats.source_nodes}")
    print(f"Source GE changed: {stats.source_ge_changed}")
    print(f"HLOD GE changed: {stats.hlod_ge_changed}")
    print(f"External reference GE changed: {stats.external_ge_changed}")
    print(f"base_cell_ge: {base_cell_ge:.6f}")
    print(f"grid max extent/padded: {max_extent}/{padded_size}")
    for label in sorted(levels, key=lambda item: (-1 if item == 'root' else int(item[1:]))):
        values = levels[label]
        preview = ", ".join(f"{value:.6f}" for value in sorted(values)[:4])
        print(f"HLOD {label}: unique={len(values)} values=[{preview}]")
    print("Source PagedLOD geometricError by file level:")
    source_order = lambda item: (-2 if item == "COARSE" else
                                 -1 if item == "FINE_MERGE" else int(item[1:]))
    for label in sorted(source_levels, key=source_order):
        values = source_levels[label]
        print(
            f"  {label:10s} count={len(values):6d} "
            f"min={min(values):10.6f} p50={percentile(values, 0.50):10.6f} "
            f"p90={percentile(values, 0.90):10.6f} max={max(values):10.6f}")
    print(f"Missing references after repair: {len(missing_after)}")
    for path, uri in missing_after[:10]:
        print(f"  {path}: {uri}")

    if missing_after:
        print("Refusing to write because unresolved references remain.", file=sys.stderr)
        return 2
    if not apply:
        print("Dry run only. Re-run with --apply to back up and rewrite JSON files.")
        return 0

    backup = make_backup(root, paths)
    try:
        for path in paths:
            write_json_atomic(path, documents[path])
    except Exception:
        for path in paths:
            backup_path = backup / path.relative_to(root)
            if backup_path.is_file():
                shutil.copy2(backup_path, path)
        raise
    print(f"Backup: {backup}")
    print("JSON upgrade completed; GLB files were not modified.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Upgrade an existing split HLOD tileset to current JSON/GE rules")
    parser.add_argument("tileset", type=Path, help="output directory containing tileset.json")
    parser.add_argument("--branching-factor", type=int, default=16)
    parser.add_argument("--apply", action="store_true", help="back up and rewrite JSON files")
    args = parser.parse_args()
    try:
        return run(args.tileset, args.apply, args.branching_factor)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
