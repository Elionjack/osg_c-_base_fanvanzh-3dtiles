# osgb2b3dm — OSGB 到 3D Tiles 转换器

将 OpenSceneGraph Binary (OSGB) 格式的倾斜摄影/城市三维瓦片数据转换为 Cesium 3D Tiles 格式。

基于 [fanvanzh/3dtiles](https://github.com/fanvanzh/3dtiles) 的 Rust 版本改写为 C++17。

## 项目结构

```
.
├── CMakeLists.txt              # CMake 构建（两个 target）
├── README.md
├── src/                        # 3D Tiles 1.0（输出 .b3dm）
│   ├── main.cpp                # 入口：环境配置、参数解析
│   ├── osgb_converter.cpp/.h   # 主控：metadata.xml 解析、坐标初始化、tileset.json 生成
│   ├── osg_gltf_converter.cpp/.h  # 核心：OSG→glTF→B3DM
│   ├── coordinate_system.cpp/.h   # 坐标系统抽象 (ENU/EPSG/WKT)
│   ├── coordinate_transformer.cpp/.h  # WGS84/ECEF/ENU 变换
│   ├── geoid_height.cpp/.h     # 大地水准面高度校正 (EGM84/96/2008)
│   ├── mesh_processor.cpp/.h   # 可选压缩 (Draco/KTX2/meshopt)
│   └── utils.h                 # 日志、文件 I/O、二进制序列化、包围盒
├── src1.1/                     # 3D Tiles 1.1（输出 .glb + 3DTILES_content_gltf）
│   └── （文件与 src/ 对应，差异见下方说明）
├── out/                        # CMake 构建输出
└── display/                    # CesiumJS 网页查看器（独立 git 仓库）
    ├── server.js               # Node.js 静态文件服务器
    ├── index.html              # 主页面
    ├── index_debug.html        # Debug 诊断页面
    ├── cli.txt                 # 启动命令备忘
    └── Cesium/                 # CesiumJS 1.134 预编译包
```

## 两个版本对比

| | src/ (1.0) | src1.1/ (1.1) |
|---|---|---|
| **tileset.json** | `"asset.version": "1.0"` | `"asset.version": "1.1"` + `extensionsUsed`/`extensionsRequired` |
| **内容格式** | B3DM 封装 (.b3dm) | 原始 glTF (.glb) |
| **扩展** | 无 | `3DTILES_content_gltf` |
| **Feature/Batch Table** | 有（28 字节头 + JSON） | 无（直接 GLB） |
| **构建目标** | `osgb_converter` | `osgb_converter_1_1` |

核心差异：3D Tiles 1.1 原生支持 glTF 内容，不需要 B3DM 包裹。src1.1 调用 `osgb2glb_buf()` 直接输出 GLB，省去了 28 字节头、Feature Table、Batch Table。

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
| basisu | KTX2 纹理压缩 (ETC1S) | ✅ |
| Draco | 网格顶点/索引压缩 | ✅ |
| meshoptimizer | 网格简化 + EXT_meshopt_compression | ✅ |
| stb | 纹理 JPEG 编码 | ✅ |

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

# 3. 编译（两个 target 一起编）
cmake --build out/build/x64-Debug

# 产物：
#   out/build/x64-Debug/osgb_converter.exe       ← 1.0 版
#   out/build/x64-Debug/osgb_converter_1_1.exe   ← 1.1 版

# 或只编其中一个：
cmake --build out/build/x64-Debug --target osgb_converter_1_1
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

**默认命令行模式**（`USE_HARDCODED_CONFIG = 0`）。两种 target 都支持：

```bash
# 1.0 版（输出 .b3dm）
./osgb_converter -i E:\learning\data\1 -o E:\learning\data\output\my_test

# 1.1 版（输出 .glb + 3DTILES_content_gltf）
./osgb_converter_1_1 -i E:\learning\data\1 -o E:\learning\data\output\my_test_1_1

# 启用所有压缩 + Root 瓦片重建
./osgb_converter_1_1 -i E:\learning\data\1 -o E:\learning\data\output\all_on \
    --enable-simplify --enable-draco --enable-texture-compress \
    --enable-top-reconstruct --simplify-ratio 0.5 \
    --draco-pos-bits 11 --draco-normal-bits 10 --draco-uv-bits 12
```

如需恢复硬编码配置，修改 `src/main.cpp` 或 `src1.1/main.cpp`：

```cpp
#define USE_HARDCODED_CONFIG  1   // 改回 1，然后修改下方的 g_config

static const HardcodedConfig g_config = {
    /* input_dir           */ R"(E:\learning\data\1)",
    /* output_dir          */ R"(E:\learning\data\output\OSG_CJIAJIAbase3dtiles)",
    /* config_json         */ "",
    /* geoid_model         */ "none",
    /* geoid_path          */ "",
    /* texture_compress    */ false,
    /* meshopt             */ false,
    /* draco               */ false,
    /* unlit               */ true,
    /* top_reconstruct     */ false,
    /* top_texture_max_size */ 512,
    /* simplify_ratio      */ 0.5,
    /* draco_pos_bits      */ 11,
    /* draco_normal_bits   */ 10,
    /* draco_uv_bits       */ 12,
    /* enable_parallel     */ true,
    /* override_lon        */ 0.0,
    /* override_lat        */ 0.0,
    /* override_alt        */ 0.0,
    /* has_override_alt    */ false,
};
```

### 输出结构

```
output_dir/
├── tileset.json              ← 3D Tiles 根 tileset
└── Data/
    └── Tile_-051_+050/
        ├── Tile_-051_+050.b3dm    ← 1.0 输出
        ├── Tile_-051_+050.glb     ← 1.1 输出（原始 glTF）
        └── ...
```

## 3D Tiles Viewer（display/）

`display/` 是一个独立的 Node.js + CesiumJS 网页服务器，用于在浏览器中查看转换结果。同时支持 1.0 和 1.1。

### 使用

```bash
cd display
npm install   # 如果没有 node，去 https://nodejs.org 下载安装

# 启动（默认加载 1.1 输出）
node server.js

# 或指定其他模型目录
node server.js E:/learning/data/output/OSG_CJIAJIAbase3dtiles      # 1.0
node server.js E:/learning/data/output/OSG_CJIAJIAbase3dtiles_1_1  # 1.1
```

浏览器打开 `http://localhost:8080`。

Debug 页面：`http://localhost:8080/index_debug.html`（显示 tile 加载统计、包围盒等）。

参数：`?model=/tiles/tileset.json` 可指定 tileset 路径。

### 技术栈

- **CesiumJS 1.134** — 预编译包，Cesium/ 目录下
- **Node.js** 内置模块 — 零 npm 依赖
- CesiumJS 自动识别 `tileset.json` 中的 `asset.version`，同时支持 1.0 (.b3dm) 和 1.1 (.glb + `3DTILES_content_gltf`)

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
          ┌──────────┴──────────┐
          ▼                     ▼
    1.0: B3DM 封装          1.1: 直接写 GLB
    (28B header + FT + BT)  (.glb 文件)
    → 写入 .b3dm            + 3DTILES_content_gltf
          │                     │
          └──────────┬──────────┘
                     ▼
              写入 tileset.json
```

### 模块职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 入口：环境配置 (GDAL/PROJ)、参数解析、调用 `convert_osgb()` |
| `osgb_converter.cpp/.h` | 主控：metadata.xml 解析、坐标系统初始化、瓦片遍历、tileset.json 输出 |
| `osg_gltf_converter.cpp/.h` | 核心转换：OSG 场景图→glTF Model→GLB buffer→B3DM buffer（1.0）/ GLB file（1.1） |
| `coordinate_system.cpp/.h` | 坐标系统抽象层 (ENU/EPSG/WKT)，`std::variant` 多态 |
| `coordinate_transformer.cpp/.h` | 坐标变换：WGS84↔ECEF↔局部ENU、Z-up↔Y-up 轴转换 |
| `geoid_height.cpp/.h` | GeographicLib 封装，大地水准面高度校正 (EGM84/96/2008) |
| `mesh_processor.cpp/.h` | 可选压缩：KTX2(basisu)、Draco、meshoptimizer 简化 |
| `utils.h` | 日志宏、文件 I/O、二进制序列化 (`put_val`/`alignment_buffer`)、包围盒运算 |

### 关键设计点

1. **坐标修正**（`InfoVisitor::apply`）：对 8 个包围盒角点做 `ToLocalENU` 变换，用 SVD 最小二乘求解最优仿射变换矩阵，批量纠正所有顶点——比逐顶点变换快一个数量级。

2. **GDAL/PROJ 路径自动检测**：`setup_environment()` 通过 `VCPKG_ROOT` 环境变量自动定位 vcpkg 安装的 GDAL 和 PROJ 数据文件（proj.db、epsg.wkt），fallback 到 exe 同目录。解决 WKT SRS 解析需要 proj.db 的问题。

3. **OSG 插件路径修复**：OSG Registry 构造函数在 DLL 初始化时执行，早于 `main()`，无法读取环境变量。`log_osg_plugin_info()` 强制将 `OSG_LIBRARY_PATH` 注入 Registry 搜索路径列表，确保 `.osgb` 插件能被加载。

4. **网格处理管线**（`write_osgGeometry`）：三步流水线：
   - Step 1: meshopt 简化（去重、缓存优化、减面 target_ratio=0.5）
   - Step 2: meshopt 流压缩（仅当 Draco 未启用时，`meshopt_encodeVertexBuffer` / `meshopt_encodeIndexBuffer`）或 Draco 压缩
   - Step 3: 写入 glTF primitive + 对应扩展（`EXT_meshopt_compression` 或 `KHR_draco_mesh_compression`）
   - 两种压缩互斥：同时开启时 meshopt 只简化、Draco 负责压缩

5. **纹理处理**：`process_texture()` 支持 KTX2（Basis Universal ETC1S）和 JPEG fallback，统一通过 `mesh_processor.cpp` 调用。

6. **`model.buffers` 陷阱**：`tinygltf::Buffer` 写入数据后，**必须** `model.buffers.push_back(buffer)`，否则 GLB 不包含 BIN chunk，Cesium 加载后无模型显示。

7. **CMakeLists.txt 重构**：公共依赖提取为 `osgb_converter_deps` INTERFACE 库，通过 `configure_osgb_target()` CMake 函数消除两个 target 的重复配置。新增 target 只需 3 行：`set(SOURCES)` → `add_executable` → `configure_osgb_target`。

### 1.0 到 1.1 代码差异

src/ 和 src1.1/ 现在共享完整的网格处理管线。核心差异仅在于输出格式：

| 方面 | src/ (1.0) | src1.1/ (1.1) |
|------|-----------|---------------|
| tileset.json version | `"1.0"` | `"1.1"` + `extensionsUsed`/`extensionsRequired` |
| 内容格式 | B3DM (.b3dm) | 原始 glTF (.glb) |
| 内容扩展 | 无 | `3DTILES_content_gltf` |
| Feature/Batch Table | 有 | 无 |
| 构建目标 | `osgb_converter` | `osgb_converter_1_1` |

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
| `--enable-top-reconstruct` | 合并最粗 LOD 瓦片生成 root.glb 概览 | off |
| `--no-parallel` | 禁用多线程 tile 转换（默认开启并行） | off（即默认并行） |
| `--top-texture-max-size` | Root GLB 纹理最大尺寸（0=不限制） | 512 |
| `--simplify-ratio` | Meshopt 简化目标比例（1.0=不简化） | 0.5 |
| `--draco-pos-bits` | Draco 位置量化位数 | 11 |
| `--draco-normal-bits` | Draco 法向量化位数 | 10 |
| `--draco-uv-bits` | Draco UV 量化位数 | 12 |
| `--geoid` | 大地水准面模型：`none`/`egm84`/`egm96`/`egm2008` | none |
| `--geoid-path` | 大地水准面数据文件路径 | 自动 |

## 许可证

基于 [fanvanzh/3dtiles](https://github.com/fanvanzh/3dtiles) 改写，遵循原项目许可证。

## 仓库

| 目录 | 仓库 | 远程 |
|------|------|------|
| `osg_cjiajia_base3dtiles/` | 转换器主仓库 | `github.com/Elionjack/osg_c-_base_fanvanzh-3dtiles` |
| `display/` | CesiumJS 查看器（独立仓库） | 本地，未推送 |

### 转换器提交历史

```
61df8e7 更新 src1.1: osg_gltf_converter 增强、osgb_converter 重构、mesh_processor 优化
        - osg_gltf_converter 新增 osgb2glb_buf_from_node()（预加载节点转换，避免 OSG 全局锁）
        - osg_gltf_converter 新增 compute_tile_output()（计算/IO 分离，提升并行吞吐）
        - osg_gltf_converter 新增 build_merged_root_glb()（合并最粗 LOD 生成 root.glb）
        - osgb_converter 重构为 3 阶段管线（并行建树 → 块级并行转换 → 串行聚合）
        - mesh_processor 移除 basisu cFlagThreaded（外层并行时内部单线程，避免线程爆炸）
2ff7b33 Update README: document 3-phase pipeline, --no-parallel, and recent fixes
04a12af Refactor pipeline to 3-phase: parallel tree build → tile-level parallel conversion → serial aggregation
        - 三阶段管线：Phase 1 并行建树 → Phase 2 全 tile 并行转换 → Phase 3 串行聚合(bbox/ge/JSON)
        - convert_one_tile() 始终从磁盘加载，避免 cached_node 几何体污染
        - build_merged_root_glb 修复：loaded_nodes 防悬空指针、encoded_hashes 分离纹理去重
        - 新增 --no-parallel CLI 参数、FlatTile 结构体、collect_flat_tiles()

```

### 并行处理性能优化 (最新)

基于实测性能分析，针对并行处理进行了多项优化：

**已修复的瓶颈：**

| 问题 | 修复 | 文件 |
|------|------|------|
| Phase 2 重复调 `osgDB::readNodeFiles()`，OSG 全局锁串行化 | `convert_one_tile_from_cached()` 使用 Phase 1 的 `cached_node` 加载节点 | `osg_gltf_converter.cpp` |
| `basisu::cFlagThreaded` 导致 N×M 线程爆炸 | 移除 `cFlagThreaded`，外层并行时内部单线程 | `mesh_processor.cpp` |
| 一个 tile 一个 `std::async`，线程调度开销过大 | 按 16 个 tile 分 chunk，一个 chunk 一个 `std::async` | `osgb_converter.cpp` |
| `sem.acquire()` 在主线程阻塞任务提交 | 移到 async lambda 内部 | `osgb_converter.cpp` |
| `--no-parallel` 只关闭 Phase 2，Phase 1 仍并行 | Phase 1 也遵循 `--no-parallel` | `osgb_converter.cpp` |
| 并行计算和文件写入纠缠导致写 I/O 串行化 | 计算(C)和写入(I/O)分离：并行计算 → sem.release → `write_mutex` 保护串行写入 | `osgb_converter.cpp` |

**性能结论（i9-12900H, 1925 tiles）：**

| 场景 | 推荐方式 | 耗时 |
|------|----------|------|
| 不开 KTX2 纹理压缩 | **`--no-parallel` 串行** | ~240s（并行 727s 更慢） |
| 开 KTX2 纹理压缩 | **默认并行** | 并行远快于串行 |

- **小 tile 不开 KTX2**：每个 tile 计算轻（~0.1s），6 线程 CPU 并行开销（L3 缓存/内存带宽竞争）> 计算收益，串行更快
- **开 KTX2**：纹理压缩极重（数秒/tile），计算开销 >> 线程开销，并行大幅加速

`--no-parallel` 现在完全禁用 Phase 1 + Phase 2 的并行，Phase 3 始终串行。
