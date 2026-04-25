#!/bin/bash

# 配置信息 (默认编译 x86_64 架构)
SDK_URL="https://downloads.openwrt.org/releases/24.10.4/targets/x86/64/openwrt-sdk-24.10.4-x86-64_gcc-13.3.0_musl.Linux-x86_64.tar.zst"
SDK_DIR="openwrt-sdk-x86_64"
PROJECT_DIR=$(pwd)

# 捕获错误
set -e

echo "=== RTSP Proxy OpenWrt 一键本地编译脚本 === "

# 0. 检查依赖
check_dependency() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "错误: 未找到 $1, 请先安装它。"
        echo "您可以运行: sudo apt-get update && sudo apt-get install -y gawk xsltproc build-essential libncurses-dev zlib1g-dev rsync git unzip zstd"
        exit 1
    fi
}

echo "[0/4] 检查本地依赖..."
check_dependency gawk
check_dependency xsltproc
check_dependency wget
check_dependency tar
check_dependency make
check_dependency rsync
check_dependency git
check_dependency unzip
check_dependency zstd


# 1. 检查并下载 SDK
if [ ! -d "$SDK_DIR" ]; then
    echo "[1/4] 正在下载并解压 OpenWrt SDK (约 150MB)..."
    mkdir -p "$SDK_DIR"
    wget -qO- "$SDK_URL" | tar --zstd -x -C "$SDK_DIR" --strip-components=1
else
    echo "[1/4] SDK 已存在，跳过下载。"
fi

# 2. 初始化 SDK 环境
echo "[2/4] 初始化 SDK 环境..."
cd "$SDK_DIR"
./scripts/feeds update -a > /dev/null
./scripts/feeds install -a > /dev/null

# 3. 关联当前源码
echo "[3/4] 准备软件包源码..."
# 清理旧的包定义
rm -rf package/rtsproxy package/luci-app-rtsproxy

# 创建并复制核心包
mkdir -p package/rtsproxy
cp "$PROJECT_DIR/openwrt/rtsproxy/Makefile" package/rtsproxy/
cp -r "$PROJECT_DIR/openwrt/rtsproxy/files" package/rtsproxy/
cp -r "$PROJECT_DIR/src" package/rtsproxy/
cp -r "$PROJECT_DIR/include" package/rtsproxy/
cp "$PROJECT_DIR/meson.build" package/rtsproxy/
cp "$PROJECT_DIR/meson_options.txt" package/rtsproxy/

# 创建并复制 LuCI 包
mkdir -p package/luci-app-rtsproxy
cp "$PROJECT_DIR/openwrt/luci-app-rtsproxy/Makefile" package/luci-app-rtsproxy/
[ -d "$PROJECT_DIR/openwrt/luci-app-rtsproxy/root" ] && cp -r "$PROJECT_DIR/openwrt/luci-app-rtsproxy/root" package/luci-app-rtsproxy/
[ -d "$PROJECT_DIR/openwrt/luci-app-rtsproxy/htdocs" ] && cp -r "$PROJECT_DIR/openwrt/luci-app-rtsproxy/htdocs" package/luci-app-rtsproxy/

# 强制修复换行符问题 (CRLF -> LF)
find package/rtsproxy package/luci-app-rtsproxy -type f -exec sed -i 's/\r$//' {} +

# 4. 执行编译
echo "[4/4] 开始交叉编译..."
# 强制选中我们的包
echo "CONFIG_PACKAGE_rtsproxy=y" >> .config
echo "CONFIG_PACKAGE_luci-app-rtsproxy=y" >> .config
make defconfig > /dev/null

# 编译，使用多核加速
make package/rtsproxy/compile V=s -j$(nproc)
make package/luci-app-rtsproxy/compile V=s -j$(nproc)

# 5. 整理产物
cd "$PROJECT_DIR"
mkdir -p bin/openwrt
rm -f bin/openwrt/*.ipk
find "$SDK_DIR/bin/packages/" -name "rtsproxy*.ipk" -exec cp {} bin/openwrt/ \;
find "$SDK_DIR/bin/packages/" -name "luci-app-rtsproxy*.ipk" -exec cp {} bin/openwrt/ \;

echo "======================================="
echo "编译成功！"
echo "安装包路径: $(pwd)/bin/openwrt/"
ls -lh bin/openwrt/*.ipk
