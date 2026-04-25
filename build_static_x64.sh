#!/bin/bash
set -e

# 工具链信息（与 CI 保持一致）
TOOLCHAIN="x86_64-unknown-linux-musl"
URL="https://github.com/cross-tools/musl-cross/releases/download/20250929/${TOOLCHAIN}.tar.xz"
TOOLCHAIN_DIR="$(pwd)/toolchains"
TOOLCHAIN_PATH="${TOOLCHAIN_DIR}/${TOOLCHAIN}"

# 1. 检查并安装本地基础依赖
if ! command -v meson >/dev/null 2>&1; then
    echo "错误: 未找到 meson, 请先安装: sudo apt install meson ninja-build"
    exit 1
fi

# 2. 下载并解压工具链
mkdir -p "$TOOLCHAIN_DIR"
if [ ! -d "$TOOLCHAIN_PATH" ]; then
    echo "正在下载工具链 $TOOLCHAIN (约 30MB)..."
    wget -qO- "$URL" | tar xJ -C "$TOOLCHAIN_DIR"
else
    echo "工具链已存在，跳过下载。"
fi

# 3. 生成 Meson 交叉编译配置文件
cat <<CROSS > x86_64-musl.ini
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[binaries]
cpp = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-g++'
ar = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-ar'
as = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-as'
ld = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-ld'
nm = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-nm'
objcopy = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-objcopy'
objdump = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-objdump'
ranlib = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-ranlib'
strip = '${TOOLCHAIN_PATH}/bin/${TOOLCHAIN}-strip'
CROSS

# 4. 执行编译
echo "开始交叉编译静态二进制文件..."
rm -rf build-static-x64
meson setup build-static-x64 --cross-file=x86_64-musl.ini --strip --buildtype=release -Disstatic=true
meson compile -C build-static-x64

echo "======================================="
echo "编译成功！"
echo "静态二进制文件路径: $(pwd)/build-static-x64/rtsproxy"
ls -lh build-static-x64/rtsproxy
