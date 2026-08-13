#!/usr/bin/env python3
"""Batch-optimize every GLB in a 3D Tiles Data/HLOD directory.

The source directory is never modified.  Each optimized GLB is checked before
it is accepted, and the original file is copied when optimization would make
the file larger.  A JSON report is written next to the output directory.
"""

import argparse
import concurrent.futures
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


def read_glb_json(path):
    with path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError("truncated GLB header")
        magic, version, total_length = struct.unpack("<4sII", header)
        if magic != b"glTF" or version != 2:
            raise ValueError("not a GLB v2 file")
        actual_length = path.stat().st_size
        if total_length != actual_length:
            raise ValueError(
                f"GLB length mismatch: header={total_length}, file={actual_length}"
            )
        chunk_header = stream.read(8)
        if len(chunk_header) != 8:
            raise ValueError("missing GLB JSON chunk")
        json_length, json_type = struct.unpack("<II", chunk_header)
        if json_type != 0x4E4F534A:
            raise ValueError("first GLB chunk is not JSON")
        document = json.loads(stream.read(json_length).decode("utf-8"))
    return document, total_length


def glb_stats(path):
    model, size = read_glb_json(path)
    primitive_count = sum(
        len(mesh.get("primitives", [])) for mesh in model.get("meshes", [])
    )
    return {
        "bytes": size,
        "meshes": len(model.get("meshes", [])),
        "primitives": primitive_count,
        "materials": len(model.get("materials", [])),
        "textures": len(model.get("textures", [])),
        "images": len(model.get("images", [])),
        "extensions_required": model.get("extensionsRequired", []),
    }


def layer_for(path):
    name = path.name.lower()
    if name == "root.glb":
        return "root"
    prefix = name.split("_", 1)[0]
    return prefix.upper() if prefix.startswith("l") else "other"


def settings_for(layer, material_count, preset):
    # Farther HLOD levels can tolerate more geometry reduction.  Atlas counts
    # are derived from material counts so small L1 files are not inflated by a
    # fixed set of mostly empty textures.
    if preset == "balanced":
        if layer == "root":
            return {
                "ratio": 1.00,
                "atlas_size": 2048,
                "atlases": min(32, max(1, math.ceil(material_count / 128))),
                "jpeg_quality": 85,
            }
        if layer == "L0":
            return {
                "ratio": 1.00,
                "atlas_size": 1024,
                "atlases": min(64, max(1, math.ceil(material_count / 32))),
                "jpeg_quality": 85,
            }
        return {
            "ratio": 1.00,
            "atlas_size": 512,
            "atlases": min(64, max(1, math.ceil(material_count / 8))),
            "jpeg_quality": 85,
        }
    if layer == "root":
        return {
            "ratio": 0.05,
            "atlas_size": 512,
            "atlases": min(16, max(1, material_count)),
            "jpeg_quality": 70,
        }
    if layer == "L0":
        return {
            "ratio": 0.10,
            "atlas_size": 256,
            "atlases": min(64, max(1, math.ceil(material_count / 64))),
            "jpeg_quality": 70,
        }
    return {
        "ratio": 0.20,
        "atlas_size": 128,
        "atlases": min(64, max(1, math.ceil(material_count / 16))),
        "jpeg_quality": 70,
    }


def optimize_one(source, output_dir, optimizer, preset, accept_larger):
    started = time.monotonic()
    before = glb_stats(source)
    layer = layer_for(source)
    settings = settings_for(layer, before["materials"], preset)
    destination = output_dir / source.name
    temporary = destination.with_suffix(destination.suffix + ".optimizing")
    command = [
        str(optimizer),
        str(source),
        str(temporary),
        "--atlases",
        str(settings["atlases"]),
        "--atlas-size",
        str(settings["atlas_size"]),
        "--ratio",
        str(settings["ratio"]),
        "--jpeg-quality",
        str(settings["jpeg_quality"]),
    ]
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"optimizer exited with {completed.returncode}:\n{completed.stdout[-4000:]}"
            )
        after = glb_stats(temporary)
        if after["primitives"] <= 0:
            raise ValueError("optimized GLB contains no primitives")
        if "KHR_draco_mesh_compression" not in after["extensions_required"]:
            raise ValueError("optimized GLB does not require Draco")

        if accept_larger or after["bytes"] < before["bytes"]:
            temporary.replace(destination)
            action = "optimized" if after["bytes"] < before["bytes"] else "optimized_seam_safe_larger"
        else:
            temporary.unlink()
            shutil.copy2(source, destination)
            after = before.copy()
            action = "kept_original_not_smaller"
        return {
            "file": source.name,
            "layer": layer,
            "status": "ok",
            "action": action,
            "settings": settings,
            "before": before,
            "after": after,
            "seconds": round(time.monotonic() - started, 3),
        }
    except Exception as exc:
        if temporary.exists():
            temporary.unlink()
        if destination.exists():
            destination.unlink()
        return {
            "file": source.name,
            "layer": layer,
            "status": "failed",
            "action": "none",
            "settings": settings,
            "before": before,
            "error": str(exc),
            "seconds": round(time.monotonic() - started, 3),
        }


def print_result(index, total, item):
    if item["status"] == "ok":
        before = item["before"]["bytes"]
        after = item["after"]["bytes"]
        percent = 100.0 * after / max(1, before)
        print(
            f"[{index:3d}/{total}] {item['file']}: {item['action']}, "
            f"{before / 1048576:.2f} -> {after / 1048576:.2f} MiB "
            f"({percent:.1f}%)",
            flush=True,
        )
    else:
        print(
            f"[{index:3d}/{total}] {item['file']}: FAILED: {item['error']}",
            file=sys.stderr,
            flush=True,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_hlod", type=Path)
    parser.add_argument("output_hlod", type=Path)
    parser.add_argument("--optimizer", type=Path, required=True)
    parser.add_argument(
        "--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1))
    )
    parser.add_argument(
        "--preset",
        choices=("balanced", "compact"),
        default="balanced",
        help="balanced preserves substantially more geometry and texture detail",
    )
    parser.add_argument(
        "--accept-larger",
        action="store_true",
        help="keep validated optimized GLBs even when high precision makes them larger",
    )
    args = parser.parse_args()

    source_dir = args.input_hlod.resolve()
    output_dir = args.output_hlod.resolve()
    optimizer = args.optimizer.resolve()
    if not source_dir.is_dir():
        parser.error(f"input HLOD directory not found: {source_dir}")
    if output_dir.exists():
        parser.error(f"output HLOD directory already exists: {output_dir}")
    if not optimizer.is_file():
        parser.error(f"optimizer executable not found: {optimizer}")
    if args.jobs < 1 or args.jobs > 32:
        parser.error("--jobs must be in 1..32")

    sources = sorted(
        source_dir.glob("*.glb"),
        key=lambda p: (0 if p.name.lower() == "root.glb" else 1, p.name.lower()),
    )
    if not sources:
        parser.error(f"no GLB files found in: {source_dir}")
    output_dir.mkdir(parents=True)
    report_path = output_dir.parent / f"{output_dir.name}_optimization_report.json"
    started = time.monotonic()
    results = []

    # Process root alone because it dominates memory use, then parallelize the
    # much smaller L0/L1 files.
    root_sources = [p for p in sources if p.name.lower() == "root.glb"]
    remaining = [p for p in sources if p.name.lower() != "root.glb"]
    completed_count = 0
    for source in root_sources:
        item = optimize_one(
            source, output_dir, optimizer, args.preset, args.accept_larger
        )
        results.append(item)
        completed_count += 1
        print_result(completed_count, len(sources), item)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(
                optimize_one,
                source,
                output_dir,
                optimizer,
                args.preset,
                args.accept_larger,
            ): source
            for source in remaining
        }
        for future in concurrent.futures.as_completed(futures):
            item = future.result()
            results.append(item)
            completed_count += 1
            print_result(completed_count, len(sources), item)

    results.sort(key=lambda item: item["file"].lower())
    failures = [item for item in results if item["status"] != "ok"]
    before_bytes = sum(item["before"]["bytes"] for item in results)
    after_bytes = sum(
        item.get("after", item["before"])["bytes"] for item in results
    )
    report = {
        "source": str(source_dir),
        "output": str(output_dir),
        "optimizer": str(optimizer),
        "preset": args.preset,
        "accept_larger": args.accept_larger,
        "file_count": len(results),
        "failure_count": len(failures),
        "before_bytes": before_bytes,
        "after_bytes": after_bytes,
        "saved_bytes": before_bytes - after_bytes,
        "after_percent": round(100.0 * after_bytes / max(1, before_bytes), 3),
        "seconds": round(time.monotonic() - started, 3),
        "files": results,
    }
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"Report: {report_path}")
    print(
        f"Total: {before_bytes / 1048576:.2f} -> {after_bytes / 1048576:.2f} MiB "
        f"({report['after_percent']:.1f}%), failures={len(failures)}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
