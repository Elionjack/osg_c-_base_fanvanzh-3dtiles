# Single-root tileset conversion tool

Converts an old split, multi-level HLOD output into the current `src1.1`
layout:

- one `Data/HLOD/root.glb`;
- root `geometricError` and tileset `geometricError` set to `1000`;
- root children directly reference one external tileset per `Tile_*` directory;
- no intermediate HLOD GLBs or HLOD shard JSONs in the new output.

The source directory is never changed. On the same disk, data files are hard
linked into the new directory, so the conversion does not duplicate their
contents. If hard links are unavailable, the tool falls back to copying.

```powershell
python .\single_root_tileset_tool\convert_v7_to_single_root.py `
  "E:\learning\data\output\25v7" `
  "E:\learning\data\output\25v7_single_root"
```

Validate without writing:

```powershell
python .\single_root_tileset_tool\convert_v7_to_single_root.py `
  "E:\learning\data\output\25v7" `
  "E:\learning\data\output\25v7_single_root" `
  --dry-run
```
