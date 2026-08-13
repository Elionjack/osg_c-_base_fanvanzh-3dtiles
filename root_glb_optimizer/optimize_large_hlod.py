#!/usr/bin/env python3
"""Selectively optimize only HLOD GLBs larger than a size threshold.

Small files are copied byte-for-byte. Large files are rebuilt as one atlas,
one primitive, without geometry simplification. This keeps every border in a
single Draco quantization domain and avoids the cracks caused by independently
quantized output primitives.
"""

import argparse
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path


def glb_stats(path):
    with path.open("rb") as stream:
        magic, version, total = struct.unpack("<4sII", stream.read(12))
        json_length, json_type = struct.unpack("<II", stream.read(8))
        model = json.loads(stream.read(json_length).decode("utf-8"))
    if magic != b"glTF" or version != 2 or total != path.stat().st_size:
        raise ValueError("invalid GLB: %s" % path)
    return {
        "bytes": total,
        "indices": sum(
            accessor.get("count", 0)
            for accessor in model.get("accessors", [])
            if accessor.get("type") == "SCALAR"
        ),
        "primitives": sum(
            len(mesh.get("primitives", [])) for mesh in model.get("meshes", [])
        ),
        "materials": len(model.get("materials", [])),
        "textures": len(model.get("textures", [])),
    }


def atlas_size(path):
    # Root covers the whole tileset, so retain a larger atlas. Other large HLOD
    # files cover much smaller areas and use a 1024px atlas.
    return 2048 if path.name.lower() == "root.glb" else 1024


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_hlod", type=Path)
    parser.add_argument("output_hlod", type=Path)
    parser.add_argument("--optimizer", type=Path, required=True)
    parser.add_argument("--threshold-mib", type=float, default=4.0)
    parser.add_argument("--jpeg-quality", type=int, default=80)
    args = parser.parse_args()

    source_dir = args.input_hlod.resolve()
    output_dir = args.output_hlod.resolve()
    optimizer = args.optimizer.resolve()
    if not source_dir.is_dir():
        parser.error("input HLOD directory not found")
    if output_dir.exists():
        parser.error("output HLOD directory already exists")
    if not optimizer.is_file():
        parser.error("optimizer executable not found")
    if args.threshold_mib <= 0:
        parser.error("--threshold-mib must be positive")

    threshold = int(args.threshold_mib * 1024 * 1024)
    sources = sorted(source_dir.glob("*.glb"), key=lambda path: path.name.lower())
    output_dir.mkdir(parents=True)
    report = {
        "source": str(source_dir),
        "output": str(output_dir),
        "threshold_mib": args.threshold_mib,
        "position_bits": 20,
        "geometry_ratio": 1.0,
        "large_file_layout": "one atlas / one primitive",
        "files": [],
    }
    started = time.monotonic()

    for index, source in enumerate(sources, 1):
        before = glb_stats(source)
        destination = output_dir / source.name
        if before["bytes"] <= threshold:
            shutil.copy2(source, destination)
            after = before.copy()
            action = "copied_original_at_or_below_threshold"
            settings = None
        else:
            size = atlas_size(source)
            command = [
                str(optimizer), str(source), str(destination),
                "--atlases", "1", "--atlas-size", str(size),
                "--ratio", "1.0", "--jpeg-quality", str(args.jpeg_quality),
            ]
            completed = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                universal_newlines=True, encoding="utf-8", errors="replace"
            )
            if completed.returncode != 0:
                raise RuntimeError(
                    "%s failed:\n%s" % (source.name, completed.stdout[-4000:])
                )
            after = glb_stats(destination)
            if after["indices"] != before["indices"]:
                raise ValueError("index count changed for %s" % source.name)
            if after["primitives"] != 1:
                raise ValueError("large output is not one primitive: %s" % source.name)
            action = "optimized_large_single_primitive"
            settings = {
                "atlases": 1,
                "atlas_size": size,
                "ratio": 1.0,
                "jpeg_quality": args.jpeg_quality,
                "position_bits": 20,
            }
        report["files"].append({
            "file": source.name,
            "action": action,
            "settings": settings,
            "before": before,
            "after": after,
        })
        if before["bytes"] > threshold:
            print(
                "[%d/%d] %s: %.2f -> %.2f MiB, indices=%d, primitives=%d"
                % (index, len(sources), source.name,
                   before["bytes"] / 1048576.0, after["bytes"] / 1048576.0,
                   after["indices"], after["primitives"]),
                flush=True,
            )

    report["file_count"] = len(report["files"])
    report["optimized_count"] = sum(
        item["action"] == "optimized_large_single_primitive"
        for item in report["files"]
    )
    report["before_bytes"] = sum(item["before"]["bytes"] for item in report["files"])
    report["after_bytes"] = sum(item["after"]["bytes"] for item in report["files"])
    report["saved_bytes"] = report["before_bytes"] - report["after_bytes"]
    report["after_percent"] = round(
        100.0 * report["after_bytes"] / report["before_bytes"], 3
    )
    report["seconds"] = round(time.monotonic() - started, 3)
    report_path = output_dir.parent / (output_dir.name + "_optimization_report.json")
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Report: %s" % report_path)
    print(
        "Total HLOD: %.2f -> %.2f MiB (%.1f%%), optimized=%d"
        % (report["before_bytes"] / 1048576.0,
           report["after_bytes"] / 1048576.0,
           report["after_percent"], report["optimized_count"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
