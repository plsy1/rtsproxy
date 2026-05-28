#!/bin/sh
# RTSP Proxy 一键安装脚本 (OpenWrt)
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

# 加载系统原生配置信息
. /etc/openwrt_release
OWRT_VERSION="${DISTRIB_RELEASE:-SNAPSHOT}"
OWRT_ARCH="${DISTRIB_ARCH}"
OWRT_MAJOR=$(echo "$OWRT_VERSION" | cut -d. -f1,2)

if [ -z "$OWRT_ARCH" ]; then
    # 如果没有读取到 DISTRIB_ARCH 变量，则尝试回退用 opkg 获取
    if command -v opkg >/dev/null 2>&1; then
        OWRT_ARCH=$(opkg print-architecture | grep -v 'all' | grep -v 'noarch' | tail -n1 | awk '{print $2}')
    else
        OWRT_ARCH=$(uname -m)
    fi
fi

UNAME_M=$(uname -m)

echo "[*] 系统版本: ${DISTRIB_ID:-OpenWrt} $OWRT_VERSION"
echo "[*] 系统架构: $OWRT_ARCH ($UNAME_M)"

# 2. 获取最新版本号
echo "[*] 正在从 GitHub 获取最新版本信息..."
TAG=$(wget -qO- "$GITHUB_API" | grep '"tag_name":' | head -n1 | sed -E 's/.*"([^"]+)".*/\1/')
VERSION_FULL=$(echo "$TAG" | sed 's/^v//')

if [ -z "$VERSION_FULL" ]; then
    echo "错误: 无法获取最新版本号，请检查网络连接。"
    exit 1
fi

echo "[*] 最新版本: $TAG"

# 3. 映射静态二进制架构名（精细化浮点数与EABI选择）
# 配合 .github/workflows/static_binaries.yml 中编译的 27 个架构进行精准选择，防范 Illegal instruction 崩溃
BIN_ARCH=""
case "$OWRT_ARCH" in
    # x86
    x86_64) BIN_ARCH="x86_64" ;;
    i386|i486|i586|i686) BIN_ARCH="i686" ;;
    
    # ARM64 (aarch64)
    aarch64*) BIN_ARCH="arm64" ;;
    
    # ARM32 (OpenWrt ARMv7 路由器基本均支持硬浮点，使用 EABIhf)
    arm_cortex-a7*|arm_cortex-a9*|arm_cortex-a15*) BIN_ARCH="arm32v7hf" ;;
    arm_cortex-a5*) BIN_ARCH="arm32hf" ;;
    arm*) BIN_ARCH="arm32" ;; # 其它 ARM32 设备使用软浮点保底
    
    # MIPS 小端 (mipsel) - 必须放在 mips 大端之前，因为 mipsel 也是以 mips 开头！
    mipsel_24kc*) BIN_ARCH="mips32elsf" ;; # mipsel 24Kc (如 MT7620/MT7621 软路由) 是经典软浮点
    mipsel_74kc) BIN_ARCH="mips32el" ;; # mipsel 74Kc 支持硬浮点
    mipsel_mips32) BIN_ARCH="mips32elsf" ;;
    mipsel*) BIN_ARCH="mips32el" ;; # 其它小端
    
    # MIPS 大端
    mips_24kc) BIN_ARCH="mips32sf" ;; # MIPS 24Kc 是经典软浮点，运行硬浮点会导致非法指令崩溃
    mips_mips32) BIN_ARCH="mips32sf" ;;
    mips*) BIN_ARCH="mips32" ;; # 其它大端
    
    # 其他现代架构
    riscv64) BIN_ARCH="riscv64" ;;
    loongarch64*) BIN_ARCH="loong64" ;;
esac

# 如果 OWRT_ARCH 没匹配成功，使用 uname -m 粗粒度映射保底
if [ -z "$BIN_ARCH" ]; then
    case "$UNAME_M" in
        x86_64) BIN_ARCH="x86_64" ;;
        aarch64) BIN_ARCH="arm64" ;;
        mips) BIN_ARCH="mips32sf" ;;
        mipsel) BIN_ARCH="mips32elsf" ;;
        arm*) BIN_ARCH="arm32" ;;
    esac
fi

# 4. 检测包管理器并映射对应的编译后缀与 SDK 版本
if command -v apk >/dev/null 2>&1; then
    SUFFIX="apk"
    SDK_VER="25.12.0"
else
    SUFFIX="ipk"
    SDK_VER="24.10.4"
fi

LUCI_PKG="luci-app-rtsproxy_${VERSION_FULL}_all.${SUFFIX}"
CORE_PKG="rtsproxy_${VERSION_FULL}_openwrt-${SDK_VER}-${OWRT_ARCH}.${SUFFIX}"

# 判断是否支持常规安装主程序包 (仅在 24.10.x 和 25.12.x 上使用系统包管理器安装 rtsproxy 核心包，其它老版本一律走静态二进制保底以防止库 ABI 不兼容崩溃)
USE_PRECOMPILED=0
if [ "$OWRT_MAJOR" = "24.10" ] || [ "$OWRT_MAJOR" = "25.12" ]; then
    USE_PRECOMPILED=1
fi

# 5. 下载文件
echo "[*] 正在从 GitHub 下载软件包..."
wget -qO "/tmp/$LUCI_PKG" "$GITHUB_DOWNLOAD/$TAG/$LUCI_PKG" || true
if [ "$USE_PRECOMPILED" -eq 1 ]; then
    wget -qO "/tmp/$CORE_PKG" "$GITHUB_DOWNLOAD/$TAG/$CORE_PKG" || true
fi

INSTALLED_CORE=0

# 6. 尝试使用系统包管理器进行正常安装
if [ "$USE_PRECOMPILED" -eq 1 ] && [ -f "/tmp/$CORE_PKG" ] && [ -f "/tmp/$LUCI_PKG" ]; then
    echo "[*] 正在使用系统包管理器安装核心程序与网页界面..."
    if [ "$SUFFIX" = "apk" ]; then
        if apk add --allow-untrusted "/tmp/$CORE_PKG" "/tmp/$LUCI_PKG"; then
            INSTALLED_CORE=1
        else
            echo "[!] 软件包安装失败，准备尝试全静态二进制回退方案..."
        fi
    else
        if opkg install "/tmp/$CORE_PKG" "/tmp/$LUCI_PKG" --force-reinstall; then
            INSTALLED_CORE=1
        else
            echo "[!] 软件包安装失败，准备尝试全静态二进制回退方案..."
        fi
    fi
fi

# 7. 回退保底方案：强装 LuCI 界面并下载全静态 musl 二进制
if [ "$INSTALLED_CORE" -eq 0 ]; then
    if [ -z "$BIN_ARCH" ]; then
        echo "错误: 无法确定适用于您架构的静态二进制文件 ($OWRT_ARCH)"
        exit 1
    fi

    # 7.1 强装 LuCI 界面（忽略对 rtsproxy 核心包的依赖）
    if [ -f "/tmp/$LUCI_PKG" ]; then
        echo "[*] 正在强行安装 LuCI 界面..."
        if [ "$SUFFIX" = "apk" ]; then
            apk add --allow-untrusted --nodeps "/tmp/$LUCI_PKG" || true
        else
            opkg install "/tmp/$LUCI_PKG" --force-depends --force-reinstall || true
        fi
    else
        echo "[!] 警告: 未能找到本地的 LuCI 网页安装包，无法安装界面。"
    fi

    # 7.2 下载静态二进制
    BIN_FILE="rtsproxy-${VERSION_FULL}-linux-$BIN_ARCH"
    echo "[*] 正在下载适用于当前架构的静态二进制主程序: $BIN_FILE"
    
    if wget -qO "/usr/bin/rtsproxy" "$GITHUB_DOWNLOAD/$TAG/$BIN_FILE"; then
        chmod +x /usr/bin/rtsproxy
        echo "[*] 静态二进制下载并安装成功。"
        
        # 7.3 手动写入启动服务与默认配置文件（因为跳过了核心包的安装）
        echo "[*] 正在配置系统守护进程与服务项..."
        mkdir -p /etc/init.d /etc/rtsproxy /etc/config
        
        # 写入 init 启动脚本 (procd)
        cat << 'EOF' > /etc/init.d/rtsproxy
#!/bin/sh /etc/rc.common
START=99
USE_PROCD=1

start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/rtsproxy -c /etc/rtsproxy/config.json
    procd_set_param respawn
    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_close_instance
}
EOF
        chmod +x /etc/init.d/rtsproxy
        
        # 写入默认 JSON 配置文件（优先从 GitHub 对应 Tag 下载，确保版本配置契合）
        if [ ! -f /etc/rtsproxy/config.json ]; then
            echo "[*] 正在从 GitHub 仓库下载标准配置文件..."
            if ! wget -qO "/etc/rtsproxy/config.json" "https://raw.githubusercontent.com/$REPO/$TAG/config.json"; then
                echo "[!] 警告: 无法下载 Tag 版本的配置文件，尝试下载主分支配置..."
                if ! wget -qO "/etc/rtsproxy/config.json" "https://raw.githubusercontent.com/$REPO/main/config.json"; then
                    echo "[!] 警告: 无法从 GitHub 获取默认配置文件，写入一个极简本地配置保底。"
                    cat << 'EOF' > /etc/rtsproxy/config.json
{
    "settings": {
        "port": 8554,
        "log_level": "info"
    }
}
EOF
                fi
            fi
        fi
        
        # 写入默认 UCI 配置
        if [ ! -f /etc/config/rtsproxy ]; then
            cat << 'EOF' > /etc/config/rtsproxy
config rtsproxy 'global'
    option enabled '1'
EOF
        fi
        
        INSTALLED_CORE=1
    else
        echo "错误: 无法下载适用于您架构的静态二进制文件 ($BIN_FILE)。"
        exit 1
    fi
fi

# 清理缓存文件
rm -f "/tmp/$LUCI_PKG" "/tmp/$CORE_PKG"

# 8. 启动与开机自启服务
echo "[*] 正在拉起并自启 RTSP Proxy 服务..."
/etc/init.d/rtsproxy enable
/etc/init.d/rtsproxy restart

echo "==============================================="
echo "   RTSP Proxy 安装成功！"
echo "   您现在可以在 LuCI 菜单 '服务' -> 'RTSProxy' 中进行配置。"
echo "==============================================="
