#!/usr/bin/env python3
"""Set only the top-level tileset and root geometricError values."""

import argparse
import datetime
import json
import math
import os
import shutil
import sys
import tempfile
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Set only tileset.geometricError and root.geometricError."
    )
    parser.add_argument("input", type=Path, help="dataset directory or tileset.json")
    parser.add_argument("value", type=float, help="new top-level geometricError")
    parser.add_argument(
        "--apply", action="store_true", help="write the change; otherwise dry-run only"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if not math.isfinite(args.value) or args.value < 0.0:
        print("ERROR: value must be a finite non-negative number", file=sys.stderr)
        return 2

    tileset = args.input / "tileset.json" if args.input.is_dir() else args.input
    if not tileset.is_file():
        print("ERROR: tileset.json not found: {}".format(tileset), file=sys.stderr)
        return 2

    try:
        with tileset.open("r", encoding="utf-8-sig") as stream:
            document = json.load(stream)
        root = document.get("root")
        if not isinstance(document, dict) or not isinstance(root, dict):
            raise ValueError("not a valid 3D Tiles tileset document")

        old_tileset_ge = document.get("geometricError")
        old_root_ge = root.get("geometricError")
        print("tileset geometricError: {} -> {}".format(old_tileset_ge, args.value))
        print("root geometricError: {} -> {}".format(old_root_ge, args.value))

        if not args.apply:
            print("DRY RUN: no files were written; add --apply to commit")
            return 0

        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_dir = tileset.parent / ("root_geometric_error_backup_" + timestamp)
        backup_dir.mkdir()
        shutil.copy2(str(tileset), str(backup_dir / tileset.name))

        document["geometricError"] = args.value
        root["geometricError"] = args.value
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".tileset_root_ge_", suffix=".json", dir=str(tileset.parent)
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
                json.dump(document, stream, ensure_ascii=False, separators=(",", ":"))
            os.replace(temporary_name, str(tileset))
        except BaseException:
            if os.path.exists(temporary_name):
                os.unlink(temporary_name)
            raise

        print("APPLIED")
        print("backup: {}".format(backup_dir / tileset.name))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("ERROR: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
