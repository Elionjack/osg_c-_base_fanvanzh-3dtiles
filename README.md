# osgb2b3dm — OSGB 到 3D Tiles 转换器

将 OpenSceneGraph Binary (OSGB) 格式的倾斜摄影/城市三维瓦片数据转换为 Cesium 3D Tiles (B3DM + tileset.json) 格式。

基于 [fanvanzh/3dtiles](https://github.com/fanvanzh/3dtiles) 的 Rust 版本改写为 C++17。

## 快速开始

### 环境要求

- Windows 10+ / Linux (x64)
- Visual Studio 2022 (Windows) 或 GCC 9+ (Linux)
- CMake 3.15+
- [vcpkg](https://github.com/microsoft/vcpkg) 包管理器

### 依赖

| 库 | 用途 | 必需 |
|----|------|------|
| OpenSceneGraph 3.6+ | OSGB 文件读取 | ✅ |
| GDAL 3.x | 坐标参考系 (SRS) 变换 | ✅ |
| Eigen3 | SVD 最小二乘坐标修正 | ✅ |
| glm | 矩阵/向量运算 | ✅ |
| tinygltf | glTF 2.0 / GLB 序列化 | ✅ |
| GeographicLib | 大地水准面高度计算 | ✅ |
| nlohmann_json | JSON 解析 | ✅ |
| basisu | KTX2 纹理压缩 | ❌ 可选 |
| Draco | 网格压缩 | ❌ 可选 |
| meshoptimizer | 网格简化 | ❌ 可选 |
| stb | 纹理 JPEG 编码 | ❌ 可选 |

### 构建

```bash
# 1. 通过 vcpkg 安装依赖
vcpkg install osg gdal glm eigen3 tinygltf geographiclib nlohmann-json stb

# 可选
vcpkg install draco meshoptimizer basisu

# 2. 配置 CMake（Windows）
cmake -B out/build/x64-Debug -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug

# 3. 编译
cmake --build out/build/x64-Debug --config Debug

# 4. 运行程序前准备运行时依赖
#    程序需要 osgPlugins、GDAL、PROJ 数据文件。
#    将以下目录复制到可执行文件同级：
#    - vcpkg/packages/osg_x64-windows/debug/lib/osgPlugins-3.6.5/
#    - vcpkg/packages/gdal_x64-windows/debug/share/gdal/
#    - vcpkg/packages/proj_x64-windows/debug/share/proj/
```

### Linux 构建

```bash
# 安装依赖
sudo apt install libosg-dev libgdal-dev libeigen3-dev libglm-dev libgeographiclib-dev

# vcpkg 安装其余依赖
vcpkg install tinygltf nlohmann-json stb

# 构建
cmake -B out/build -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build out/build -j$(nproc)
```

## 使用

### 输入数据要求

输入目录结构：

```
input_dir/
├── metadata.xml          ← 坐标参考系定义
└── Data/
    ├── Tile_-051_+050/
    │   ├── Tile_-051_+050.osgb
    │   ├── Tile_-051_+050_L14_0.osgb
    │   └── ...
    └── Tile_-052_+050/
        └── ...
```

### metadata.xml 格式

```xml
<ModelMetadata version="1.0">
    <SRS>ENU:30.0,120.0</SRS>             <!-- 或 EPSG:4548 或 WKT 字符串 -->
    <SRSOrigin>0.0,0.0,0.0</SRSOrigin>    <!-- 源坐标原点 -->
</ModelMetadata>
```

支持的 SRS 类型：

| SRS 格式 | 示例 | 说明 |
|----------|------|------|
| `ENU:lat,lon` | `ENU:30.0,120.0` | 东-北-天局部坐标系 |
| `EPSG:code` | `EPSG:4548` | EPSG 投影代码 |
| WKT 字符串 | `PROJCS["CGCS2000",...]` | OGC WKT 格式 |

### 配置方式

在 `src/main.cpp` 中直接修改硬编码配置：

```cpp
static const HardcodedConfig g_config = {
    /* input_dir         */ R"(E:\learning\data\1)",
    /* output_dir        */ R"(E:\learning\data\output\tiles)",
    /* config_json       */ "",                      // JSON: {"x":lon,"y":lat,"max_lvl":20}
    /* geoid_model       */ "none",                  // none / egm84 / egm96 / egm2008
    /* geoid_path        */ "",
    /* texture_compress  */ false,                   // 需 basisu
    /* meshopt           */ false,                   // 需 meshoptimizer
    /* draco             */ false,                   // 需 Draco
    /* unlit             */ true,                    // KHR_materials_unlit
    /* override_lon/lat  */ 0.0,
    /* has_override_alt  */ false,
};
```

设置 `USE_HARDCODED_CONFIG` 为 `0` 可切换为命令行模式：

```bash
./osgb_converter -i ./input -o ./output -c '{"x":120.0,"y":30.0,"max_lvl":20}' --enable-unlit
```

### 输出结构

```
output_dir/
├── tileset.json              ← 3D Tiles 根 tileset
└── Data/
    └── Tile_-051_+050/
        ├── Tile_-051_+050.b3dm
        ├── Tile_-051_+050_L14_0.b3dm
        └── ...
```

## 架构

### 数据流

```
metadata.xml ──→ osgb_converter.cpp（主控）
                     │
    ┌────────────────┼────────────────┐
    │  坐标系统初始化   │  遍历 Data/ 瓦片   │
    │  (ENU/EPSG/WKT) │  (*.osgb)        │
    └────────────────┼────────────────┘
                     │
                     ▼
         osg_gltf_converter.cpp
                     │
    ┌────────────────┼────────────────┐
    │  osgDB::readNodeFiles()         │
    │  InfoVisitor 遍历场景图           │
    │    ├── 收集 Geometry（顶点/法线/UV/索引）│
    │    ├── 收集 Texture（StateSet）      │
    │    └── SVD 坐标修正（ToLocalENU）    │
    │                                  │
    │  tinygltf Model 构建               │
    │    ├── buffer → 顶点/索引/纹理数据    │
    │    ├── accessors / bufferViews   │
    │    ├── meshes / primitives       │
    │    ├── materials / textures      │
    │    └── model.buffers ← buffer ← 必须！│
    │                                  │
    │  WriteGltfSceneToStream() → GLB  │
    └──────────────────────────────────┘
                     │
                     ▼
              B3DM 封装 (28B header)
              + Feature Table JSON
              + Batch Table JSON
              + GLB Binary
                     │
                     ▼
              写入 .b3dm 文件
```

### 模块职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 入口：环境配置 (GDAL/PROJ)、参数解析、调用 `convert_osgb()` |
| `osgb_converter.cpp/.h` | 主控：metadata.xml 解析、坐标系统初始化、瓦片遍历、tileset.json 输出 |
| `osg_gltf_converter.cpp/.h` | 核心转换：OSG 场景图→glTF Model→GLB buffer→B3DM buffer |
| `coordinate_system.cpp/.h` | 坐标系统抽象层 (ENU/EPSG/WKT)，`std::variant` 多态 |
| `coordinate_transformer.cpp/.h` | 坐标变换：WGS84↔ECEF↔局部ENU、Z-up↔Y-up 轴转换 |
| `geoid_height.cpp/.h` | GeographicLib 封装，大地水准面高度校正 (EGM84/96/2008) |
| `mesh_processor.cpp/.h` | 可选压缩：KTX2(basisu)、Draco、meshoptimizer 简化 |
| `utils.h` | 日志宏、文件 I/O、二进制序列化 (`put_val`/`alignment_buffer`)、包围盒运算 |

### 关键设计点

1. **坐标修正**（`InfoVisitor::apply`）：对 8 个包围盒角点做 `ToLocalENU` 变换，用 SVD 最小二乘求解最优仿射变换矩阵，批量纠正所有顶点——比逐顶点变换快一个数量级。

2. **硬编码配置**：开发阶段不需要每次传命令行参数，直接修改 `main.cpp` 中的 `g_config` 结构体即可。

3. **`model.buffers` 陷阱**：`tinygltf::Buffer` 写入数据后，**必须** `model.buffers.push_back(buffer)`，否则 GLB 不包含 BIN chunk，Cesium 加载后无模型显示。

## 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-i, --input` | 输入 OSGB 瓦片集目录（含 `Data/` 和 `metadata.xml`） | — |
| `-o, --output` | 输出目录 | — |
| `-c, --config` | JSON 配置：`{"x":lon,"y":lat,"offset":h,"max_lvl":20}` | — |
| `--lon` / `--lat` / `--alt` | 覆盖中心经纬度和高度 | — |
| `--enable-texture-compress` | 启用 KTX2 纹理压缩（需 basisu） | off |
| `--enable-draco` | 启用 Draco 网格压缩 | off |
| `--enable-simplify` | 启用 meshoptimizer 网格简化 | off |
| `--enable-unlit` | 启用 KHR_materials_unlit 扩展 | on |
| `--geoid` | 大地水准面模型：`none`/`egm84`/`egm96`/`egm2008` | none |
| `--geoid-path` | 大地水准面数据文件路径 | 自动 |

## 许可证

基于 [fanvanzh/3dtiles](https://github.com/fanvanzh/3dtiles) 改写，遵循原项目许可证。
