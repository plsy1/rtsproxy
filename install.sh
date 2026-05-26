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

# 4. 检查包管理器和设置参数
if command -v apk >/dev/null 2>&1; then
    PKG_CMD="apk"
    SUFFIX="apk"
    SDK_VER="25.12.0"
else
    PKG_CMD="opkg"
    SUFFIX="ipk"
    SDK_VER="24.10.4"
fi

LUCI_PKG="luci-app-rtsproxy_${VERSION}_all.${SUFFIX}"
CORE_PKG="rtsproxy_${VERSION}_openwrt-${SDK_VER}-${OWRT_ARCH}.${SUFFIX}"

echo "[*] 正在从 GitHub 下载安装包..."
if ! wget -qO "/tmp/$LUCI_PKG" "$GITHUB_DOWNLOAD/$TAG/$LUCI_PKG"; then
    echo "[!] 错误: 无法下载 LuCI 安装包 ($LUCI_PKG)"
    echo "    请确认 GitHub Release ($TAG) 中是否已包含该文件。"
    exit 1
fi

INSTALLED_CORE=0
# 尝试安装对应架构的预编译包
echo "[*] 正在下载核心程序: $CORE_PKG"
if wget -qO "/tmp/$CORE_PKG" "$GITHUB_DOWNLOAD/$TAG/$CORE_PKG"; then
    echo "[*] 正在安装核心程序和 LuCI 界面..."
    if [ "$PKG_CMD" = "apk" ]; then
        if apk add --allow-untrusted "/tmp/$CORE_PKG" "/tmp/$LUCI_PKG"; then
            INSTALLED_CORE=1
        else
            echo "[!] APK 安装失败，准备尝试静态二进制回退方案..."
        fi
    else
        if opkg install "/tmp/$CORE_PKG" "/tmp/$LUCI_PKG" --force-reinstall; then
            INSTALLED_CORE=1
        else
            echo "[!] IPK 安装失败，准备尝试静态二进制回退方案..."
        fi
    fi
    rm -f "/tmp/$CORE_PKG"
else
    echo "[!] 警告: 无法下载对应的预编译核心包，将尝试静态二进制方案。"
fi

# 5. 回退方案：安装 LuCI 并下载静态二进制
if [ "$INSTALLED_CORE" -eq 0 ]; then
    if [ -z "$BIN_ARCH" ]; then
        echo "错误: 无法确定适用于您架构的静态二进制文件 ($OWRT_ARCH)"
        exit 1
    fi

    echo "[*] 正在强制安装 LuCI 界面..."
    if [ "$PKG_CMD" = "apk" ]; then
        apk add --allow-untrusted --nodeps "/tmp/$LUCI_PKG" || true
    else
        opkg install "/tmp/$LUCI_PKG" --force-depends --force-reinstall || true
    fi

    BIN_FILE="rtsproxy-${VERSION}-linux-$BIN_ARCH"
    echo "[!] 准备下载并安装静态二进制文件: $BIN_FILE"
    
    if wget -qO "/usr/bin/rtsproxy" "$GITHUB_DOWNLOAD/$TAG/$BIN_FILE"; then
        chmod +x /usr/bin/rtsproxy
        echo "[*] 静态二进制安装成功。"
        INSTALLED_CORE=1
    else
        echo "错误: 无法下载静态二进制文件 ($BIN_FILE)。"
        exit 1
    fi
fi

rm -f "/tmp/$LUCI_PKG"

# 7. 启动服务
echo "[*] 正在启动 RTSP Proxy 服务..."
/etc/init.d/rtsproxy enable
/etc/init.d/rtsproxy restart

echo "==============================================="
echo "   安装完成！"
echo "   您现在可以在 LuCI 菜单 '服务' -> 'RTSProxy' 中进行配置。"
echo "==============================================="
