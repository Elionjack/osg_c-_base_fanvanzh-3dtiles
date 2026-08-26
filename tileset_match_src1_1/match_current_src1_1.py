#!/usr/bin/env python3
"""Migrate an existing split HLOD output to the current src1.1 JSON layout.

Only JSON files are changed. Existing GLB payloads are reused unchanged.
The default mode is a read-only dry run; --apply creates a backup first.
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
HLOD_MULTIPLIER = 1.55
HLOD_RE = re.compile(r"(?:^|/)L(?P<display>\d+)_X[+-]?\d+_Y[+-]?\d+\.glb$", re.I)
TILE_RE = re.compile(r"^Tile_[+-]?\d+_[+-]?\d+$", re.I)


@dataclass
class Stats:
    input_documents: int = 0
    output_documents: int = 0
    old_group_documents: int = 0
    source_documents: int = 0
    missing_content: int = 0
    pruned_leaves: int = 0
    source_index_nodes: int = 0
    promoted_hlod_nodes: int = 0
    source_ge_changed: int = 0
    hlod_ge_changed: int = 0


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


def set_content_uri(tile: dict[str, Any], uri: str) -> None:
    content = tile.setdefault("content", {})
    content["uri"] = uri


def walk_tiles(tile: dict[str, Any]) -> Iterable[dict[str, Any]]:
    yield tile
    for child in tile.get("children", []):
        if isinstance(child, dict):
            yield from walk_tiles(child)


def local_target(document: Path, uri: str) -> Path | None:
    parts = urlsplit(uri)
    if parts.scheme or parts.netloc:
        return None
    decoded = unquote(parts.path).replace("/", os.sep)
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
        axes = [math.sqrt(sum(float(box[i + j]) ** 2 for j in range(3)))
                for i in (3, 6, 9)]
        value = max(axes)
        return value if math.isfinite(value) and value > 0.0 else 0.0
    except (TypeError, ValueError):
        return 0.0


def set_ge(tile: dict[str, Any], value: float, counter: str, stats: Stats) -> None:
    old = tile.get("geometricError")
    try:
        changed = not math.isclose(float(old), value, rel_tol=1e-12, abs_tol=1e-9)
    except (TypeError, ValueError):
        changed = True
    tile["geometricError"] = value
    if changed:
        setattr(stats, counter, getattr(stats, counter) + 1)


def normalized_uri(uri: str) -> str:
    return urlsplit(uri).path.replace("\\", "/")


def is_hlod_uri(uri: str | None) -> bool:
    return bool(uri and "/Data/HLOD/" in normalized_uri(uri))


def is_json_uri(uri: str | None) -> bool:
    return bool(uri and normalized_uri(uri).lower().endswith(".json"))


def source_stem(tile: dict[str, Any]) -> str | None:
    for node in walk_tiles(tile):
        uri = content_uri(node)
        if not uri or is_hlod_uri(uri) or is_json_uri(uri):
            continue
        parts = Path(normalized_uri(uri)).parts
        for part in reversed(parts[:-1]):
            if TILE_RE.match(part):
                return part
    return None


def rewrite_data_prefix(tile: dict[str, Any], prefix: str) -> None:
    for node in walk_tiles(tile):
        uri = content_uri(node)
        if not uri or is_json_uri(uri):
            continue
        path = normalized_uri(uri)
        marker = "/Data/"
        index = path.find(marker)
        if index >= 0:
            set_content_uri(node, prefix + path[index + len(marker):])


def repair_source_tree(tile: dict[str, Any], document: Path, stats: Stats,
                       cache: dict[Path, bool]) -> dict[str, Any] | None:
    repaired: list[dict[str, Any]] = []
    for child in tile.get("children", []):
        if isinstance(child, dict):
            value = repair_source_tree(child, document, stats, cache)
            if value is not None:
                repaired.append(value)
    tile["children"] = repaired

    uri = content_uri(tile)
    failed_here = False
    if uri:
        target = local_target(document, uri)
        if target is not None and not valid_file(target, cache):
            tile.pop("content", None)
            stats.missing_content += 1
            failed_here = True

    if content_uri(tile):
        return tile
    if repaired:
        # src1.1 computes source GE before writes are audited. Only a drawable
        # node whose own write failed becomes a 1e12 traversal-only index;
        # an original type-0 grouping node retains its previously computed GE.
        if failed_here or float(tile.get("geometricError", 0.0)) >= INDEX_ONLY_GE:
            tile["geometricError"] = INDEX_ONLY_GE
            stats.source_index_nodes += 1
        return tile
    stats.pruned_leaves += 1
    return None


def recalc_source_ge(tile: dict[str, Any], stats: Stats) -> float:
    preexisting_failed_index = (
        not content_uri(tile)
        and float(tile.get("geometricError", 0.0)) >= INDEX_ONLY_GE
    )
    children = [c for c in tile.get("children", []) if isinstance(c, dict)]
    child_errors = [recalc_source_ge(child, stats) for child in children]
    if not children:
        value = 0.0
    elif all(not child.get("children") for child in children):
        value = max((box_half_span(child) * 0.1 for child in children), default=0.0)
    else:
        value = max(child_errors, default=0.0) * 2.5
    cap = box_half_span(tile)
    if cap > 0.0:
        value = min(value, cap)
    # A previously migrated failed parent stores 1e12 in JSON, but its parent
    # GE was computed from the ordinary pre-write value. Return that ordinary
    # value while leaving the failure marker intact, matching src1.1 exactly.
    if not preexisting_failed_index:
        set_ge(tile, value, "source_ge_changed", stats)
    return value


def make_envelope(root: dict[str, Any]) -> dict[str, Any]:
    ge = float(root.get("geometricError", 0.0))
    return {
        "asset": {"version": "1.1"},
        "extensionsRequired": ["3DTILES_content_gltf"],
        "extensionsUsed": ["3DTILES_content_gltf"],
        "geometricError": ge,
        "root": root,
    }


def make_source_ref(root: dict[str, Any], stem: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "content": {"uri": f"./subtilesets/{stem}.json"},
        "geometricError": float(root.get("geometricError", 0.0)),
        "refine": "REPLACE",
    }
    if "boundingVolume" in root:
        result["boundingVolume"] = copy.deepcopy(root["boundingVolume"])
    return result


def migrate_group(group: dict[str, Any], virtual_document: Path, stats: Stats,
                  cache: dict[Path, bool], source_docs: dict[str, dict[str, Any]]) -> dict[str, Any] | None:
    root = copy.deepcopy(group.get("root"))
    if not isinstance(root, dict):
        return None

    # A current-layout document already contains exactly one source tree.
    direct_stem = source_stem(root) if not is_hlod_uri(content_uri(root)) else None
    if direct_stem:
        rewrite_data_prefix(root, "../Data/")
        recalc_source_ge(root, stats)
        root = repair_source_tree(root, virtual_document, stats, cache)
        if root is not None:
            source_docs[direct_stem] = make_envelope(root)
        return None

    stats.old_group_documents += 1
    refs: list[dict[str, Any]] = []
    for child in root.get("children", []):
        if not isinstance(child, dict):
            continue
        stem = source_stem(child)
        if not stem:
            continue
        source_root = copy.deepcopy(child)
        rewrite_data_prefix(source_root, "../Data/")
        recalc_source_ge(source_root, stats)
        source_root = repair_source_tree(source_root, virtual_document, stats, cache)
        if source_root is None:
            continue
        if stem in source_docs:
            raise ValueError(f"duplicate source tile in old groups: {stem}")
        source_docs[stem] = make_envelope(source_root)
        refs.append(make_source_ref(source_root, stem))

    root["children"] = refs
    rewrite_data_prefix(root, "./Data/")
    uri = content_uri(root)
    if uri:
        target = local_target(virtual_document.parent.parent / "tileset.json", uri)
        if target is not None and not valid_file(target, cache):
            root.pop("content", None)
            stats.missing_content += 1
    return root if content_uri(root) or refs else None


def expand_old_refs(tile: dict[str, Any], replacements: dict[str, dict[str, Any] | None],
                    stats: Stats) -> None:
    children: list[dict[str, Any]] = []
    for child in tile.get("children", []):
        if not isinstance(child, dict):
            continue
        uri = content_uri(child)
        name = Path(normalized_uri(uri)).name if is_json_uri(uri) else None
        if name in replacements:
            replacement = replacements[name]
            if replacement is not None:
                children.append(copy.deepcopy(replacement))
            continue
        expand_old_refs(child, replacements, stats)
        children.append(child)
    tile["children"] = children


def normalize_hlod(tile: dict[str, Any], stats: Stats, is_root: bool = False) -> list[dict[str, Any]]:
    children: list[dict[str, Any]] = []
    for child in tile.get("children", []):
        if isinstance(child, dict):
            children.extend(normalize_hlod(child, stats))
    tile["children"] = children
    if content_uri(tile):
        return [tile]
    if children:
        if is_root:
            tile["geometricError"] = INDEX_ONLY_GE
            return [tile]
        stats.promoted_hlod_nodes += 1
        return children
    return []


def hlod_display(uri: str | None) -> int | None:
    if not uri or not is_hlod_uri(uri):
        return None
    path = normalized_uri(uri)
    if path.lower().endswith("/root.glb"):
        return -1
    match = HLOD_RE.search(path)
    return int(match.group("display")) if match else None


def apply_hlod_ge(root: dict[str, Any], base_ge: float, max_display: int,
                  stats: Stats) -> None:
    max_level = max_display + 1
    for tile in walk_tiles(root):
        display = hlod_display(content_uri(tile))
        if display is None:
            continue
        exponent = max_level + 1 if display < 0 else max_level - display
        set_ge(tile, base_ge * (HLOD_MULTIPLIER ** exponent),
               "hlod_ge_changed", stats)


def write_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def backup_json(target: Path, paths: list[Path]) -> Path:
    backup = target / ("_json_backup_before_src1_1_match_" + datetime.now().strftime("%Y%m%d_%H%M%S"))
    for source in paths:
        destination = backup / source.relative_to(target)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    return backup


def run(target: Path, baseline: Path | None, apply: bool) -> int:
    target = target.resolve()
    baseline = (baseline or target).resolve()
    baseline_root = baseline / "tileset.json"
    target_root = target / "tileset.json"
    if not baseline_root.is_file() or not target_root.is_file():
        raise FileNotFoundError("baseline and target must both contain tileset.json")

    group_paths = sorted((baseline / "subtilesets").glob("*.json"))
    stats = Stats(input_documents=1 + len(group_paths))
    cache: dict[Path, bool] = {}
    source_docs: dict[str, dict[str, Any]] = {}
    replacements: dict[str, dict[str, Any] | None] = {}

    for path in group_paths:
        virtual = target / "subtilesets" / path.name
        replacement = migrate_group(load_json(path), virtual, stats, cache, source_docs)
        # Current-layout Tile_*.json references must remain in the main tree.
        # Only legacy HLOD group documents are expanded into inline HLOD nodes.
        if path.stem.upper().startswith("HLOD_"):
            replacements[path.name] = replacement

    main = load_json(baseline_root)
    main_root = main.get("root")
    if not isinstance(main_root, dict):
        raise ValueError("main tileset has no root object")
    expand_old_refs(main_root, replacements, stats)
    normalized = normalize_hlod(main_root, stats, is_root=True)
    if len(normalized) != 1:
        raise ValueError("main root became empty")
    main_root = normalized[0]
    main["root"] = main_root

    source_roots = [doc["root"] for doc in source_docs.values()]
    if not source_roots:
        raise ValueError("no source Tile_* trees found in subtilesets")
    base_ge = max(max(box_half_span(root) * 1.2,
                      min(float(root.get("geometricError", 0.0)), INDEX_ONLY_GE - 1.0))
                  for root in source_roots)
    displays = [hlod_display(content_uri(tile)) for tile in walk_tiles(main_root)]
    finite_displays = [value for value in displays if value is not None and value >= 0]
    if not finite_displays:
        raise ValueError("no numbered HLOD GLB references found")
    max_display = max(finite_displays)
    apply_hlod_ge(main_root, base_ge, max_display, stats)

    # Source reference GE must match the referenced document root.
    for tile in walk_tiles(main_root):
        uri = content_uri(tile)
        if not is_json_uri(uri):
            continue
        stem = Path(normalized_uri(uri)).stem
        doc = source_docs.get(stem)
        if doc:
            tile["geometricError"] = float(doc["root"]["geometricError"])

    if not content_uri(main_root) and main_root.get("children"):
        main_root["geometricError"] = INDEX_ONLY_GE
    main["geometricError"] = float(main_root.get("geometricError", 0.0))
    stats.source_documents = len(source_docs)
    stats.output_documents = 1 + len(source_docs)

    planned = {target_root.resolve()}
    planned.update((target / "subtilesets" / f"{stem}.json").resolve() for stem in source_docs)
    missing: list[tuple[Path, str]] = []
    documents = {target_root: main}
    documents.update({target / "subtilesets" / f"{stem}.json": doc
                      for stem, doc in source_docs.items()})
    for document, envelope in documents.items():
        for tile in walk_tiles(envelope["root"]):
            uri = content_uri(tile)
            if not uri:
                continue
            resolved = local_target(document, uri)
            if resolved is not None and resolved.resolve() not in planned and not valid_file(resolved, cache):
                missing.append((document, uri))

    print(f"Tileset: {target}")
    print(f"Baseline JSON: {baseline}")
    print(f"Mode: {'APPLY' if apply else 'DRY RUN'}")
    print(f"JSON layout: {stats.input_documents} old -> {stats.output_documents} current")
    print(f"Old HLOD groups: {stats.old_group_documents}")
    print(f"Source Tile_* subtilesets: {stats.source_documents}")
    print(f"Missing GLB references removed: {stats.missing_content}")
    print(f"Failed leaves pruned: {stats.pruned_leaves}")
    print(f"Failed source parents kept as index nodes: {stats.source_index_nodes}")
    print(f"Empty HLOD wrappers promoted: {stats.promoted_hlod_nodes}")
    print(f"Source GE changed: {stats.source_ge_changed}")
    print(f"HLOD GE changed: {stats.hlod_ge_changed}")
    print(f"HLOD base GE / multiplier: {base_ge:.9f} / {HLOD_MULTIPLIER}")
    print(f"HLOD numbered levels: L0..L{max_display}")
    print(f"Result root GE: {main['geometricError']:.9f}")
    print(f"Missing references after migration: {len(missing)}")
    for document, uri in missing[:10]:
        print(f"  {document}: {uri}")
    if missing:
        print("Refusing to write while unresolved references remain.", file=sys.stderr)
        return 2
    if not apply:
        print("Dry run only. Re-run with --apply to back up and rewrite JSON files.")
        return 0

    current_paths = [target_root]
    current_subdir = target / "subtilesets"
    current_paths.extend(sorted(current_subdir.glob("*.json")))
    backup = backup_json(target, current_paths)
    desired = {f"{stem}.json" for stem in source_docs}
    try:
        write_atomic(target_root, main)
        for stem, document in source_docs.items():
            write_atomic(current_subdir / f"{stem}.json", document)
        for old in current_subdir.glob("*.json"):
            if old.name not in desired:
                old.unlink()
    except Exception:
        print(f"Write failed. Original JSON backup is at: {backup}", file=sys.stderr)
        raise
    print(f"Backup: {backup}")
    print("Migration complete; GLB files were not modified.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Match an old split-HLOD output to the current src1.1 JSON layout")
    parser.add_argument("tileset", type=Path, help="target output directory")
    parser.add_argument("--baseline-json-dir", type=Path,
                        help="optional older JSON snapshot to migrate into the target")
    parser.add_argument("--apply", action="store_true", help="back up and rewrite target JSON")
    args = parser.parse_args()
    try:
        return run(args.tileset, args.baseline_json_dir, args.apply)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
