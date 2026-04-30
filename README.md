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
    - `http://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>`
    - `http://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>` (支持规则重写)

### 2. RTSP MITM 模式
客户端直接使用 RTSP 协议连接代理。代理透明转发所有信令，并对 RTP/RTCP 数据包进行双向中继。

- **访问格式**：
    - `rtsp://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>`
    - `rtsp://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>` (同步支持 HTTP 模式的重写规则)

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

## 配置说明 (config.json)

用于 `/tv` 路径下的 URL 自动转换。支持语法 `remove`、`replace` 和 `timeshift`（时间偏移）。

具体参考 [config.json](./config.json)

---

## 命令行参数

```bash
Options:
  -p, --port            <port>  设置代理主端口 (默认: 8554)
  -n, --enable-nat              开启 NAT 穿越
      --nat-method      <method> 设置 NAT 穿越模式: stun, zte (默认: stun)
  -r, --rtp-buffer-size <size>  设置 RTP 缓冲区包数量 (默认: 8192)
  -u, --udp-packet-size <size>  设置 UDP 包大小基准 (默认: 2048)
  -t, --set-auth-token  <token> 设置鉴权 Token (可选)
  -l, --listen-interface <iface> 设置服务监听网口 (下游)
      --http-interface  <iface> 设置 HTTP 模式上游网口
      --mitm-interface  <iface> 设置 MITM 模式上游网口
      --set-stun-host   <host>  设置 STUN 服务器地址 (默认: stun.l.google.com)
      --set-stun-port   <port>  设置 STUN 服务器端口 (默认: 19302)
  -j, --set-json-path   <path>  设置规则配置文件路径 (默认: config.json)
  -w, --watchdog                开启自动重启模式
  -d, --daemon                  后台运行
  -k, --kill                    杀死正在运行的实例
      --log-file        <path>  将日志写入指定文件 (留空则输出到标准输出)
      --log-lines       <count> 设置日志滚动行数 (默认: 10000)
      --log-level       <level> 设置日志等级: error, warn, info, debug (默认: info)
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
> **多接口支持**：现在你可以为 HTTP 模式和 MITM 模式分别指定上游网口（例如分别走两个不同的 IPTV 专网），并可以限定服务只在特定的本地网口（如 `br-lan`）监听。

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

