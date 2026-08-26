# 旧输出对齐当前 src1.1

该工具把旧版 `--enable-top-reconstruct --split-json` 输出的 JSON 拓扑迁移成当前
`src1.1` 的输出形式，复用已有 GLB，不重新转换约 32 GB 模型数据。

## 25v8 与当前 src1.1 的主要区别

- `25v8` 使用 169 个 `HLOD_L2/L3_*.json`，每个文件打包一组源瓦片树；当前程序为每个
  `Tile_x_y` 单独生成 `subtilesets/Tile_x_y.json`，HLOD 节点留在主 `tileset.json`。
- `25v8` 的 HLOD `geometricError` 来源于旧算法；当前程序以最大的源瓦片半跨度为基线，
  从底层 HLOD 到根逐级乘以固定 `1.55`。
- 当前程序删除写入失败的叶内容；失败但仍有有效子节点的源 PagedLOD 父节点保留为
  `geometricError=1e12` 的索引节点；无内容的 HLOD 包装层则提升其子节点。
- GLB、纹理、包围盒和根变换不需要修改。若转换参数或 GLB 生成算法本身发生变化，
  JSON 迁移不能替代重新转换；本工具只对齐当前 JSON 生成规则。

## 使用

先只读预检：

```powershell
py -3.13 .\tileset_match_src1_1\match_current_src1_1.py `
  "E:\learning\data\output\25v8"
```

`25v8` 此前被旧修复脚本处理过。为了恢复被提升掉的失败父节点，应以其原始备份作为输入：

```powershell
py -3.13 .\tileset_match_src1_1\match_current_src1_1.py `
  "E:\learning\data\output\25v8" `
  --baseline-json-dir "E:\learning\data\output\25v8\_json_backup_before_current_repair_20260811_150228"
```

确认 `Missing references after migration: 0` 后正式应用，在上条命令末尾加 `--apply`。
应用前会在目标目录创建 `_json_backup_before_src1_1_match_时间戳`。工具只删除被新
`Tile_*.json` 取代的旧 HLOD JSON；这些文件可从该备份恢复。GLB 不会删除或修改。
