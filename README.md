# rtsproxy

一款高性能、多模式的 RTSP 媒体代理工具。支持统一端口（8554）分发 HTTP 和 RTSP 流量，具备 URL 重写、时间偏移（Timeshift）以及 NAT 穿越功能。

---

## 核心特性

- **统一端口**：HTTP 代理与 RTSP MITM 代理共用 8554 端口，自动识别协议。
- **URL 重写**：支持通过正则规则重写上游地址，支持 `/rtp` 和 `/tv` 路径。
- **多模式支持**：
    1. **HTTP 代理模式** (支持 STUN 打洞)
    2. **RTSP MITM 模式** (高性能透明中继)

---

## 工作模式详解

### 1. HTTP 代理模式 (推荐用于 NAT 环境)
客户端通过 HTTP 协议请求代理，代理将上游 RTSP 流转换为裸 TS 流通过 HTTP 返回。

- **STUN 支持**：**支持**。通过 `--enable-nat` 开启，适用于代理服务器位于 NAT 后的环境。
- **访问格式**：
    - `http://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>`
    - `http://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>` (支持规则重写)

### 2. RTSP MITM 模式 (推荐用于内网透明中继)
客户端直接使用 RTSP 协议连接代理。代理透明转发所有信令，并对 RTP/RTCP 数据包进行双向中继。

- **STUN 支持**：**不支持**。适用于代理服务器拥有公网 IP 或与上游服务器在同一内网的环境。
- **访问格式**：
    - `rtsp://<proxy-ip>:8554/rtp/<real-host>:<real-port>/<path>`
    - `rtsp://<proxy-ip>:8554/tv/<real-host>:<real-port>/<path>` (同步支持 HTTP 模式的重写规则)

---

## 配置说明 (config.json)

用于 `/tv` 路径下的 URL 自动转换。支持 `remove`、`replace` 和 `timeshift`（时间偏移）。

```json
{
  "replace_templates": [
    {
      "action": "remove",
      "match": "/{number}_Uni.sdp",
      "description": "删除冗余字段"
    },
    {
      "action": "replace",
      "match": "/iptv/import",
      "replacement": "/iptv",
      "description": "替换 /iptv/import 为 /iptv"
    },
    {
      "action": "replace",
      "match": "tvdr={number}-{number}",
      "replacement": "tvdr={number}GMT-{number}GMT",
      "description": "补全回看地址"
    },
    {
      "action": "timeshift",
      "match": "tvdr={number}GMT-{number}GMT",
      "shift_hours": -8,
      "description": "将时间区间前移8小时"
    }
  ]
}
```

---

## 命令行参数

```bash
Options:
  -p, --port            <port>  设置代理主端口 (默认: 8554)
  -n, --enable-nat              开启 NAT 穿越 (仅 HTTP 模式有效)
  -r, --rtp-buffer-size <size>  设置 RTP 缓冲区包数量 (默认: 8192)
  -u, --udp-packet-size <size>  设置 UDP 包大小基准 (默认: 1500)
  -i, --set-interface   <iface> 设置上游出接口
  -j, --set-json-path   <path>  设置规则配置文件路径 (默认: config.json)
  -d, --daemon                  后台运行
  -w, --watchdog                开启自动重启模式
  -k, --kill                    杀死正在运行的实例
```

---

## 编译与运行

```bash
# 编译
cd build && ninja

# 运行
./rtsproxy
```
