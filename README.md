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

用于将回放地址与直播地址的 URI 统一，以便在某些不支持 m3u catchup 参数的播放器看回放。

使用正则将客户端发来的地址替换为正确的回看地址，再去请求上游。

需要手动配置 JSON 文件替换规则，例如：

将`rtsp://a.b.c.d:554/iptv/import/Tvod/iptv/001/001/channelName.rsc/abcde_Uni.sdp?tvdr=yyyyMMddHHmmss-yyyyMMddHHmmss`替换为`rtsp://a.b.c.d:554/iptv/Tvod/iptv/001/001/channelName.rsc?tvdr=yyyyMMddHHmmssGMT-yyyyMMddHHmmssGMT`，配置如下：

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

match 可以写 {number} {word} {any}，避免手动写正则

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

