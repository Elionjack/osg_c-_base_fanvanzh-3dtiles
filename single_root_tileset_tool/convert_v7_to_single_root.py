#!/usr/bin/env python3
"""Convert an old split multi-HLOD tileset into the single-root layout.

The source is never modified. Data files are hard-linked into a new output
directory when possible and copied only when hard links are unavailable.
"""

import argparse
import copy
import json
import os
import shutil
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Dict, Iterator, Optional, Tuple


ROOT_GEOMETRIC_ERROR = 1000.0


def iter_tiles(node: Dict[str, Any], depth: int = 0) -> Iterator[Tuple[Dict[str, Any], int]]:
    yield node, depth
    children = node.get("children", [])
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                yield from iter_tiles(child, depth + 1)


def content_uri(node: Dict[str, Any]) -> Optional[str]:
    content = node.get("content")
    if not isinstance(content, dict):
        return None
    uri = content.get("uri")
    return uri if isinstance(uri, str) else None


def tile_directory_from_root_uri(uri: str) -> Optional[str]:
    """Return Tile_* only for Data/Tile_*/Tile_*.glb directory roots."""
    parts = PurePosixPath(uri.replace("\\", "/")).parts
    try:
        data_index = parts.index("Data")
    except ValueError:
        return None
    if data_index + 2 >= len(parts):
        return None
    directory = parts[data_index + 1]
    filename = parts[data_index + 2]
    if directory == "HLOD" or not directory.startswith("Tile_"):
        return None
    if PurePosixPath(filename).stem != directory:
        return None
    return directory


def load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"JSON root is not an object: {path}")
    return value


def extract_tile_roots(source: Path) -> Dict[str, Dict[str, Any]]:
    subtilesets = source / "subtilesets"
    if not subtilesets.is_dir():
        raise FileNotFoundError(f"Missing subtilesets directory: {subtilesets}")

    selected = {}  # type: Dict[str, Tuple[int, Dict[str, Any], Path]]
    shard_paths = sorted(subtilesets.glob("*.json"))
    if not shard_paths:
        raise FileNotFoundError(f"No JSON shards found in: {subtilesets}")

    for shard_path in shard_paths:
        document = load_json(shard_path)
        root = document.get("root")
        if not isinstance(root, dict):
            raise ValueError(f"Missing tile root in: {shard_path}")
        for node, depth in iter_tiles(root):
            uri = content_uri(node)
            if uri is None:
                continue
            stem = tile_directory_from_root_uri(uri)
            if stem is None:
                continue
            previous = selected.get(stem)
            if previous is None or depth < previous[0]:
                selected[stem] = (depth, copy.deepcopy(node), shard_path)

    if not selected:
        raise ValueError("No Data/Tile_*/Tile_*.glb directory roots were found")
    return {stem: value[1] for stem, value in selected.items()}


def normalize_subtileset_uris(node: Dict[str, Any]) -> None:
    uri = content_uri(node)
    if uri is not None:
        normalized = uri.replace("\\", "/")
        if normalized.startswith("./Data/"):
            normalized = "." + normalized
        elif normalized.startswith("Data/"):
            normalized = "../" + normalized
        node["content"]["uri"] = normalized
    children = node.get("children", [])
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                normalize_subtileset_uris(child)


def link_or_copy_file(source: Path, destination: Path) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
        return "linked"
    except OSError:
        shutil.copy2(source, destination)
        return "copied"


def link_tree(source: Path, destination: Path) -> Tuple[int, int]:
    linked = 0
    copied = 0
    for directory, _, filenames in os.walk(source):
        source_directory = Path(directory)
        relative = source_directory.relative_to(source)
        output_directory = destination / relative
        output_directory.mkdir(parents=True, exist_ok=True)
        for filename in filenames:
            mode = link_or_copy_file(source_directory / filename, output_directory / filename)
            if mode == "linked":
                linked += 1
            else:
                copied += 1
    return linked, copied


def build_root_document(
    source_document: Dict[str, Any], tile_roots: Dict[str, Dict[str, Any]]
) -> Dict[str, Any]:
    old_root = source_document.get("root")
    if not isinstance(old_root, dict):
        raise ValueError("Source tileset.json has no root tile")
    root_content = old_root.get("content")
    if not isinstance(root_content, dict):
        raise ValueError("Source root has no reconstructed root.glb content")

    root = {  # type: Dict[str, Any]
        "boundingVolume": copy.deepcopy(old_root["boundingVolume"]),
        "content": copy.deepcopy(root_content),
        "geometricError": ROOT_GEOMETRIC_ERROR,
        "refine": "REPLACE",
        "children": [],
    }
    if "transform" in old_root:
        root["transform"] = copy.deepcopy(old_root["transform"])

    root["content"]["uri"] = "./Data/HLOD/root.glb"
    for stem in sorted(tile_roots):
        tile = tile_roots[stem]
        reference = {  # type: Dict[str, Any]
            "boundingVolume": copy.deepcopy(tile["boundingVolume"]),
            "content": {"uri": f"./subtilesets/{stem}.json"},
            "geometricError": tile.get("geometricError", 0.0),
            "refine": "REPLACE",
        }
        root["children"].append(reference)

    return {
        "asset": copy.deepcopy(source_document.get("asset", {"version": "1.1"})),
        "extensionsRequired": copy.deepcopy(
            source_document.get("extensionsRequired", ["3DTILES_content_gltf"])
        ),
        "extensionsUsed": copy.deepcopy(
            source_document.get("extensionsUsed", ["3DTILES_content_gltf"])
        ),
        "geometricError": ROOT_GEOMETRIC_ERROR,
        "root": root,
    }


def write_json(path: Path, value: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))


def validate_source(source: Path, tile_roots: Dict[str, Dict[str, Any]]) -> None:
    root_glb = source / "Data" / "HLOD" / "root.glb"
    if not root_glb.is_file():
        raise FileNotFoundError(f"Missing final reconstructed GLB: {root_glb}")
    missing = [stem for stem in tile_roots if not (source / "Data" / stem).is_dir()]
    if missing:
        preview = ", ".join(missing[:10])
        raise FileNotFoundError(f"Missing {len(missing)} tile data directories: {preview}")


def convert(source: Path, output: Path, dry_run: bool) -> None:
    source = source.resolve()
    output = output.resolve()
    if source == output:
        raise ValueError("Output must differ from source; in-place conversion is not allowed")
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")

    source_document = load_json(source / "tileset.json")
    tile_roots = extract_tile_roots(source)
    validate_source(source, tile_roots)
    root_document = build_root_document(source_document, tile_roots)

    print(f"source: {source}")
    print(f"output: {output}")
    print(f"tile directories found: {len(tile_roots)}")
    print(f"root geometricError: {ROOT_GEOMETRIC_ERROR:g}")
    if dry_run:
        print("dry run: no files written")
        return

    staging = output.parent / f".{output.name}.tmp-{os.getpid()}"
    if staging.exists():
        raise FileExistsError(f"Staging directory already exists: {staging}")

    linked = 0
    copied = 0
    try:
        staging.mkdir(parents=True)
        for stem in sorted(tile_roots):
            tile = tile_roots[stem]
            normalize_subtileset_uris(tile)
            envelope = {
                "asset": {"version": "1.1"},
                "extensionsRequired": ["3DTILES_content_gltf"],
                "extensionsUsed": ["3DTILES_content_gltf"],
                "geometricError": tile.get("geometricError", 0.0),
                "root": tile,
            }
            write_json(staging / "subtilesets" / f"{stem}.json", envelope)
            tree_linked, tree_copied = link_tree(
                source / "Data" / stem, staging / "Data" / stem
            )
            linked += tree_linked
            copied += tree_copied

        mode = link_or_copy_file(
            source / "Data" / "HLOD" / "root.glb",
            staging / "Data" / "HLOD" / "root.glb",
        )
        linked += mode == "linked"
        copied += mode == "copied"
        write_json(staging / "tileset.json", root_document)
        staging.rename(output)
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        raise

    print(f"data files hard-linked: {linked}")
    print(f"data files copied: {copied}")
    print("conversion complete")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert an old multi-HLOD split tileset to one root.glb directly referencing tile directories."
    )
    parser.add_argument("source", type=Path, help="old output directory (for example 25v7)")
    parser.add_argument("output", type=Path, help="new output directory")
    parser.add_argument("--dry-run", action="store_true", help="validate and report without writing")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        convert(args.source, args.output, args.dry_run)
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
