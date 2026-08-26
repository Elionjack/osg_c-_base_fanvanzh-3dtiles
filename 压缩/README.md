# GLB 4 MiB 自动简化压缩工具

`compress-glb.mjs` 专门处理项目产生的 Draco + KTX2 GLB：

- 递归扫描输入目录，只简化大于目标体积的 `.glb`；
- 逐轮降低三角形数量并重新进行 Draco 压缩；
- 简化仍不能达标时，保留全部 primitive 并拆成多个小于上限的 GLB；
- 自动把 3D Tiles 1.1 JSON 中的 `content.uri` 改为 `contents`，让所有分片同时加载；
- 默认原样复制 JSON 和已不超限的文件，保留目录结构。

> 4 MiB = 4,194,304 字节。程序不再为了硬性上限删除 primitive。输出仍使用 `KHR_draco_mesh_compression` 和原 KTX2 纹理。

## 安装

依赖已安装在本目录。如需重新安装：

```powershell
npm install --no-audit --no-fund
```

## 用法

只压缩 `25v2` 的 HLOD 目录，输出到新目录：

```powershell
.\run.cmd `
  'E:\learning\data\output\25v2\Data\HLOD' `
  'E:\learning\data\output\25v2_compressed\Data\HLOD' `
  --limit-mib 4 --split-only
```

压缩完整 tileset 并保留 JSON/小文件：

```powershell
.\run.cmd `
  'E:\learning\data\output\25v2' `
  'E:\learning\data\output\25v2_compressed' `
  --limit-mib 4 --split-only
```

常用参数：

- `--limit-mib N`：目标 MiB，默认 4。
- `--large-only`：仅产生原始大于上限的 GLB，不复制其他文件。
- `--overwrite`：允许覆盖输出目录中的同名文件。
- `--split-only`：不减面，直接把超限 GLB 安全拆分，视觉保真最高。

程序的返回码为 0 表示所有待压缩 GLB 都达到目标；有任何一个文件未达标时返回 1，并在日志中列出失败文件。
