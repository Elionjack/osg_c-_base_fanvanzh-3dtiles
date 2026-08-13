#!/usr/bin/env python3
"""Rebuild every HLOD GLB as one safely simplified primitive."""

import argparse
import json
import struct
import subprocess
import time
from pathlib import Path


def stats(path):
    with path.open("rb") as stream:
        magic, version, total = struct.unpack("<4sII", stream.read(12))
        length, chunk_type = struct.unpack("<II", stream.read(8))
        model = json.loads(stream.read(length).decode("utf-8"))
    if magic != b"glTF" or version != 2 or total != path.stat().st_size:
        raise ValueError("invalid GLB: %s" % path)
    return {
        "bytes": total,
        "indices": sum(a.get("count", 0) for a in model.get("accessors", [])
                       if a.get("type") == "SCALAR"),
        "primitives": sum(len(m.get("primitives", []))
                          for m in model.get("meshes", [])),
    }


def layer(path):
    if path.name.lower() == "root.glb":
        return "root"
    return path.name.split("_", 1)[0].upper()


def settings(path):
    value = layer(path)
    if value == "root":
        return {"ratio": 0.65, "max_error": 0.50, "atlas_size": 1024}
    if value == "L0":
        return {"ratio": 0.70, "max_error": 0.25, "atlas_size": 512}
    return {"ratio": 0.75, "max_error": 0.15, "atlas_size": 256}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_hlod", type=Path)
    parser.add_argument("output_hlod", type=Path)
    parser.add_argument("--optimizer", type=Path, required=True)
    parser.add_argument("--jpeg-quality", type=int, default=80)
    args = parser.parse_args()

    source_dir = args.input_hlod.resolve()
    output_dir = args.output_hlod.resolve()
    optimizer = args.optimizer.resolve()
    if not source_dir.is_dir() or not optimizer.is_file():
        parser.error("input directory or optimizer not found")
    if output_dir.exists():
        parser.error("output directory already exists")
    output_dir.mkdir(parents=True)
    sources = sorted(source_dir.glob("*.glb"), key=lambda p: p.name.lower())
    report = {"source": str(source_dir), "output": str(output_dir),
              "position_bits": 20, "border_locked": True,
              "simplifier": "meshopt_simplifyWithAttributes",
              "permissive_internal_seams": True, "files": []}
    started = time.monotonic()

    for index, source in enumerate(sources, 1):
        before = stats(source)
        target = output_dir / source.name
        option = settings(source)
        command = [str(optimizer), str(source), str(target),
                   "--atlases", "1", "--atlas-size", str(option["atlas_size"]),
                   "--ratio", str(option["ratio"]),
                   "--max-error", str(option["max_error"]),
                   "--jpeg-quality", str(args.jpeg_quality)]
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                universal_newlines=True, encoding="utf-8",
                                errors="replace")
        if result.returncode != 0:
            raise RuntimeError("%s failed:\n%s" % (source.name, result.stdout[-4000:]))
        after = stats(target)
        if after["primitives"] != 1 or after["indices"] <= 0:
            raise ValueError("invalid single-primitive output: %s" % source.name)
        item = {"file": source.name, "layer": layer(source),
                "settings": option, "before": before, "after": after}
        report["files"].append(item)
        print("[%d/%d] %s indices %.1f%%, size %.1f%%" %
              (index, len(sources), source.name,
               100.0 * after["indices"] / before["indices"],
               100.0 * after["bytes"] / before["bytes"]), flush=True)

    report["file_count"] = len(report["files"])
    report["before_bytes"] = sum(x["before"]["bytes"] for x in report["files"])
    report["after_bytes"] = sum(x["after"]["bytes"] for x in report["files"])
    report["before_indices"] = sum(x["before"]["indices"] for x in report["files"])
    report["after_indices"] = sum(x["after"]["indices"] for x in report["files"])
    report["seconds"] = round(time.monotonic() - started, 3)
    report_path = output_dir.parent / (output_dir.name + "_optimization_report.json")
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Report: %s" % report_path)
    print("Total size %.2f -> %.2f MiB; indices %.1f%%" %
          (report["before_bytes"] / 1048576.0,
           report["after_bytes"] / 1048576.0,
           100.0 * report["after_indices"] / report["before_indices"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
