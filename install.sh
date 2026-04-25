#!/bin/sh
# RTSP Proxy One-Click Installer for OpenWrt
# Repository: https://github.com/plsy1/rtsproxy

set -e

REPO="plsy1/rtsproxy"
GITHUB_API="https://api.github.com/repos/$REPO/releases/latest"
GITHUB_DOWNLOAD="https://github.com/$REPO/releases/download"

echo "==============================================="
echo "   RTSP Proxy 一键安装脚本 (OpenWrt)           "
echo "==============================================="

# 1. 检查运行环境
if [ ! -f /etc/openwrt_release ]; then
    echo "错误: 此脚本仅支持在 OpenWrt/ImmortalWrt 系统上运行。"
    exit 1
fi

# 加载系统信息
. /etc/openwrt_release
OWRT_VERSION="${DISTRIB_RELEASE:-SNAPSHOT}"
OWRT_MAJOR=$(echo "$OWRT_VERSION" | cut -d. -f1,2)

# 获取 OpenWrt 架构
OWRT_ARCH=$(opkg print-architecture | grep -v 'all' | grep -v 'noarch' | tail -n1 | awk '{print $2}')
UNAME_M=$(uname -m)

echo "[*] 系统版本: OpenWrt $OWRT_VERSION"
echo "[*] 系统架构: $OWRT_ARCH ($UNAME_M)"

# 2. 获取最新版本号
echo "[*] 正在从 GitHub 获取最新版本信息..."
# 使用 wget 获取 JSON 并解析 tag_name (简单解析)
VERSION_JSON=$(wget -qO- "$GITHUB_API")
TAG=$(echo "$VERSION_JSON" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
VERSION=$(echo "$TAG" | sed 's/^v//')

if [ -z "$VERSION" ]; then
    echo "错误: 无法获取最新版本号，请检查网络连接。"
    exit 1
fi

echo "[*] 最新版本: $TAG"

# 3. 映射静态二进制架构名
BIN_ARCH=""
case "$OWRT_ARCH" in
    x86_64) BIN_ARCH="x86_64" ;;
    aarch64_*) BIN_ARCH="arm64" ;;
    arm_*) BIN_ARCH="arm32v7hf" ;;
    mips_*) BIN_ARCH="mips32" ;;
    mipsel_*) BIN_ARCH="mips32el" ;;
esac

# 如果 OWRT_ARCH 没匹配上，试试 uname -m
if [ -z "$BIN_ARCH" ]; then
    case "$UNAME_M" in
        x86_64) BIN_ARCH="x86_64" ;;
        aarch64) BIN_ARCH="arm64" ;;
        mips) BIN_ARCH="mips32" ;;
        mipsel) BIN_ARCH="mips32el" ;;
    esac
fi

# 4. 下载并安装 LuCI 界面
LUCI_IPK="luci-app-rtsproxy_${VERSION}-r1_all.ipk"
echo "[*] 正在下载 LuCI 界面: $LUCI_IPK"
wget -qO "/tmp/$LUCI_IPK" "$GITHUB_DOWNLOAD/$TAG/$LUCI_IPK"

echo "[*] 正在安装 LuCI 界面..."
opkg install "/tmp/$LUCI_IPK" --force-reinstall
rm -f "/tmp/$LUCI_IPK"

# 5. 尝试安装匹配的 IPK 本包
INSTALLED_CORE=0

# 只针对我们编译矩阵中有的版本和架构尝试 IPK
if [ "$OWRT_MAJOR" = "23.05" ] || [ "$OWRT_MAJOR" = "24.10" ]; then
    IPK_ARCH_TAG=""
    case "$OWRT_ARCH" in
        x86_64) IPK_ARCH_TAG="x86_64" ;;
        aarch64_*) IPK_ARCH_TAG="aarch64" ;;
    esac

    if [ -n "$IPK_ARCH_TAG" ]; then
        CORE_IPK="rtsproxy_${VERSION}-r1_openwrt-${OWRT_MAJOR}-${IPK_ARCH_TAG}.ipk"
        echo "[*] 尝试下载匹配系统的 IPK: $CORE_IPK"
        if wget -qO "/tmp/$CORE_IPK" "$GITHUB_DOWNLOAD/$TAG/$CORE_IPK"; then
            echo "[*] 正在安装核心程序 IPK..."
            if opkg install "/tmp/$CORE_IPK" --force-reinstall; then
                INSTALLED_CORE=1
            fi
            rm -f "/tmp/$CORE_IPK"
        fi
    fi
fi

# 6. 如果 IPK 安装失败或不匹配，则回退到静态二进制文件
if [ "$INSTALLED_CORE" -eq 0 ]; then
    if [ -z "$BIN_ARCH" ]; then
        echo "错误: 无法确定适用于您架构的静态二进制文件 ($OWRT_ARCH)"
        exit 1
    fi

    BIN_FILE="rtsproxy-${VERSION}-linux-$BIN_ARCH"
    echo "[!] 未找到匹配的 IPK 或安装失败，回退到静态二进制: $BIN_FILE"
    
    if wget -qO "/usr/bin/rtsproxy" "$GITHUB_DOWNLOAD/$TAG/$BIN_FILE"; then
        chmod +x /usr/bin/rtsproxy
        echo "[*] 静态二进制安装成功。"
        INSTALLED_CORE=1
    else
        echo "错误: 无法下载静态二进制文件。"
        exit 1
    fi
fi

# 7. 启动服务
echo "[*] 正在启动 RTSP Proxy 服务..."
/etc/init.d/rtsproxy enable
/etc/init.d/rtsproxy restart

echo "==============================================="
echo "   安装完成！"
echo "   您现在可以在 LuCI 菜单 '服务' -> 'RTSProxy' 中进行配置。"
echo "==============================================="
