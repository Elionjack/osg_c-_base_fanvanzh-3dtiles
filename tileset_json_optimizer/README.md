# tileset_json_optimizer

对已有 3D Tiles 输出做 JSON 级运行时优化，不重新转换或复制 `Data/*.glb`。

主要功能：

- 用有效子节点包围盒修复 `1e38`、非有限值等无效 `boundingVolume.box`。
- 删除超过阈值且存在替代子节点的 HLOD GLB 引用。
- 将无内容索引节点的 `geometricError` 提升到强制细化值。
- 压平连续的无内容索引层。
- 将优化 JSON 写入独立覆盖目录，并自动重写到原 `Data` 的相对 URI。

先分析：

```powershell
.\build\tileset_json_optimizer\Release\tileset_json_optimizer.exe `
  -i E:\learning\data\output\25 `
  --max-hlod-mb 4 `
  --dry-run
```

生成 JSON 覆盖目录：

```powershell
.\build\tileset_json_optimizer\Release\tileset_json_optimizer.exe `
  -i E:\learning\data\output\25 `
  -o E:\learning\data\output\25_json_optimized `
  --max-hlod-mb 4
```

新目录只包含 JSON。Web 服务必须以
`E:\learning\data\output` 或更高层目录作为静态资源根目录，使优化 JSON
中的 `../25/Data/...` 能访问原 GLB。例如加载：

```text
/25_json_optimized/tileset.json
```

原始 `E:\learning\data\output\25` 不会被修改。

已经自行备份并需要直接替换原 JSON 时：

```powershell
.\build\tileset_json_optimizer\Release\tileset_json_optimizer.exe `
  -i E:\learning\data\output\25 `
  --max-hlod-mb 4 `
  --in-place
```

`--in-place` 只替换 `tileset.json` 和 `subtilesets/*.json`，不会修改或删除
`Data/*.glb`。
