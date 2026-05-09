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

> [!TIP]
> **静态资源放行**：开启鉴权后，代理会自动放行 WebUI 所需的 JS/CSS 资源，确保监控页面在未授权前也能正确加载基础框架。

### 2. 命令行参数详解

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

> [!TIP]
> **多网口绑定 (Multi-Interface Support)**：
> 你可以通过 `--listen-interface` 指定服务在特定的本地网口（如 `br-lan`）监听，同时通过 `--http-interface` 或 `--mitm-interface` 指定上游拉流流量走不同的物理网口（如 `eth1` 专网），实现真正的内外网隔离。

---

## 配置文件说明 (`config.json`)

`config.json` 是 RTSProxy 的核心配置文件，支持全局参数、URL 重写规则及安全黑名单设置。

### 1. 全局设置 (`settings`)

| 字段 | 类型 | 说明 | 默认值 |
| :--- | :--- | :--- | :--- |
| `port` | Number | 代理监听端口 | `8554` |
| `enable_nat` | Boolean | 是否开启 NAT 穿越 | `false` |
| `nat_method` | String | NAT 穿越模式 (`stun`, `zte`) | `stun` |
| `buffer_pool_count` | Number | 预分配内存池块数量 | `8192` |
| `buffer_pool_block_size` | Number | 每块内存的大小 (字节) | `2048` |
| `log_level` | String | 日志等级 (`error`, `warn`, `info`, `debug`) | `info` |
| `log_file` | String | 日志文件路径 (为空则输出至控制台) | `""` |
| `log_lines` | Number | 日志文件最大滚动行数 | `10000` |
| `strip_padding` | Boolean | 是否剥离 MPEG-TS 空包以节省带宽 | `false` |
| `wait_keyframe` | Boolean | 是否等待关键帧后再开始转发 (防绿屏) | `false` |
| `watchdog` | Boolean | 开启进程监控，崩溃后自动重启 | `false` |
| `daemon` | Boolean | 是否以守护进程方式后台运行 | `false` |
| `auth_token` | String | 访问管理后台或接口的鉴权 Token | `""` |
| `listen_interface` | String | 指定服务监听的本地网口 (如 `br-lan`) | `""` |
| `http_interface` | String | HTTP 模式拉流时使用的出口网口 | `""` |
| `mitm_interface` | String | MITM 模式拉流时使用的出口网口 | `""` |
| `stun_host` | String | STUN 服务器地址 | `stun.l.google.com` |
| `stun_port` | Number | STUN 服务器端口 | `19302` |

### 2. URL 重写规则 (`replace_templates`)

当请求路径匹配特定规则时，代理将自动变换上游地址。支持以下操作：

| 操作类型 (`action`) | 说明 | 示例配置项 |
| :--- | :--- | :--- |
| `remove` | 移除匹配的路径片段 | `match: "/.sdp"` |
| `replace` | 字符串或模式替换 | `match: "/iptv/import", replacement: "/iptv"` |
| `timeshift` | **时间平移**：自动偏移回看地址中的时间戳 | `shift_hours: -8` (用于 GMT 转换) |

**通配符支持**：
- `{number}`: 匹配任意连续数字（如频道 ID、时间戳）。

### 3. 安全黑名单 (`blacklist`)

包含一系列 CIDR 格式的 IP 地址段。代理将**拒绝**向这些地址发起上游连接，用于防止内网穿透攻击或递归环回死循环。

> [!NOTE]
> **递归环回检测**：即使未配置黑名单，RTSProxy 也会自动识别并拒绝指向其自身监听端口的请求。

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
