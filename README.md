# rtsproxy

一款高性能、多模式的 RTSP 媒体代理工具。支持统一端口分发 HTTP 和 RTSP 流量，具备 URL 重写、时间偏移（Timeshift）以及 NAT 穿越功能。

---

## 核心特性

- **统一端口**：HTTP 代理与 RTSP MITM 代理共用端口，自动识别协议。
- **URL 重写**：支持通过正则规则重写上游地址，支持 `/rtp` 和 `/tv` 路径。
- **多模式支持**：
    1. **HTTP 代理模式**
    2. **RTSP MITM 模式**
- **协议自适应**：
    - **下游自适应**：根据客户端 SETUP 请求自动选择 UDP 或 TCP (Interleaved) 回传。
    - **上游故障回退**：向上游拉流优先尝试 UDP，若失败（如 461 错误）自动无感回退至 TCP。

---

## 工作模式详解

### 1. HTTP 代理模式
客户端通过 HTTP 协议请求代理，代理将上游 RTSP 流转换为裸 TS 流通过 HTTP 返回。

- **访问格式**：
    - `http://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>[?token=xxx]`
    - `http://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>[?token=xxx]` (支持规则重写)

### 2. RTSP MITM 模式
客户端直接使用 RTSP 协议连接代理。代理透明转发所有信令，并对 RTP/RTCP 数据包进行双向中继。

- **访问格式**：
    - `rtsp://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>[?token=xxx]`
    - `rtsp://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>[?token=xxx]` (同步支持 HTTP 模式的重写规则)

### 3. 管理后台 (WebUI)
通过浏览器访问内置的监控面板，实时查看会话状态和流量统计。

- **访问格式**：
    - `http://<proxy-ip>:8554/admin/[?token=xxx]`

---

## 协议传输机制

本项目在传输层具备极强的健壮性，能够自动处理复杂的网络环境。

### 1. 下游回传 (Proxy -> Client)
代理会根据客户端 `SETUP` 请求中的 `Transport` 报头自动决策：
- 如果客户端请求 `RTP/AVP`（如 `ffplay -rtsp_transport udp`），代理通过 **UDP** 回传数据。
- 如果客户端请求 `RTP/AVP/TCP`（如 `ffplay -rtsp_transport tcp`），代理通过 **TCP 交织 (Interleaved)** 模式回传数据。

### 2. 上游拉流 (Proxy -> Upstream Server)
代理对上游的连接采取“**优先 UDP，故障回退**”策略：
- **第一阶段**：代理首先尝试以 **UDP** 模式向上游发起 `SETUP`。
- **第二阶段 (自动回退)**：如果上游服务器返回 `461 Unsupported Transport`（不支持 UDP），代理会**自动拦截**该错误并立即改用 **TCP 交织** 模式重新发起请求。
- **透明性**：整个回退过程对客户端完全透明，确保在任何网络环境下都能成功拉流。

---

## 配置说明

通过 `config.json` 文件，可以灵活定义 URL 重写规则和上游安全黑名单。

### 1. URL 重写规则 (`replace_templates`)
当访问路径以 `/tv/` 开头时，代理会应用这些规则。支持以下操作语义：

*   **占位符 `{number}`**：匹配 URL 中的任意连续数字。
*   **`remove` (删除)**：匹配并移除 URL 中的特定片段。
    *   *示例*：移除冗余的 `.sdp` 后缀。
*   **`replace` (替换)**：
    *   *普通替换*：将 `/iptv/import` 简化为 `/iptv`。
    *   *模式补全*：将 `tvdr={number}-{number}` 转换为带 GMT 后缀的标准格式。
*   **`timeshift` (时间平移)**：
    *   专门用于回看地址。解析 `tvdr=` 中的时间戳，并根据 `shift_hours` 参数进行偏移。
    *   *示例*：`shift_hours: -8` 可将北京时间转换为 GMT 时间。

### 2. 安全黑名单 (`blacklist`)
定义禁止连接的上游地址段，防止代理被滥用或产生环回请求。支持：

*   **CIDR 网段**：如 `"127.0.0.0/8"`, `"10.0.0.0/8"`。
*   **域名通配符**：如 `"*.evil.com"`。
*   **精确匹配**：如 `"8.8.8.8"`。

### 3. 带宽优化 (`--strip-padding`)
针对 IPTV 场景中的 CBR（恒定比特率）视频流进行优化。

*   **自动剥离空包**：识别并丢弃 MPEG-TS 流中 PID 为 `0x1FFF` 的 Null Packets。
*   **带宽节省**：通常可降低 10%-30% 的下游带宽需求，显著减少网络卡顿。
*   **低延迟**：采用原地内存压缩算法，几乎不增加额外延迟。
*   **控制开关**：该功能默认关闭。如果需要优化带宽，可使用 `--strip-padding` 开启该逻辑。

### 4. 起播优化 (关键帧等待)
为了解决直播流起播瞬间可能出现的绿屏或花屏问题：

*   **关键帧等待**：代理在接收到播放请求后，可以选择丢弃第一个关键帧（I-Frame）之前的蓝帧或非完整帧。
*   **控制开关**：该功能默认关闭。如果需要防止起播绿屏，可使用 `--wait-keyframe` 开启该逻辑。

> [!NOTE]
> 环回检测是内置的：即使未配置黑名单，代理也会自动拒绝指向其自身监听地址的递归请求。

---

## 命令行参数

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
      --log-file        <path>  将日志写入指定文件 (留空则输出到标准输出)
      --log-lines       <count> 设置日志滚动行数 (默认: 10000)
      --log-level       <level> 设置日志等级: error, warn, info, debug (默认: info)
      --strip-padding           开启 MPEG-TS 空包剥离 (带宽优化)
      --wait-keyframe           开启起播关键帧等待 (防止起播初始绿屏)
```

## NAT 穿越与打洞功能

为了解决代理服务器或终端设备处于 NAT 防火墙（如光猫、拨号网关）后无法接收到上游 UDP RTP 视频流的问题，本项目提供了两种穿越机制：

### 1. STUN 模式 (`--nat-method stun`)
* **机制**：在 RTSP `SETUP` 协商前，使用 STUN 协议探测当前上游连接端口在 NAT 上的外部映射端口（WAN Port）。
* **用途**：适配公网标准流媒体服务器，确保 `Transport` 头中填入的是真正可触达的外部打洞端口。

### 2. 中兴 IPTV 打洞模式 (`--nat-method zte`)
* **机制**：模拟中兴机顶盒（ZTE STB）协议：
  * 在协商出的 RTP 传输通道上,主动向上游发送 **84 字节的身份验证报文**，打通 NAT 映射路径。
* **用途**：适配中兴设备为客户端的 IPTV 运营商专网环境。

> [!TIP]
> **鉴权机制**：
> 启用 `--auth-token` 后，所有的 HTTP API 调用、WebUI 访问以及 RTSP 初始连接均需在 URL 中附加 `token=你的Token` 参数。静态资源（JS/CSS）会自动放行以确保页面正常加载。

> [!TIP]
> **多接口支持**：现在你可以为 HTTP 模式 and MITM 模式分别指定上游网口（例如分别走两个不同的 IPTV 专网），并可以限定服务只在特定的本地网口（如 `br-lan`）监听。

---

## 编译与运行

```bash
# 编译
cd build && ninja

# 运行
./rtsproxy
```

---

## Docker 部署

本项目提供官方 Docker 镜像，托管于 GitHub Container Registry (GHCR)。

### 1. 使用 Docker Compose (推荐)

在项目根目录下创建一个 `compose.yml` 文件：

```yaml
services:
  rtsproxy:
    image: ghcr.io/plsy1/rtsproxy:latest
    container_name: rtsproxy
    ports:
      - 8554:8554
    restart: always
    # 如需传递启动参数（如开启 NAT 穿越），取消下行注释
    # command: "--enable-nat"
```

**启动服务：**
```bash
docker compose up -d
```

### 2. 命令行启动

```bash
docker run -d \
  --name rtsproxy \
  -p 8554:8554 \
  --restart always \
  ghcr.io/plsy1/rtsproxy:latest
```

---

## OpenWrt 支持

本项目深度适配 OpenWrt 系统，提供了完整的 LuCI 控制面板以及自动化安装方案。

### 1. 一键安装 (推荐)

在 OpenWrt 终端执行以下命令，脚本会自动识别架构并安装最新版本（包含 LuCI 界面）：

```bash
wget -qO- https://raw.githubusercontent.com/plsy1/rtsproxy/main/install.sh | sh
```

### 2. 手动安装

你可以从 [Releases](https://github.com/plsy1/rtsproxy/releases) 页面下载预编译好的安装包：

*   **luci-app-rtsproxy_all.ipk**: 通用界面包（必装）。
*   **rtsproxy_openwrt-xxx-xxx.ipk**: 针对特定 OpenWrt 版本（如 23.05, 24.10）和架构（x86_64, aarch64）的程序包。
*   **rtsproxy-xxx-linux-xxx**: 针对其他架构的静态二进制文件（由安装脚本自动调用）。

### 3. 自行编译

1.  将本项目源码放入 OpenWrt SDK 或源码树的 `package/rtsproxy` 目录。
2.  运行 `make menuconfig`，选中：
    *   `Network -> rtsproxy`
    *   `LuCI -> 3. Applications -> luci-app-rtsproxy`
3.  执行编译：`make package/rtsproxy/compile V=s`

### 配置文件
*   **UCI 配置**: `/etc/config/rtsproxy`
*   **规则配置**: `/etc/rtsproxy/config.json`
*   **启动脚本**: `/etc/init.d/rtsproxy`

> [!IMPORTANT]
> **专家模式 (Expert Mode)**：
> 在 OpenWrt 中，你可以开启 `use_external_config` 选项。开启后，程序将**完全忽略** UCI 的参数，直接读取 `/etc/rtsproxy/config.json` 中的设置。这对于需要进行复杂 URL 重写或精细化配置的用户非常有用。

