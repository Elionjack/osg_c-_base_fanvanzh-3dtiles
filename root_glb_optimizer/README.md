# ROOT GLB optimizer

The optimizer is both a standalone post-processor and the shared HLOD
finalization implementation used by `src1.1`. The standalone executable
refuses to overwrite its input.

It decodes Draco geometry and KTX2 textures, bakes material colors into a
small number of JPEG atlases, merges geometry by atlas, simplifies the merged
geometry, and encodes it with Draco again.

`src1.1` calls the same implementation in memory for every generated HLOD
before writing it. This is intentionally a two-stage pipeline: the original
per-geometry conversion and simplification runs first, then HLOD textures and
primitives are merged and safely simplified as a whole. Ordinary detail GLBs
are not finalized this way.

Integrated HLOD policy:

- `root.glb`: one 1024px JPEG atlas, 65% target, 0.50 absolute error;
- `L0_*.glb`: one 512px JPEG atlas, 70% target, 0.25 absolute error;
- `L1_*.glb` and deeper: one 256px JPEG atlas, 75% target, 0.15 absolute error;
- JPEG quality 80 and Draco POSITION quantization at 20 bits;
- locked topological borders and attribute-aware absolute-error simplification;
- exactly one primitive per emitted HLOD GLB.

The shared decoder accepts KTX2, JPEG, and PNG input textures, so integrated
HLOD finalization does not require `--enable-texture-compress`.

Default target:

- at most 16 primitives, materials, and textures;
- 16 atlases of 512 x 512 pixels;
- 5% merged geometry index target;
- JPEG quality 70.

```powershell
cmake -S root_glb_optimizer -B root_glb_optimizer/build `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build root_glb_optimizer/build --config Release

.\root_glb_optimizer\build\Release\root_glb_optimizer.exe `
  input.root.glb output.root.glb
```

To optimize every `root.glb`, `L0_*.glb`, and `L1_*.glb` in an HLOD
directory without modifying the source, use the batch driver. It applies
layer-specific geometry ratios and atlas sizes, validates every output, and
keeps the original whenever an optimized file would be larger:

```powershell
python .\root_glb_optimizer\batch_optimize_hlod.py `
  E:\path\to\tileset\Data\HLOD `
  E:\path\to\staging\Data\HLOD `
  --optimizer .\root_glb_optimizer\build\root_glb_optimizer.exe `
  --preset balanced `
  --jobs 4
```

Balanced policy (default):

- `root.glb`: no geometry removal, 2048px atlases, JPEG quality 85;
- `L0_*.glb`: no geometry removal, 1024px atlases, JPEG quality 85;
- `L1_*.glb` (and other levels): no geometry removal, 512px atlases,
  JPEG quality 85.

The earlier aggressive policy remains available as `--preset compact`:

- `root.glb`: 5% geometry target, 512px atlases;
- `L0_*.glb`: 10% geometry target, 256px atlases;
- `L1_*.glb` (and other levels): 20% geometry target, 128px atlases.

Atlas counts are chosen from each file's material count. A detailed JSON
report is written next to the output HLOD directory.

Positions are Draco-quantized at 20 bits. This is intentional: root HLOD
meshes can span kilometres, and lower position precision can separate borders
that were coincident before primitives were merged into independent atlas
groups.

Use `--accept-larger` for seam-safe rebuilds. High position precision can make
some GLBs larger than their source; without this flag the batch driver keeps
the original whenever the optimized file is not smaller.

For the recommended selective workflow, process only GLBs larger than 4 MiB
and copy smaller HLODs byte-for-byte:

```powershell
python .\root_glb_optimizer\optimize_large_hlod.py `
  E:\path\to\source\Data\HLOD E:\path\to\staging\Data\HLOD `
  --optimizer .\root_glb_optimizer\build\root_glb_optimizer.exe `
  --threshold-mib 4
```

Large files are rebuilt as one atlas and one primitive with no geometry
simplification. This provides one shared 20-bit Draco position quantization
domain per GLB, while small files remain exactly as generated originally.

To rebuild every HLOD as one primitive with topology-safe simplification and
locked GLB borders, use `optimize_all_hlod_single_primitive.py`. The default
absolute error limits are 0.50 model units for root, 0.25 for L0, and 0.15 for
L1; target index ratios are 65%, 70%, and 75% respectively. The simplifier may
stop before a target when topology or the error limit requires it.
Every GLB uses one primitive and one JPEG atlas (1024px root, 512px L0, 256px
L1). Internal material seams may collapse, while topological GLB borders stay
locked to prevent cracks between independently simplified neighboring files.

Options:

```text
--atlases N       1..64 (default 16)
--atlas-size N    128..4096 (default 512)
--ratio R         0.001..1 (default 0.05)
--jpeg-quality N  1..100 (default 70)
```
