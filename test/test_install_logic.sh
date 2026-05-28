#!/bin/sh
# install.sh 逻辑单元测试脚本
# 运行于本地以验证所有架构、大版本号、包管理器识别及 Fallback 触发的逻辑
# 本脚本执行自动化断言验证，如果有任何测试案例不满足预期，会以非 0 状态码退出

# 模拟 GitHub API 响应
TAG="v0.3.3-r2"
VERSION_FULL="0.3.3-r2"
REPO="plsy1/rtsproxy"

# 全局失败计数器
FAILED_COUNT=0

test_case() {
    local test_name="$1"
    local OWRT_VERSION="$2"
    local OWRT_ARCH="$3"
    local HAS_APK="$4" # true or false
    
    # 期望的断言结果
    local EXP_BIN_ARCH="$5"
    local EXP_USE_PRECOMPILED="$6"
    local EXP_FALLBACK_TRIGGERED="$7"
    local EXP_FALLBACK_BIN="$8"
    
    # 模拟 install.sh 的核心变量解析逻辑
    local OWRT_MAJOR=$(echo "$OWRT_VERSION" | cut -d. -f1,2)
    local UNAME_M=""
    
    # 模拟 uname -m 回退
    case "$OWRT_ARCH" in
        x86_64) UNAME_M="x86_64" ;;
        aarch64*) UNAME_M="aarch64" ;;
        mipsel*) UNAME_M="mipsel" ;;
        mips*) UNAME_M="mips" ;;
        arm*) UNAME_M="armv7l" ;;
        *)
            # 如果没有 OWRT_ARCH，在此处模拟系统的 uname -m
            if [ "$test_name" = "Fallback-uname-m-mipsel" ]; then
                UNAME_M="mipsel"
            else
                UNAME_M="unknown"
            fi
            ;;
    esac
    
    # 3. 映射静态二进制架构名
    local BIN_ARCH=""
    case "$OWRT_ARCH" in
        # x86
        x86_64) BIN_ARCH="x86_64" ;;
        i386|i486|i586|i686) BIN_ARCH="i686" ;;
        
        # ARM64 (aarch64)
        aarch64*) BIN_ARCH="arm64" ;;
        
        # ARM32
        arm_cortex-a7*|arm_cortex-a9*|arm_cortex-a15*) BIN_ARCH="arm32v7hf" ;;
        arm_cortex-a5*) BIN_ARCH="arm32hf" ;;
        arm*) BIN_ARCH="arm32" ;;
        
        # MIPS 小端 (mipsel) - 必须放在 mips 大端之前，因为 mipsel 也是以 mips 开头！
        mipsel_24kc*) BIN_ARCH="mips32elsf" ;;
        mipsel_74kc) BIN_ARCH="mips32el" ;;
        mipsel_mips32) BIN_ARCH="mips32elsf" ;;
        mipsel*) BIN_ARCH="mips32el" ;;
        
        # MIPS 大端
        mips_24kc) BIN_ARCH="mips32sf" ;;
        mips_mips32) BIN_ARCH="mips32sf" ;;
        mips*) BIN_ARCH="mips32" ;;
        
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
    local SUFFIX=""
    local SDK_VER=""
    if [ "$HAS_APK" = "true" ]; then
        SUFFIX="apk"
        SDK_VER="25.12.0"
    else
        SUFFIX="ipk"
        SDK_VER="24.10.4"
    fi
    
    # 判断是否支持常规安装主程序包
    local USE_PRECOMPILED=0
    if [ "$OWRT_MAJOR" = "24.10" ] || [ "$OWRT_MAJOR" = "25.12" ]; then
        USE_PRECOMPILED=1
    fi
    
    # 模拟下载与常规安装判定
    local INSTALLED_CORE=0
    if [ "$USE_PRECOMPILED" -eq 1 ]; then
        # 模拟常规包管理器安装成功
        INSTALLED_CORE=1
    fi
    
    # 模拟 Fallback 行为
    local FALLBACK_TRIGGERED=0
    local FALLBACK_BIN=""
    if [ "$INSTALLED_CORE" -eq 0 ]; then
        FALLBACK_TRIGGERED=1
        FALLBACK_BIN="rtsproxy-${VERSION_FULL}-linux-$BIN_ARCH"
    fi
    
    # 断言验证
    local PASS=1
    local ERR_MSG=""
    
    if [ "$BIN_ARCH" != "$EXP_BIN_ARCH" ]; then
        PASS=0
        ERR_MSG="${ERR_MSG} 架构映射错误: 预期 '$EXP_BIN_ARCH'，实际为 '$BIN_ARCH';"
    fi
    
    if [ "$USE_PRECOMPILED" -ne "$EXP_USE_PRECOMPILED" ]; then
        PASS=0
        ERR_MSG="${ERR_MSG} 预编译判定错误: 预期 '$EXP_USE_PRECOMPILED'，实际为 '$USE_PRECOMPILED';"
    fi
    
    if [ "$FALLBACK_TRIGGERED" -ne "$EXP_FALLBACK_TRIGGERED" ]; then
        PASS=0
        ERR_MSG="${ERR_MSG} Fallback触发错误: 预期 '$EXP_FALLBACK_TRIGGERED'，实际为 '$FALLBACK_TRIGGERED';"
    fi
    
    if [ "$FALLBACK_TRIGGERED" -eq 1 ] && [ "$FALLBACK_BIN" != "$EXP_FALLBACK_BIN" ]; then
        PASS=0
        ERR_MSG="${ERR_MSG} Fallback文件名错误: 预期 '$EXP_FALLBACK_BIN'，实际为 '$FALLBACK_BIN';"
    fi
    
    # 输出结果
    if [ "$PASS" -eq 1 ]; then
        printf "  [ PASS ] %-30s -> BIN_ARCH: %-12s, USE_PRECOMP: %s, FALLBACK: %s\n" \
               "$test_name" "$BIN_ARCH" "$USE_PRECOMPILED" "$FALLBACK_TRIGGERED"
    else
        printf "  [ FAIL ] %-30s -> %s\n" "$test_name" "$ERR_MSG"
        FAILED_COUNT=$((FAILED_COUNT + 1))
    fi
}

echo "=========================================================================="
echo "                   RTSP Proxy install.sh 逻辑单元测试                     "
echo "=========================================================================="

# 案例 1：标准 24.10 x86_64 设备
test_case "24.10-x86_64" "24.10.4" "x86_64" "false" "x86_64" "1" "0" ""

# 案例 2：23.05 mipsel_24kc 设备 (MT7621等软浮点)
test_case "23.05-mipsel_24kc" "23.05.2" "mipsel_24kc" "false" "mips32elsf" "0" "1" "rtsproxy-0.3.3-r2-linux-mips32elsf"

# 案例 3：25.12 aarch64 设备 (apk)
test_case "25.12-aarch64" "25.12.0" "aarch64_generic" "true" "arm64" "1" "0" ""

# 案例 4：22.03 arm_cortex-a9 设备 (硬浮点)
test_case "22.03-arm_cortex-a9" "22.03.5" "arm_cortex-a9" "false" "arm32v7hf" "0" "1" "rtsproxy-0.3.3-r2-linux-arm32v7hf"

# 案例 5：SNAPSHOT 软浮点大端 mips 设备
test_case "SNAPSHOT-mips_24kc" "SNAPSHOT" "mips_24kc" "false" "mips32sf" "0" "1" "rtsproxy-0.3.3-r2-linux-mips32sf"

# 案例 6：24.10 mipsel_74kc 硬浮点设备
test_case "24.10-mipsel_74kc" "24.10.1" "mipsel_74kc" "false" "mips32el" "1" "0" ""

# 案例 7：22.03 mipsel_74kc 硬浮点设备 (Fallback 到静态硬浮点)
test_case "22.03-mipsel_74kc" "22.03.1" "mipsel_74kc" "false" "mips32el" "0" "1" "rtsproxy-0.3.3-r2-linux-mips32el"

# 案例 8：21.02 riscv64 设备
test_case "21.02-riscv64" "21.02.0" "riscv64" "false" "riscv64" "0" "1" "rtsproxy-0.3.3-r2-linux-riscv64"

# 案例 9：25.12 loongarch64 设备
test_case "25.12-loongarch64" "25.12.0" "loongarch64" "true" "loong64" "1" "0" ""

# 案例 10：24.10 arm_cortex-a5 硬浮点设备 (vfpv4)
test_case "24.10-arm_cortex-a5" "24.10.1" "arm_cortex-a5_vfpv4" "false" "arm32hf" "1" "0" ""

# 案例 11：23.05 通用无浮点 arm 设备 (软浮点保底)
test_case "23.05-generic-arm" "23.05.1" "arm" "false" "arm32" "0" "1" "rtsproxy-0.3.3-r2-linux-arm32"

# 案例 12：环境异常无 DISTRIB_ARCH 导致回退 uname -m 设备
test_case "Fallback-uname-m-mipsel" "23.05" "" "false" "mips32elsf" "0" "1" "rtsproxy-0.3.3-r2-linux-mips32elsf"

echo "=========================================================================="
if [ "$FAILED_COUNT" -eq 0 ]; then
    echo "  单元测试结论: 所有案例均通过！ [ SUCCESS ]"
    exit 0
else
    echo "  单元测试结论: 存在 $FAILED_COUNT 个失败案例！ [ FAILURE ]"
    exit 1
fi
