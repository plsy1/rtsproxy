#!/bin/sh
# install.sh 逻辑单元测试脚本
# 运行于本地以验证所有架构、大版本号、包管理器识别及 Fallback 触发的逻辑

# 模拟 GitHub API 响应
TAG="v0.3.3-r2"
VERSION_FULL="0.3.3-r2"
REPO="plsy1/rtsproxy"

test_case() {
    local test_name="$1"
    local OWRT_VERSION="$2"
    local OWRT_ARCH="$3"
    local HAS_APK="$4" # true or false
    
    echo "==============================================="
    echo "测试案例: $test_name"
    echo "输入 -> 版本: $OWRT_VERSION, 架构: $OWRT_ARCH, 包管理器: $( [ "$HAS_APK" = "true" ] && echo "APK" || echo "OPKG" )"
    
    # 模拟 install.sh 的核心变量解析逻辑
    local OWRT_MAJOR=$(echo "$OWRT_VERSION" | cut -d. -f1,2)
    local UNAME_M=""
    case "$OWRT_ARCH" in
        x86_64) UNAME_M="x86_64" ;;
        aarch64*) UNAME_M="aarch64" ;;
        mipsel*) UNAME_M="mipsel" ;;
        mips*) UNAME_M="mips" ;;
        arm*) UNAME_M="armv7l" ;;
    esac
    
    # 3. 映射静态二进制架构名
    local BIN_ARCH=""
    case "$OWRT_ARCH" in
        x86_64) BIN_ARCH="x86_64" ;;
        i386|i486|i586|i686) BIN_ARCH="i686" ;;
        aarch64*) BIN_ARCH="arm64" ;;
        arm_cortex-a7*|arm_cortex-a9*|arm_cortex-a15*) BIN_ARCH="arm32v7hf" ;;
        arm_cortex-a5*) BIN_ARCH="arm32hf" ;;
        arm*) BIN_ARCH="arm32" ;;
        
        # MIPS 小端 (mipsel) - 必须放在 mips 大端之前，因为 mipsel 也是以 mips 开头！
        mipsel_24kc*) BIN_ARCH="mips32elsf" ;; # mipsel 24Kc (如 MT7620/MT7621 软路由) 是经典软浮点
        mipsel_74kc) BIN_ARCH="mips32el" ;; # mipsel 74Kc 支持硬浮点
        mipsel_mips32) BIN_ARCH="mips32elsf" ;;
        mipsel*) BIN_ARCH="mips32el" ;; # 其它小端
        
        # MIPS 大端
        mips_24kc) BIN_ARCH="mips32sf" ;; # MIPS 24Kc 是经典软浮点，运行硬浮点会导致非法指令崩溃
        mips_mips32) BIN_ARCH="mips32sf" ;;
        mips*) BIN_ARCH="mips32" ;; # 其它大端
        
        riscv64) BIN_ARCH="riscv64" ;;
        loongarch64*) BIN_ARCH="loong64" ;;
    esac

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
    local SUFFIX=""
    local SDK_VER=""
    if [ "$HAS_APK" = "true" ]; then
        SUFFIX="apk"
        SDK_VER="25.12.0"
    else
        SUFFIX="ipk"
        SDK_VER="24.10.4"
    fi
    
    local LUCI_PKG="luci-app-rtsproxy_${VERSION_FULL}_all.${SUFFIX}"
    local CORE_PKG="rtsproxy_${VERSION_FULL}_openwrt-${SDK_VER}-${OWRT_ARCH}.${SUFFIX}"
    
    # 判断是否支持常规安装主程序包
    local USE_PRECOMPILED=0
    if [ "$OWRT_MAJOR" = "24.10" ] || [ "$OWRT_MAJOR" = "25.12" ]; then
        USE_PRECOMPILED=1
    fi
    
    # 模拟下载行为
    local DOWNLOAD_CORE=0
    local DOWNLOAD_LUCI=1 # luci 总是下载
    if [ "$USE_PRECOMPILED" -eq 1 ]; then
        DOWNLOAD_CORE=1
    fi
    
    # 模拟安装判定
    local INSTALLED_CORE=0
    if [ "$USE_PRECOMPILED" -eq 1 ] && [ "$DOWNLOAD_CORE" -eq 1 ]; then
        # 模拟常规包管理器安装
        INSTALLED_CORE=1
    fi
    
    # 模拟 Fallback 行为
    local FALLBACK_TRIGGERED=0
    local FALLBACK_BIN=""
    if [ "$INSTALLED_CORE" -eq 0 ]; then
        FALLBACK_TRIGGERED=1
        FALLBACK_BIN="rtsproxy-${VERSION_FULL}-linux-$BIN_ARCH"
    fi
    
    # 输出解析结果
    echo "大版本号解析: $OWRT_MAJOR"
    echo "是否允许预编译包: $USE_PRECOMPILED"
    echo "预计下载文件 ->"
    echo "  LuCI界面包: $LUCI_PKG"
    if [ "$DOWNLOAD_CORE" -eq 1 ]; then
        echo "  核心程序包: $CORE_PKG"
    else
        echo "  核心程序包: (已跳过)"
    fi
    
    echo "常规安装结果 -> $( [ "$INSTALLED_CORE" -eq 1 ] && echo "成功" || echo "跳过/失败" )"
    echo "Fallback 状态 -> $( [ "$FALLBACK_TRIGGERED" -eq 1 ] && echo "【已触发】" || echo "未触发" )"
    if [ "$FALLBACK_TRIGGERED" -eq 1 ]; then
        echo "  静态二进制包下载目标: $FALLBACK_BIN"
    fi
    echo ""
}

# 测试案例 1：标准 24.10 x86_64 设备 (opkg)
test_case "24.10-x86_64" "24.10.4" "x86_64" "false"

# 测试案例 2：23.05 mipsel_24kc 设备 (opkg, 触发 Fallback 并拉取 MIPS 小端软浮点)
test_case "23.05-mipsel_24kc" "23.05.2" "mipsel_24kc" "false"

# 测试案例 3：25.12 aarch64 设备 (apk)
test_case "25.12-aarch64" "25.12.0" "aarch64_generic" "true"

# 测试案例 4：22.03 arm_cortex-a9 设备 (opkg, 触发 Fallback 并拉取 ARM32 硬浮点)
test_case "22.03-arm_cortex-a9" "22.03.5" "arm_cortex-a9" "false"
