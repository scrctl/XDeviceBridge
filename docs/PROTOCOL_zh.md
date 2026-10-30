# xdb 本机通信协议（版本 1）

[English](PROTOCOL.md) | 简体中文

这是**客户端到宿主服务端的协议**，不是 Apple 设备协议。
Server 只监听 IPv4 回环地址，本机客户端无需认证。
回环地址限制直接网络访问，但不会对客户端进程或操作系统用户鉴权。
能够连接该端口的本机进程可调用设备控制、剪贴板和停止服务等操作。
手机配对与设备认证由服务端另行处理，不会单独授权本机客户端。

## 版本与升级策略

软件包版本、C ABI（1）和通信协议（1）是不同契约。
帧头和 `hello.params.protocol` 的协议号都必须为 1。
不能仅因软件包版本不同就终止或替换现有 Server。

XDB 1.0 确立文档所述的 C ABI 1 和通信协议 1 基线。
兼容性承诺适用于已发布版本，不适用于未发布的开发快照。

在协议 1 内：

- 新增可选请求字段时，省略字段必须保留原行为。
- 客户端须忽略未知结果字段；服务端可新增结果字段／事件。
  事件消费者须容忍不用的名称。必填字段和已有错误码保留文档语义；
  错误消息仅供诊断。
- 可以新增方法。客户端须处理 `unknown_method`／操作失败，
  不能因软件包版本看似较新就假设某方法存在。
- 删除方法／字段，改变类型、单位、默认含义或媒体正文布局，必须使用新协议号。
  所有权／生命周期变化也属于这一范围。
- 拒绝握手后关闭连接，不会自动替换监听者。应使用旧服务对应的可执行文件停止服务，
  然后启动所需版本。SDK 绝不自动终止已占用端口或不兼容的监听者。

C SDK 按 `xdb.h` 的约定持有不透明句柄和响应缓冲。
已发布 ABI 内，已有导出入口保持签名不变；新入口为增量扩展，调用方须检查可选符号。
协议兼容不表示 Apple 设备支持请求的功能。

C ABI 的不兼容变化必须使用新的 ABI 主版本和动态库 SOVERSION；
通信协议的不兼容变化必须使用新协议号并提供迁移说明。
使用者应分发经过测试的客户端／服务端组合，并保留旧包用于回滚。
安装保留配对数据；未来不兼容的磁盘格式迁移必须保留旧记录或提供明确的导出路径。

## 分帧

每条消息为 16 字节头部，随后是 UTF-8 JSON 和可选原始二进制正文：

| 偏移 | 大小 | 含义 |
| --- | --- | --- |
| 0 | 4 | ASCII `XDB1` |
| 4 | 4 | 版本 `1`，无符号大端整数 |
| 8 | 4 | JSON 字节数，无符号大端整数，1..1 MiB |
| 12 | 4 | 二进制字节数，无符号大端整数，0..64 MiB |

读取必须兼容 TCP 拆包和粘包。JSON 嵌套深度限制为 64 层。
错误分帧会在分配超大负载之前关闭连接。客户端请求当前不携带二进制正文。
不要用换行符分隔消息。

## 握手与调用

首个请求必须是：

```json
{"id":1,"method":"hello","params":{"protocol":1}}
```

协议版本或 hello 方法错误会关闭连接。库会自动完成握手。
之后每次发送一个请求，使用唯一的无符号整数 `id` 和对象 `params`：

```json
{"id":2,"method":"server.info","params":{}}
```

响应为 `{"id":2,"ok":true,"result":{...}}` 或
`{"id":2,"ok":false,"error":{"code":"...","message":"..."}}`。
请求期间可在最终响应之前收到零个或多个
`{"id":2,"event":"progress","data":{...}}`。
配对码事件使用 `event: "pin"`、`data: {"pin":"123456"}`，不得记入日志。
服务端不会在响应、事件或发现结果中返回设备凭据。

关闭客户端套接字会在支持的位置取消配对／DDI 操作，并脱离该客户端的会话；
其他附着客户端可继续保持会话。手动连接的基础隧道不随客户端会话清理关闭。
取消或失败的操作可能已经改变设备状态。
客户端可从另一线程中断套接字，须等请求结束后才能销毁客户端。
事件回调不能重入或销毁自己的客户端。

## 方法

完整字段、默认值、结果、错误和状态参考见 [API_zh.md](API_zh.md)；
C 符号所有权、线程和接口参考见 [C_API_zh.md](C_API_zh.md)。
下表是摘要，不代替完整的方法契约。

| 方法 | 参数 | 结果／二进制 |
| --- | --- | --- |
| `server.info` | 无 | 协议号、服务端版本和进程 ID |
| `server.stop` | 无 | 回复后请求关闭；取消全部会话 |
| `devices` | `transport`：auto/usb/wifi；`timeout_ms`：0..60000 | 设备、候选项、警告 |
| `mdns.check` / `mdns.services` | 可选 `timeout_ms`: 1..60000 | 发现后端、可用性、警告；services 另列出原始服务端点 |
| `connect` | `address`，可选 `port`、`udid`、`timeout_ms` | Server 持有的已认证 Wi-Fi 基础连接；不需要发现 |
| `disconnect` | 可选 `address`、`port`、`udid` | 关闭匹配的手动连接并使附着会话失效；保留凭据和 DDI |
| `pair` | `transport`：wifi/usb；可选 `udid`；`timeout_ms`：1..300000 | UDID 和复用状态；进度／PIN 事件 |
| `ddi.download` | `os_version`，可选 `product_type`、`cache_directory`、`timeout_seconds` | 构建／缓存元数据；进度事件 |
| `ddi.prepare` | `udid`，可选 `wifi`、`port`、`cache_directory`、`replace`、下载／安装超时秒数 | 准备状态；进度事件 |
| `ddi.status` | `udid`，可选 `wifi`、`port` | 挂载状态和证据；独立查询的 DDI Cryptex 安装信息 |
| `ddi.remove` | `udid`，可选 `wifi`、`port` | 验证后的已移除状态，以及本次是否执行了移除 |
| `connection.open` | `udid`，可选 `wifi`、`port`、`access`：control/read | 控制会话或并行只读关联，按能力协商 |
| `connection.close` | 无 | 释放服务、输入和设备租约 |
| `device.info` | 无 | 设备 UDID、系统版本、型号 |
| `screenshot` | 可选 `format`：png/bgra（默认 png） | PNG 正文或解码后的 BGRA 像素，附显示几何 |
| `video.start` | 可选 `hardware_decode`、`max_fps`、`max_bitrate` | 启动后台最新帧采集 |
| `video.frame` | `since`：帧序号，`timeout_ms`：0..1000 | BGRA 格式、序号、宽高、行跨度、可选显示几何；原始像素 |
| `video.stop` | 无 | 停止视频 pump |
| `input.tap` | `x`、`y`：0..1；`hold_ms`：0..2000 | 报告发送状态 |
| `input.touch` | `x`、`y`：0..1；`down`：布尔值；可选 `contact`：整数 0..4，默认 0 | 指定触点的接触／释放报告发送状态 |
| `input.keyboard` | `usages`：USB HID 键盘用法码，各值 <240 | 替换按住的按键集合；`[]` 释放 |
| `input.button` | `page`（默认 12）、`usage`、`down` | consumer 硬件按键按下／抬起 |
| `input.text` | `text`：US 键盘 ASCII/Tab/换行，<=4096 字节；`timeout_ms`：1..300000（默认 30000） | 整体拒绝不支持的文本 |
| `app.list` | 无 | bundle 标识和名称 |
| `app.launch` | `bundle_id`，可选 `terminate_existing` | 请求状态 |
| `app.stop` | `bundle_id` | 请求状态 |

管理方法之外的操作需要设备会话。断开的套接字无法自行恢复；
只有另一个附着连接仍保持会话时，新套接字才能 attach。
多个连续操作应使用持久的库客户端或 `rpc-shell`。

### 手动 Wi-Fi 连接

`connect`／`disconnect` 从 1.2 起提供，通信协议仍为 1；通过
`server.info.capabilities.manual_connections` 探测。`connect` 必须使用已有配对记录，
成功后由 Server 保留认证后的隧道，不执行新配对或 DDI 准备。
已知 IP 不需要 mDNS；未指定 UDID 时仅允许选择唯一可用本地记录。
默认手机 RemotePairing 端口为 49152，与 CLI `--port` 指定的本机 Server 端口无关。

设备会话可复用手动连接，最后一个附着客户端关闭时释放媒体／输入服务，保留空闲隧道。
默认 control 接入仍独占。`devices` 按 UDID 合并发现与手动连接，后者在没有
广播时也列为 connected，附 `manual_connection:{address,port}`。
`disconnect` 不带筛选条件时关闭全部手动连接；指定端点与 UDID 时都必须匹配。
传输掉线或关闭 Server 也会关闭手动连接，不自动重连、不持久化连接地址。
传输掉线由空闲 TCP keepalive／实际 I/O 检测，检测耗时依操作系统而定。
关闭失效会话后可以显式重新 connect/open；旧 attachment 不再有效。
重新配对或管理 DDI 前须先断开手动连接。

### 并行只读关联（1.3 起）

`server.info.capabilities.read_only_sessions` 为 true 时，`connection.open` 可指定
`access:"read"`。读取客户端共享设备及既有视频的读取能力，不启动第二路视频。
只读 attachment 继承限制，不允许输入、媒体配置、消耗音频队列或全局管理修改。
最后一个控制客户端退出后媒体结束、控制 attachment 失效；读取客户端仍可截图，
新控制客户端可在没有拥有者时取得控制权。旧版 Server 可能忽略未知 access 字段，
调用方必须检查能力；CLI 截图自动协商，旧 Server 保留原行为。详见完整 API 契约。

共享控制另行通过 `shared_control_sessions` 能力及 `connection.open.access:"shared"` 协商。
独立控制组共用一条视频，各组退出仅释放自身输入，最后一组退出才停止视频。
视频参数必须一致；显式媒体关闭和录制保持独占，音频只有一个消费组。完整规则见 API_zh.md。

## 几何与媒体所有权

`devices` 为每台设备增加 `state`（`discovered`、`paired`、`connected`）、
`pairing`（`record_present` 或 `unknown`）和 `connection`（`connected` 或 `unverified`）。
connected 表示活跃、已认证的服务端会话，不是刚执行的存活探测，也不承诺全部设备服务正常。
候选项的 `pairing` 保留发现原因：
`record_present`、`incomplete_record`、`identifier_hint`、`ambiguous`、
`invalid_advertisement`、`crypto_error`、`unknown`。
已有候选项 `paired` 布尔值仍仅描述完整且匹配的本地记录。
列举请求不会主动连接或配对。

桌面 Server 在进程内共享原生发现订阅（Bonjour、可选 Avahi 或 Windows DNS-SD）。
已有快照时不再等满 `timeout_ms`；冷启动或空缓存可在该预算内等待。
系统回调更新和撤销记录，Server 不会周期性重启扫描。
缓存只存广播，每次请求独立匹配当前配对记录；最后一个 Server 关闭时释放订阅。
原生后端失败会清空快照，后续请求可重连或回退到直接 mDNS。
Android 继续使用 App 持有的 NSD 租约；缓存不写入磁盘。

仅 CLI 的设备选择允许 `screenshot`／`ddi prepare`／`ddi status`／`ddi remove`
在只发现一台已识别设备时省略 `-s`。SDK 的 DDI 方法要求显式传入非空 UDID。

`ddi.status` 独立查询挂载证据和 DDI Cryptex 的安装信息，不下载或安装。
`cryptex_installed:null` 表示设备未广播该服务；查询安装状态失败时返回错误。
`ddi.remove` 执行准备的反向操作：若存在 DDI Cryptex，按实际安装版本卸载；
否则取消 Classic／Personalized 镜像挂载。移除后复查状态，已经不存在时直接成功。
保留电脑缓存、配对记录和其他 Cryptex，不请求更新 nonce。
两者都需要独占设备租约，应先关闭活跃的设备会话。
`wifi` 为空时使用 USB，`wifi:"auto"` 使用已配对的 Wi-Fi 发现。
返回的系统镜像类别不代表当前镜像的构建身份。
报错或取消前设备状态可能已经改变，应先查询状态再决定是否重试移除。

空闲的 image-mounter 连接在销毁或重连前尝试发送 `Hangup`。
收尾最多等待一秒，响应取消，不覆盖已完成操作的结果。
传输失败或原始镜像上传未完成时直接关闭，不再发送协议命令。

触摸坐标针对设备面板坐标轴，不是经过旋转或裁剪的客户端窗口。
BGRA 帧可能包含编解码填充。`display` 元数据来自设备显示订阅，
包含宽、高、方向和几何序号。客户端将可见像素映射到面板坐标时，
须处理裁剪和旋转；不可编造缺失元数据。

响应持有像素，直到 `xdb_response_destroy`。
C ABI 中没有 FFmpeg、OpenCV、C++ 对象或 GPU 句柄。
未来若要协商编码媒体，须显式进行；当前实现不向客户端发送 HEVC/AAC 负载。

## 消费者会话与其他方法

`connection.open` 接受 `transport`（`auto`、`usb`、`wifi`），
返回随机且属于该连接的私密 `attachment` 能力凭据。
客户端可在另一条套接字上调用 `connection.attach`，
让音频／视频轮询和控制独立执行。该能力凭据应保密。
会话和设备租约持续到最后一个附着连接关闭。
`connection.close` 只脱离请求它的客户端，不使其他附着连接失效。

- `display.info`：订阅显示更新，返回面板几何。
- `display.rotate`：查询当前基本方向，请求一次相反横竖屏，并验证返回状态。
  不解除设备方向锁策略，也不重试结果不明的修改。
- `clipboard.set`／`clipboard.get`：显式剪贴板访问，写入参数 `text`，最多 256 KiB。
  不作为文本输入的自动回退。
- `recording.configure`：媒体启动之前，提供服务端宿主绝对路径 `path`、
  `format`（`mp4`／`mkv`）、`video`、`audio`、`orientation`，
  以及可选 `orientation_from_device`。文件由 Server 写入；
  容器保留编码媒体，不是从 IPC 图像／音频正文重建。
- `video.start`：另接受 `decode`（默认 true）、`record_path`
  （原始 HEVC 的服务端绝对路径）和 `restart_on_silence`（默认 true）。
  只接收编码流时，须以经过检查的录制 IDR 确认就绪，调用才成功。
- `video.start.max_fps`：整数 0..240，默认 0／不限速，通过 `capabilities.video_max_fps` 协商。
  限制 BGRA 转换／复制和发布，不限制设备编码、参考帧解码或录制；运行中可以更新。
  共享组取最高需求（不限速优先），退出组后移除该组需求；启动／恢复首帧不受限。
  `media.status.video_max_fps` 返回实际上限，`video_rate_limited` 统计因限速跳过的像素发布次数。
- `video.start.max_bitrate`：uint64，单位 bit/s；新生产者以 0／省略表示设备默认值，
  通过 `capabilities.video_max_bitrate` 标示支持，原样请求，不按机型另行截断。
  第一条生产者的请求优先，后来的不同请求继续复用，返回 `warnings` 并记日志；省略则静默沿用。
  start 返回 `max_bitrate` 与 `negotiated_max_bitrate`（设备 `TXMaxBitrate` 或 null），
  status 对应字段为 `video_max_bitrate`／`video_negotiated_max_bitrate`。
- `video.frame`：`allow_cached` 允许等待到期后复用健康最新帧。
  几何随发布帧一起采集，不在之后读取。
- `screenshot`：`format: "bgra"` 请求服务端 PNG 解码；默认仍为 PNG。
  两者都包含几何；截图期间显示变化会失败。
- `audio.start`：`decode`、`duplicate`、`buffer_ms`（0..1000）；
  返回采样率、声道和预缓冲帧数。
  `audio.read` 请求 1..48000 个交错帧，返回原始**有符号 16 位小端** PCM 和实际帧数。
  应在独立的附着套接字上轮询，绝不在音频回调里发网络请求。
- `audio.stop`：只停止音频。`media.finish`：等待生产者、完成录制收尾，再停止设备音频。
  错误向调用方传播。
- `media.status`：终结性 `error` 和用于截图回退的 `video_unusable`。
- `network.debug`：`enabled` 控制服务端隧道诊断。

`devices` 包含 `capabilities.video_decode`；
Stream 可用性应取自 Server，而非客户端本地库。
C API 的端口 0 使用 `XDB_PORT` 或 17423。
桌面连接可自动启动缺失 Server；Android 连接要求 App 自行持有内嵌 Server。
`xdb_client_start_server` 显式确保服务运行，
`xdb_client_kill_server` 停止已有服务，不会启动它。
`include/xdb/client.hpp` 是可选、仅头文件的 C++ 适配器；二进制边界仍为 C API。
GUI 可直接通过该 API 使用 `pair`／`ddi.prepare`，设备凭据始终保留在服务端。

`media.status.recording_error` 单独报告录制错误，与传输／媒体 `error` 分离，
因此解码镜像可继续。`media.finish` 仍返回录制失败。
Server 保持请求的音频启动预缓冲；客户端只需小型输出队列，不应再次缓冲相同时长。

## 空闲连接限制

hello 必须在五秒内完成。握手后，不持有设备会话且无正在执行请求的连接会在空闲
60 秒后关闭；16 个工作槽位耗尽时，可立即回收此类空闲连接。未发送完整的请求不会
保留槽位。拥有或附着到设备会话的连接、正在执行的请求不因这项策略被淘汰。
帧开始接收后，数据块停顿的接收超时为五秒。空闲连接被关闭后，客户端须重新连接再发请求。

### 多点触控（Server 1.4.0 起）

`input.touch` 的 `down: true` 按下或移动指定 `contact`，`down: false` 只释放该触点。Server 将所有活动触点合成为完整 HID 报告；会话结束时释放全部触点。省略 `contact` 保持原单点调用方式。

多点调用方须先检查 Server 版本至少为 1.4.0：旧 Server 可能忽略未知的 `contact` 参数。本扩展不新增能力标记，不改变 C ABI 或 RPC 协议号。共享控制组持有整个触摸手势，全部触点抬起后才允许其他组接管；不将不同窗口的触点混成一个手势。
