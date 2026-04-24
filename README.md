# rtsproxy

一款具有 NAT 穿越功能的 RTSP 代理工具，支持两种工作模式：

- **HTTP 模式**：客户端通过 HTTP 请求访问代理，代理将 RTP 流以 HTTP 响应的形式返回。
- **RTSP MITM 模式**：客户端直接使用 `rtsp://` 连接代理，代理透明地转发 RTSP 信令和 RTP/RTCP 数据包，对客户端完全透明。

## 使用说明

### nat 环境下接收 rtp over udp 数据

启动时附加参数 `--enable-nat` 开启，原理是向 STUN 服务器发送请求，获取 NAT 映射 WAN 侧的端口，将这个端口写在 RTSP 协议的 SETUP 命令里面。

---

## HTTP 模式

### /rtp

标准 RTSP 代理，将上游 RTSP 流通过 HTTP 透传给客户端（裸 TS 流）。

**示例：**

| 项目 | 地址 |
|---|---|
| rtsproxy 地址 | `http://192.168.0.3:8554` |
| 目标 RTSP 地址 | `rtsp://a.b.c.d:554` |
| 访问地址 | `http://192.168.0.3:8554/rtp/a.b.c.d:554` |

### /tv

用于将回放地址与直播地址的 URI 统一，以便在某些不支持 m3u catchup 参数的播放器看回放。

使用正则将客户端发来的地址替换为正确的回看地址，再去请求上游。

需要手动配置 JSON 文件替换规则，例如：

将 `rtsp://a.b.c.d:554/iptv/import/Tvod/iptv/001/001/channelName.rsc/abcde_Uni.sdp?tvdr=yyyyMMddHHmmss-yyyyMMddHHmmss` 替换为 `rtsp://a.b.c.d:554/iptv/Tvod/iptv/001/001/channelName.rsc?tvdr=yyyyMMddHHmmssGMT-yyyyMMddHHmmssGMT`，配置如下：

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

action 支持 remove、replace、timeshift，应该能覆盖各种场景（?）

match 可以写 `{number}` `{word}` `{any}`，避免手动写正则

---

## RTSP MITM 模式

通过 `--rtsp-port` 参数启用，代理监听一个标准 RTSP 端口，客户端像连接真实 RTSP 服务器一样直接连接代理，代理透明转发所有 RTSP 信令并中继 RTP/RTCP UDP 数据包。

**工作原理：**

```
客户端 ──── rtsp://proxy:port/real-host:port/path ────► rtsproxy
                               │
               从 URL 路径中提取真实服务器地址
               改写 Transport 头（中继 RTP 端口）
                               │
                               ▼
                         真实 RTSP 服务器
                               │
                         RTP/RTCP UDP 数据
                         双向中继转发给客户端
```

**URL 格式：**

```
rtsp://<proxy-ip>:<rtsp-port>/<real-host>:<real-port>/<path>
```

**示例：**

| 项目 | 值 |
|---|---|
| 程序运行地址 | `10.1.0.6`，RTSP MITM 端口 `8555` |
| 目标 RTSP 地址 | `rtsp://112.245.125.44:1554/iptv/import/Tvod/iptv/001/001/ch12122514263996485740.rsc/69958_Uni.sdp` |
| 访问代理地址 | `rtsp://10.1.0.6:8555/112.245.125.44:1554/iptv/import/Tvod/iptv/001/001/ch12122514263996485740.rsc/69958_Uni.sdp` |

启动命令：

```bash
./rtsproxy --rtsp-port 8555
```

> 注意：监听 554 端口通常需要 root 权限或 `CAP_NET_BIND_SERVICE` 能力。可使用任意非特权端口（如 `--rtsp-port 8555`）代替。

---

## 命令行参数

```
Options:
  -p, --port            <port>  Set HTTP server port (default: 8554)
      --rtsp-port        <port>  Enable RTSP MITM proxy on this port (e.g. 554)
  -n, --enable-nat              Enable NAT (default: disabled)
  -r, --rtp-buffer-size <size>  Set RTP buffer size (default: 4096)
  -u, --udp-packet-size <size>  Set UDP packet size (default: 1500)
  -t, --set-auth-token  <token> Set auth token (default: no auth required)
  -i, --set-interface   <iface> Set upstream network interface
  -j, --set-json-path   <path>  Set JSON file path (default: config.json)
  -d, --daemon                  Run rtsproxy in the background
  -k, --kill                    Kill the running rtsproxy instance
      --set-stun-host   <host>  Set STUN server host (default: stun.l.google.com)
      --set-stun-port   <port>  Set STUN server port (default: 19302)
```


