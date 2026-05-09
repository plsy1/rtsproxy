# RTSProxy

**RTSProxy** 是一款高性能、多模式的 RTSP 媒体流分发中间件，提供双向 RTSP 透明代理与 HTTP 媒体流转换分发能力。它集成了动态 URL 路由重写、时间偏移（Timeshift）、NAT 穿越打洞及基于 DPI（深度报文检测）的流控优化，旨在为复杂网络环境提供稳健、灵活的流媒体转发解决方案。

---

## 部署指南

### 1. Docker 部署
```yaml
services:
  rtsproxy:
    image: ghcr.io/plsy1/rtsproxy:latest
    container_name: rtsproxy
    ports:
      - 8554:8554
    restart: always
```
执行 `docker compose up -d` 即可启动。

### 2. OpenWrt 一键安装
在 OpenWrt 终端执行：
```bash
wget -qO- https://raw.githubusercontent.com/plsy1/rtsproxy/main/install.sh | sh
```

安装脚本将自动部署二进制程序及 `luci-app-rtsproxy` 管理界面，安装完成后即可在 OpenWrt LuCI 菜单中直接进行图形化配置。

### 3. 本地构建 (Linux)
如果您需要在 Linux 环境下直接构建二进制程序，请确保已安装 `meson` 和 `ninja`：
```bash
meson setup build && ninja -C build
./build/rtsproxy
```

### 4. 构建 OpenWrt 安装包 (IPK)
项目内置了自动化交叉编译脚本，支持一键下载 SDK 并完成构建（默认为 x86_64 架构）：
```bash
chmod +x build_openwrt.sh
./build_openwrt.sh
```
编译生成的 `.ipk` 安装包将存放在 `bin/openwrt/` 目录下。

---

## 使用手册

### 1. 工作模式详解

#### **HTTP 代理模式 (RTSP to HTTP-TS)**
将上游 RTSP 流实时解复用并封装为 **MPEG-TS** 流，通过 HTTP 协议下发。适用于播放器兼容性要求高、需穿透复杂防火墙或进行 Web 播放的场景。
- **访问路径**：`http://<proxy-ip>:<port>/rtp/<upstream-host>:<port>/<path>`

#### **RTSP MITM 代理模式 (RTSP Relay)**
作为透明中继器转发 RTSP 信令，并对媒体流进行双向中继。它能自动处理 NAT 穿透并根据链路状况动态调整传输参数。
- **访问路径**：`rtsp://<proxy-ip>:<port>/rtp/<upstream-host>:<port>/<path>`

#### **管理后台 (WebUI)**
访问内置的监控面板，实时监控会话状态、码率及丢包率。
- **默认路径**：`http://<proxy-ip>:<port>/admin/`
- **鉴权示例**：`http://<proxy-ip>:<port>/admin/?token=your_token` (若开启了 `--auth-token`)

### 2. 动态 URL 重写 (`config.json`)
当访问路径以 `/tv/` 开头时，代理将根据规则自动变换上游地址。支持以下操作：

| 操作类型 | 说明 | 示例 |
| :--- | :--- | :--- |
| **`{number}`** | 匹配 URL 中的任意连续数字 | `channel_{number}` |
| **`remove`** | 移除特定路径片段 | 移除冗余的 `.sdp` 后缀 |
| **`replace`** | 字符串或模式替换 | `/iptv/import` -> `/iptv` |
| **`timeshift`** | **时间平移**：自动计算回看偏移 | `shift_hours: -8` (北京时间转 GMT) |

### 3. 命令行参数详解

```bash
Options:
  -p, --port            <port>  设置代理主端口 (默认: 8554)
  -n, --enable-nat              开启 NAT 穿越
      --nat-method      <method> 设置 NAT 穿越模式: stun, zte (默认: stun)
  -b, --buffer-pool-count <count> 设置 BufferPool 块数量 (默认: 8192)
  -s, --buffer-pool-block-size <size> 设置 BufferPool 块大小 (默认: 2048)
  -t, --auth-token      <token> 设置鉴权 Token (可选)
  -l, --listen-interface <iface> 设置服务监听网口 (下游)
      --http-interface  <iface> 设置 HTTP 模式上游网口
      --mitm-interface  <iface> 设置 MITM 模式上游网口
      --stun-host       <host>  设置 STUN 服务器地址 (默认: stun.l.google.com)
      --stun-port       <port>  设置 STUN 服务器端口 (默认: 19302)
  -c, --config          <path>  设置规则配置文件路径 (默认: config.json)
  -w, --watchdog                开启自动重启模式
  -d, --daemon                  后台运行
  -k, --kill                    杀死正在运行的实例
      --log-file        <path>  将日志写入指定文件
      --log-level       <level> 设置日志等级: error, warn, info, debug (默认: info)
      --strip-padding           开启 MPEG-TS 空包剥离 (带宽优化)
      --wait-keyframe           开启起播关键帧等待 (防止起播初始绿屏)
```

---

## 核心机制与实现

### 1. 高能 RTP 管道 (RtpPipeline)
- **高效内存管理**：基于 `BufferPool` 的预分配内存池实现**应用层零拷贝**，极大降低 CPU 负载与内存碎片。

### 2. 全自动协议自适应
- **下游自适应**：根据客户端 `SETUP` 请求中的 `Transport` 自动切换 UDP 或 TCP Interleaved 回传。
- **上游无感回退**：优先尝试 UDP 拉流，若遇到 `461 Unsupported Transport` 错误，代理将**自动拦截**并立即无感切换至 TCP 模式重试，对客户端完全透明。

### 3. DPI 媒体优化 (Deep Packet Inspection)
- **秒开优化 (`--wait-keyframe`)**：实时扫描 H.264/H.265 NAL 单元（SPS/PPS/VPS/IDR），确保从关键帧开始转发，杜绝起播瞬间的绿屏或花屏。
- **带宽压缩 (`--strip-padding`)**：DPI 实时识别并丢弃 MPEG-TS 中的空包（Null Packets），通常可节省 **10%-30%** 的下游带宽占用。

### 4. NAT 穿越与打洞技术
- **STUN 模式**：探测 WAN 口映射端口，解决标准 NAT 环境下上游 UDP RTP 流无法触达的问题。
- **运营商专网打洞 (ZTE)**：针对中兴等机顶盒协议，模拟 84 字节身份验证报文，在 RTP 通道上主动打通 NAT 映射路径。

---

## OpenWrt 深度适配

本项目为 OpenWrt 提供了完整的 LuCI 控制面板支持。

- **一键安装**：见 [快速开始](#2-openwrt-一键安装)。
- **UCI 配置**: `/etc/config/rtsproxy`。
- **专家模式**：开启 `use_external_config` 后，程序将忽略 UCI 参数，直接读取 `/etc/rtsproxy/config.json`。

---
> **LICENSE**: 本项目遵循开源协议。欢迎提交 Issue 或 Pull Request。
