#!/bin/bash

# 配置信息 (对齐 build_openwrt.sh)
SDK_URL="https://downloads.openwrt.org/releases/24.10.4/targets/x86/64/openwrt-sdk-24.10.4-x86-64_gcc-13.3.0_musl.Linux-x86_64.tar.zst"
SDK_DIR="openwrt-sdk-x86_64"
PROJECT_DIR=$(pwd)

set -e

echo "=== RTSP Proxy LuCI App 编译脚本 ==="

# 1. 检查并下载 SDK
if [ ! -d "$SDK_DIR" ]; then
    echo "[1/4] 下载并解压 OpenWrt SDK..."
    mkdir -p "$SDK_DIR"
    wget -qO- "$SDK_URL" | tar --zstd -x -C "$SDK_DIR" --strip-components=1
else
    echo "[1/4] SDK 已存在。"
fi

# 2. 初始化 SDK 环境
echo "[2/4] 初始化 SDK 环境..."
cd "$SDK_DIR"
./scripts/feeds update -a > /dev/null
./scripts/feeds install -a > /dev/null

# 3. 准备 LuCI 源码
echo "[3/4] 准备 LuCI 软件包源码..."
rm -rf package/rtsproxy package/luci-app-rtsproxy

# 核心二进制包 (只放 Makefile 解决依赖)
mkdir -p package/rtsproxy
cp "$PROJECT_DIR/openwrt/rtsproxy/Makefile" package/rtsproxy/

# LuCI 界面包
mkdir -p package/luci-app-rtsproxy
cp "$PROJECT_DIR/openwrt/luci-app-rtsproxy/Makefile" package/luci-app-rtsproxy/
[ -d "$PROJECT_DIR/openwrt/luci-app-rtsproxy/root" ] && cp -r "$PROJECT_DIR/openwrt/luci-app-rtsproxy/root" package/luci-app-rtsproxy/
[ -d "$PROJECT_DIR/openwrt/luci-app-rtsproxy/htdocs" ] && cp -r "$PROJECT_DIR/openwrt/luci-app-rtsproxy/htdocs" package/luci-app-rtsproxy/

# 修复换行符
find package/luci-app-rtsproxy -type f -exec sed -i 's/\r$//' {} +

# 4. 执行编译
echo "[4/4] 开始编译 LuCI App..."
echo "CONFIG_PACKAGE_rtsproxy=y" >> .config
echo "CONFIG_PACKAGE_luci-app-rtsproxy=y" >> .config
make defconfig > /dev/null

# 仅编译 LuCI 包
make package/luci-app-rtsproxy/compile V=s -j$(nproc)

# 5. 整理产物
cd "$PROJECT_DIR"
mkdir -p bin/openwrt
rm -f bin/openwrt/luci-app-rtsproxy*.ipk
find "$SDK_DIR/bin/packages/" -name "luci-app-rtsproxy*.ipk" -exec cp {} bin/openwrt/ \;

echo "编译成功！产物位置: bin/openwrt/"
ls -lh bin/openwrt/luci-app-rtsproxy*.ipk
