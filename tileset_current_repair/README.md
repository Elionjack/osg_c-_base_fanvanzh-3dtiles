# 已有 Tileset 升级工具

用于把旧版 `--enable-top-reconstruct --split-json` 输出的 JSON 升级到当前转换器规则。

工具只重写 `tileset.json` 和 `subtilesets/*.json`，不会修改或复制 GLB、纹理等内容文件。

需要 Python 3.10 或更高版本。本机 Windows 推荐使用已安装的 Python 3.13：`py -3.13`。

## 处理内容

- 检查全部本地 `content.uri`；删除不存在或零字节的内容引用。
- 缺失内容的父节点由有效子节点替代，空叶节点删除。
- 按当前转换器的空间跨度上限重新计算源 PagedLOD `geometricError`。
- 按 `base_cell_ge × grid_size` 重新计算 HLOD；同一 HLOD level 数值一致。
- 同步外部 subtileset 引用瓦片与目标根瓦片的误差。
- 写入前再次验证所有引用；仍有缺失时拒绝修改。
- `--apply` 模式会先把全部原 JSON 备份到输出目录内的时间戳文件夹。

## 使用

先只读预检：

```powershell
py -3.13 .\tileset_current_repair\repair_tileset.py `
  "E:\learning\data\output\25v8"
```

确认统计结果后正式应用：

```powershell
py -3.13 .\tileset_current_repair\repair_tileset.py `
  "E:\learning\data\output\25v8" `
  --apply
```

当前 `25v8` 使用 `--hlod-branching-factor 16`，因此默认值无需额外传入。其他数据可使用：

```powershell
py -3.13 .\tileset_current_repair\repair_tileset.py OUTPUT `
  --branching-factor 4 `
  --apply
```

成功输出中应满足：

```text
Missing references after repair: 0
HLOD L0: unique=1
HLOD L1: unique=1
```

需要恢复时，将 `_json_backup_before_current_repair_时间戳` 中的 JSON 按原相对路径复制回输出目录。
