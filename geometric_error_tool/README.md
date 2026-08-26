# geometricError 后处理工具

该工具无需重新转换 OSGB/GLB，即可把现有 3D Tiles JSON 的几何误差按指定
系数重新计算。工具默认让 HLOD 保持系数 `1.55`，普通 PagedLOD 瓦片使用
系数 `2.0`，无需重新生成 GLB。

它会递归处理主 `tileset.json` 和其中引用的外部 tileset JSON，并从叶子向根
重新计算 PagedLOD 与 HLOD。GLB 文件不会读取或修改。

运行环境：Python 3.6 或更高版本，不依赖第三方包。

## 使用

先执行只读预览：

```bash
python geometric_error_tool/adjust_geometric_error.py /path/to/output
```

确认统计结果后正式修改：

```bash
python geometric_error_tool/adjust_geometric_error.py /path/to/output --apply
```

Windows 示例：

```powershell
python .\geometric_error_tool\adjust_geometric_error.py `
  "E:\learning\data\output\25v2"

python .\geometric_error_tool\adjust_geometric_error.py `
  "E:\learning\data\output\25v2" --apply
```

默认普通瓦片系数是 `2.0`、HLOD 系数是 `1.55`，也可以显式指定：

```bash
python geometric_error_tool/adjust_geometric_error.py /path/to/output \
  --factor 2 --hlod-factor 1.55 --apply
```

正式修改前，工具会先在数据集目录下创建带时间戳的备份文件夹，只备份会修改
的 JSON。所有新 JSON 都成功生成后才会替换原文件。

## 注意

- 不带 `--apply` 时只计算和输出统计，不写入任何文件。
- 请传入数据集根目录或根 `tileset.json`。
- 外部 JSON 必须位于数据集根目录内。
- 工具保留无内容索引节点的 `1e12` 强制细化值，同时使用其正常有限误差计算
  父节点，这与 `src1.1` 的生成顺序一致。
- 修改 JSON 后不需要重新生成 GLB。
- `--factor` 只控制普通 PagedLOD 瓦片；`--hlod-factor` 只控制 HLOD。

## 只修改最顶层 root

只查看最顶层 `tileset.geometricError` 和 `root.geometricError` 的修改结果：

```bash
python geometric_error_tool/set_root_geometric_error.py /path/to/output 250
```

确认后正式修改（会自动备份原 `tileset.json`）：

```bash
python geometric_error_tool/set_root_geometric_error.py /path/to/output 250 --apply
```

该命令不会修改瓦片目录及其内部 LOD 的几何误差。
