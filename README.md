# rtsproxy
一款具有 NAT 穿越功能的 RTSP 代理工具。

## 使用说明

### nat 环境下接收 rtp over udp 数据

启动时附加参数`--enable-nat` 开启，原理是向 stun服务器发送请求，获取 nat 映射 wan 侧的端口，将这个端口写在 RTSP 协议的 SETUP 命令里面。

### /rtp

标准 rtsp 代理，使用示例：

rtsproxy：`http://192.168.0.3:8554`

要代理的目标地址：`rtsp://a.b.c.d:554`

访问代理地址：`http://192.168.0.3:8554/rtp/a.b.c.d:554`

### /tv

用于将回放地址与直播地址的 URI 统一，以便在某些不支持 m3u catchup 参数的播放器看回放，一般来说，回放与直播地址不同的，query 参数为`?tvdr={utc:YmdHMS}GMT-{utcend:YmdHMS}GMT"`，所以只处理了这种情况。

实现方式是，通过指定的 JSON 文件，映射 channelID 到实际播放地址，例如：

rtsproxy：`http://192.168.0.3:8554`

要代理的目标频道 ID：`ch123456789`

访问代理地址：`http://192.168.0.3:8554/tv/ch1234566789`

配置 JSON 映射文件格式:

```json
[
  {
    "ChannelID": "ch123456789",
    "uni_live": "rtsp://a.b.c.d:554",
    "uni_playback": "rtsp://a.b.c.d:554?tvdr={utc:YmdHMS}GMT-{utcend:YmdHMS}GMT"
  }
]
```

## 命令行参数

```bash
Options:
  -p, --port            <port>  Set server port (default: 8554)
  -n, --enable-nat              Enable NAT (default: disabled)
  -r, --rtp-buffer-size <size>  Set RTP buffer size (default: 4096)
  -u, --udp-packet-size <size>  Set UDP packet size (default: 1500)
  -t, --set-auth-token  <token> Set auth token (default: no auth required)
  -j, --set-json-path   <path>  Set JSON file path (default: iptv.json)
      --set-stun-host,  <port>  Set STUN server host (default: stun.l.google.com)
      --set-stun-port,  <port>  Set STUN server port (default: 19302)
```


