# Helios CMake Build System

## 简介

Helios项目使用CMake构建系统，支持Windows和macOS平台。

## 目录结构

```
Helios/
├── CMakeLists.txt              # 根CMake配置文件
├── cmake/
│   └── HeliosConfig.h.in         # 配置头文件模板
├── Kernel/
│   └── CMakeLists.txt          # 核心库配置
├── Editor/
│   └── CMakeLists.txt          # 编辑器应用配置
├── ModelEditor/
│   └── CMakeLists.txt          # 模型编辑器配置
├── Libraries/ThirdParty/
│   └── CMakeLists.txt          # 第三方库配置
├── Samples/
│   ├── Sample_SkyBox/
│   │   └── CMakeLists.txt
│   └── Sample_CSM/
│       └── CMakeLists.txt
├── Win-GenProj-CMake.bat       # Windows项目生成脚本
└── Mac-GenProj-CMake.sh        # macOS项目生成脚本
```

## 构建要求

### Windows
- CMake 3.20或更高版本
- Visual Studio 2019或2022（推荐）
- 或者Ninja构建系统
- Vulkan SDK（可选）

### macOS
- CMake 3.20或更高版本
- Xcode Command Line Tools或Xcode
- 或者Ninja构建系统
- Vulkan SDK（可选）

## 快速开始

### Windows平台

#### 使用Visual Studio
```batch
# 方法1：使用提供的脚本
Win-GenProj-CMake.bat vs2022

# 方法2：手动生成
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
```

#### 使用Ninja
```batch
mkdir build
cd build
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

### macOS平台

#### 使用Xcode
```bash
# 方法1：使用提供的脚本
./Mac-GenProj-CMake.sh xcode

# 方法2：手动生成
mkdir build
cd build
cmake -G "Xcode" -DCMAKE_BUILD_TYPE=Debug ..
```

#### 使用Unix Makefiles
```bash
mkdir build
cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug ..
make -j$(sysctl -n hw.ncpu)
```

#### 使用Ninja
```bash
mkdir build
cd build
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Debug ..
ninja
```

## 构建配置

### 构建类型
- **Debug**: 调试版本，包含调试符号，无优化
- **Release**: 发布版本，启用优化
- **Shipping**: 最终发布版本，最高优化级别

### 构建选项

#### 禁用示例项目
```bash
cmake -DBUILD_SAMPLES=OFF ..
```

#### 指定Vulkan SDK路径
```bash
# Windows
set VULKAN_SDK=C:\VulkanSDK\1.3.xxx.0
cmake ..

# macOS/Linux
export VULKAN_SDK=/path/to/VulkanSDK
cmake ..
```

## 构建项目

### 命令行构建

```bash
# Debug版本
cmake --build build --config Debug

# Release版本
cmake --build build --config Release

# Shipping版本
cmake --build build --config Shipping

# 并行构建（指定线程数）
cmake --build build --config Debug -- -j8
```

### IDE构建

- **Visual Studio**: 打开`build/Helios.slnx`，在IDE中构建

- **Xcode**: 打开`build/Helios.xcodeproj`，在IDE中构建

## 输出目录

构建产物位于：
- 可执行文件: `build/bin/<配置>-<平台>-x64/`
- 静态库: `build/lib/<配置>-<平台>-x64/`

例如：
- Debug-macOS-x64/Editor
- Release-Windows-x64/ModelEditor.exe

## 项目结构

### 主要目标

1. **Kernel**: 核心静态库
   - 包含引擎核心功能
   - 使用预编译头（Pch.h）
   - 支持Tracy性能分析

2. **Editor**: 场景编辑器应用
   - 依赖Kernel和ImGuizmo
   - 控制台应用程序

3. **ModelEditor**: 模型编辑器应用
   - 依赖Kernel和ImGuizmo
   - 控制台应用程序

4. **Sample_SkyBox**: 天空盒示例
   - 依赖Kernel
   - 演示天空盒渲染

5. **Sample_CSM**: 级联阴影示例
   - 依赖Kernel
   - 演示CSM技术

### 第三方库

项目包含以下第三方库（静态链接）：
- **GLFW**: 窗口和输入管理
- **Glad**: OpenGL加载器
- **ImGui**: 即时模式GUI
- **ImGuizmo**: 3D变换gizmo
- **glm**: 数学库
- **spdlog**: 日志库
- **assimp**: 资产导入库
- **tinyxml2**: XML解析
- **entt**: 实体组件系统
- **tracy**: 性能分析器
- **magic_enum**: 枚举反射
- 以及其他辅助库

## 平台特定说明

### Windows
- 使用MSVC编译器
- 静态链接运行时库
- 支持多处理器编译（/MP）
- 自动复制引擎资源目录（EngineRes）到输出目录

### macOS
- 使用Clang/AppleClang编译器
- 支持Xcode Bundle
- 链接Cocoa、IOKit等框架
- 使用OpenGL框架

## 故障排除

### CMake版本过低
```
CMake Error: CMake minimum required version...
```
**解决方案**: 升级CMake到3.20或更高版本

### 找不到Vulkan SDK
```
Vulkan SDK: (empty)
```
**解决方案**: 安装Vulkan SDK并设置环境变量`VULKAN_SDK`

### 构建失败
1. 清理构建目录：
   ```bash
   rm -rf build
   mkdir build
   cd build
   cmake ..
   ```

2. 检查编译器版本和工具链

3. 确保所有第三方库子模块已正确初始化：
   ```bash
   git submodule update --init --recursive
   ```

## 贡献

如果添加新的源文件或依赖项，请更新对应的CMakeLists.txt文件。
