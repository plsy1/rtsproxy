#!/bin/sh
# RTSP Proxy One-Click Installer for OpenWrt
# Repository: https://github.com/plsy1/rtsproxy

set -e

REPO="plsy1/rtsproxy"
GITHUB_API="https://api.github.com/repos/$REPO/releases"
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
# 获取发布列表中的第一个 tag_name (最新发布的版本)
TAG=$(wget -qO- "$GITHUB_API" | grep '"tag_name":' | head -n1 | sed -E 's/.*"([^"]+)".*/\1/')
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

# 4. 准备下载
LUCI_IPK="luci-app-rtsproxy_${VERSION}_all.ipk"
CORE_IPK=""
IPK_ARCH_TAG=""

# 5. 检查是否有匹配系统的核心 IPK
if [ "$OWRT_MAJOR" = "23.05" ] || [ "$OWRT_MAJOR" = "24.10" ]; then
    case "$OWRT_ARCH" in
        x86_64) IPK_ARCH_TAG="x64" ;;
        aarch64_*) IPK_ARCH_TAG="aarch64" ;;
    esac
    if [ -n "$IPK_ARCH_TAG" ]; then
        CORE_IPK="rtsproxy_${VERSION}_openwrt-${OWRT_MAJOR}-${IPK_ARCH_TAG}.ipk"
    fi
fi

echo "[*] 正在从 GitHub 下载安装包..."
wget -qO "/tmp/$LUCI_IPK" "$GITHUB_DOWNLOAD/$TAG/$LUCI_IPK"

INSTALLED_CORE=0
if [ -n "$CORE_IPK" ]; then
    echo "[*] 发现匹配系统的核心 IPK: $CORE_IPK"
    if wget -qO "/tmp/$CORE_IPK" "$GITHUB_DOWNLOAD/$TAG/$CORE_IPK"; then
        echo "[*] 正在同时安装核心程序和 LuCI 界面..."
        # 同时安装两个包可以自动解决依赖关系
        if opkg install "/tmp/$CORE_IPK" "/tmp/$LUCI_IPK" --force-reinstall; then
            INSTALLED_CORE=1
        else
            echo "[!] IPK 安装失败，准备尝试静态二进制回退方案..."
        fi
        rm -f "/tmp/$CORE_IPK"
    fi
fi

# 6. 回退方案：安装 LuCI 并下载静态二进制
if [ "$INSTALLED_CORE" -eq 0 ]; then
    if [ -z "$BIN_ARCH" ]; then
        echo "错误: 无法确定适用于您架构的静态二进制文件 ($OWRT_ARCH)"
        exit 1
    fi

    echo "[*] 正在通过强制依赖模式安装 LuCI 界面..."
    opkg install "/tmp/$LUCI_IPK" --force-depends --force-reinstall

    BIN_FILE="rtsproxy-${VERSION}-linux-$BIN_ARCH"
    echo "[!] 准备下载并安装静态二进制文件: $BIN_FILE"
    
    if wget -qO "/usr/bin/rtsproxy" "$GITHUB_DOWNLOAD/$TAG/$BIN_FILE"; then
        chmod +x /usr/bin/rtsproxy
        echo "[*] 静态二进制安装成功。"
        INSTALLED_CORE=1
    else
        echo "错误: 无法下载静态二进制文件。"
        exit 1
    fi
fi

rm -f "/tmp/$LUCI_IPK"

# 7. 启动服务
echo "[*] 正在启动 RTSP Proxy 服务..."
/etc/init.d/rtsproxy enable
/etc/init.d/rtsproxy restart

echo "==============================================="
echo "   安装完成！"
echo "   您现在可以在 LuCI 菜单 '服务' -> 'RTSProxy' 中进行配置。"
echo "==============================================="
