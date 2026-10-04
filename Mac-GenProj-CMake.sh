#!/bin/bash
# ============================================================
#  Helios CMake Project Generator (macOS)
#
#  双击运行   : 直接双击同目录下的 Mac-GenProj-CMake.command
#               无参数时会弹出交互式菜单选择生成器
#  终端运行   : ./Mac-GenProj-CMake.sh [xcode|ninja|make] [--clean] [--no-wait]
#
#  相比旧版的关键修复:
#   1. 去掉文件头 BOM(EF BB BF)，否则内核找不到 #! 无法直接执行
#   2. 自动定位工程根目录（双击时 cwd 是家目录，不能直接 cmake ..）
#   3. 补全 PATH，保证能找到 /usr/local/bin、/opt/homebrew/bin 下的 cmake
#   4. 检测 build 目录里已存在的生成器，冲突时提示清理，避免 silently 失败
# ============================================================

set -u

# ---------- 工具函数 ----------

# 解析脚本自身所在目录（支持符号链接），双击时 cwd 是家目录，必须自己找家
resolve_script_dir() {
    local src="${BASH_SOURCE[0]:-$0}"
    local dir link
    while [ -h "$src" ]; do
        dir="$(cd -P "$(dirname "$src")" && pwd)"
        link="$(readlink "$src")"
        [ "${link#/}" = "$link" ] && link="$dir/$link"
        src="$link"
    done
    (cd -P "$(dirname "$src")" && pwd)
}

# 结束前暂停，避免双击时终端窗口一闪而过（--no-wait 可跳过）
pause_if_gui() {
    [ "${HELIOS_NO_WAIT:-0}" = "1" ] && return 0
    [ -t 0 ] || return 0          # 非交互（管道/重定向）时不等待
    echo ""
    read -r -n 1 -s -p "按任意键关闭本窗口..."
    echo ""
}

PROJECT_ROOT="$(resolve_script_dir)"
cd "$PROJECT_ROOT" || { echo "[错误] 无法进入工程目录: $PROJECT_ROOT"; exit 1; }

echo "========================================"
echo " Helios CMake Project Generator (macOS)"
echo "========================================"
echo " 工程目录: $PROJECT_ROOT"
echo ""

# ---------- 1. 补全 PATH 并定位 cmake ----------
export PATH="/usr/local/bin:/opt/homebrew/bin:/opt/local/bin:$PATH"

CMAKE_BIN=""
for cand in cmake \
            /usr/local/bin/cmake \
            /opt/homebrew/bin/cmake \
            /opt/local/bin/cmake \
            /Applications/CMake.app/Contents/bin/cmake; do
    if command -v "$cand" >/dev/null 2>&1; then
        CMAKE_BIN="$(command -v "$cand")"
        break
    fi
done

if [ -z "$CMAKE_BIN" ]; then
    echo "[错误] 未找到 cmake，请先安装："
    echo "         brew install cmake"
    echo "       或 https://cmake.org/download/"
    pause_if_gui
    exit 1
fi
echo "cmake: $CMAKE_BIN ($("$CMAKE_BIN" --version | head -1))"

# ---------- 2. 参数解析 ----------
GENERATOR=""
BUILD_DIR_OVERRIDE=""
CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        xcode)   GENERATOR="Xcode";          shift ;;
        ninja)   GENERATOR="Ninja";          shift ;;
        make)    GENERATOR="Unix Makefiles"; shift ;;
        --clean|-c) CLEAN=1;                 shift ;;
        --no-wait)  HELIOS_NO_WAIT=1;        shift ;;
        --dir|-d)   BUILD_DIR_OVERRIDE="$2"; shift 2 ;;
        -h|--help)
            echo "用法: $(basename "$0") [xcode|ninja|make] [--dir <目录>] [--clean] [--no-wait]"
            exit 0 ;;
        *)
            echo "[错误] 未知参数: $1"
            echo "用法: $(basename "$0") [xcode|ninja|make] [--dir <目录>] [--clean] [--no-wait]"
            pause_if_gui
            exit 1 ;;
    esac
done

# ---------- 3. 无参数时交互式选择生成器 ----------
if [ -z "$GENERATOR" ]; then
    echo ""
    echo "请选择 CMake 生成器:"
    echo "  1) Xcode          (推荐，生成 Helios.xcodeproj)"
    echo "  2) Ninja          (需 brew install ninja)"
    echo "  3) Unix Makefiles"
    echo ""
    printf "输入序号并回车 [默认 1]: "
    read -r choice
    case "$choice" in
        ""|1) GENERATOR="Xcode" ;;
        2)    GENERATOR="Ninja" ;;
        3)    GENERATOR="Unix Makefiles" ;;
        *)
            echo "[错误] 无效选择: $choice"
            pause_if_gui
            exit 1 ;;
    esac
fi

# 按生成器选择输出目录，避免不同生成器的 CMakeCache 互相冲突
if [ -n "$BUILD_DIR_OVERRIDE" ]; then
    BUILD_DIR="$BUILD_DIR_OVERRIDE"
else
    case "$GENERATOR" in
        Xcode)           BUILD_DIR="build-xcode" ;;
        Ninja)           BUILD_DIR="build-ninja" ;;
        "Unix Makefiles") BUILD_DIR="build" ;;
    esac
fi

echo ""
echo "生成器: $GENERATOR"
echo "输出目录: $BUILD_DIR"
echo ""

# ---------- 4. 生成器冲突检测 ----------
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    EXISTING="$(grep -m1 '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2-)"
    if [ -n "$EXISTING" ] && [ "$EXISTING" != "$GENERATOR" ]; then
        echo "[警告] $BUILD_DIR 目录此前用 \"$EXISTING\" 配置过，"
        echo "       与本次的 \"$GENERATOR\" 冲突，CMake 会直接报错。"
        if [ "$CLEAN" -eq 0 ]; then
            printf "是否清空 %s 后重新生成？[y/N] " "$BUILD_DIR"
            read -r ans
            case "$ans" in
                [yY]*) CLEAN=1 ;;
                *)
                    echo "已取消，未做任何改动。"
                    pause_if_gui
                    exit 1 ;;
            esac
        fi
    fi
fi

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "清理目录: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# ---------- 5. 执行 CMake 配置 ----------
echo ""
echo "==> cmake -G \"$GENERATOR\" -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Debug"
echo ""

"$CMAKE_BIN" -G "$GENERATOR" -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
STATUS=$?

echo ""
if [ "$STATUS" -ne 0 ]; then
    echo "========================================"
    echo " CMake 配置失败 (exit code $STATUS)"
    echo "========================================"
    pause_if_gui
    exit "$STATUS"
fi

echo "========================================"
echo " CMake 工程生成成功！"
echo "========================================"
echo ""
echo "后续操作:"
if [ "$GENERATOR" = "Xcode" ]; then
    echo "  1. 打开 $BUILD_DIR/Helios.xcodeproj"
    echo "  2. 在 Xcode 中选择 scheme 后 Cmd+B 构建"
else
    echo "  1. cmake --build $BUILD_DIR -j\$(sysctl -n hw.ncpu)"
fi
echo ""

# ---------- 6. 询问是否打开 Xcode ----------
if [ "$GENERATOR" = "Xcode" ]; then
    XCPROJ="$(ls -d "$BUILD_DIR"/*.xcodeproj 2>/dev/null | head -1)"
    if [ -n "$XCPROJ" ]; then
        printf "是否在 Xcode 中打开 %s？[y/N] " "$XCPROJ"
        read -r ans
        case "$ans" in
            [yY]*) echo "正在打开 Xcode..."; open "$XCPROJ" ;;
        esac
        echo ""
    fi
fi

pause_if_gui
exit 0
