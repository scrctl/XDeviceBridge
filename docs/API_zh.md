# XDB API 参考——1.0

[English](API.md) | 简体中文

本文定义 XDB 1.0 的公开 RPC 接口约定，已对照 `xdb.h`、`Client.cpp`、`Server.cpp`、
`Backend.cpp` 和 `Discovery.h`。已发布版本遵守协议 1 的兼容性策略。
另见[通信分帧与兼容性](PROTOCOL_zh.md)、[安装](INSTALL_zh.md)、
[设备验收证据](VALIDATION.md)和 [C SDK 参考](C_API_zh.md)。

本协议用于本机调用方与 XDB Server 之间，而非 XDB 与 iOS 之间。
服务仅监听 IPv4 回环地址，不对客户端鉴权。本机客户端可以控制已配对设备，
并请求写入服务端宿主上的路径。不要将该端口转发到网络，也不要把回环地址当成
本机进程之间的授权边界。手机认证另行处理，凭据保留在 Server 中。

## 通用约定

- 请求：`{"id":1,"method":"...","params":{...}}`；每条套接字一次处理一个请求。
  SDK 自动分配 ID 并执行 `hello`。请求不携带二进制正文。
- 成功：`{"id":1,"ok":true,"result":...}`。除返回数组的 `app.list` 外，
  本文列出的结果均为对象。修改或停止操作未另行说明时返回 `{}`。
- 失败：`{"id":1,"ok":false,"error":{"code":"...","message":"..."}}`。
  `message` 是诊断信息，不是供字符串匹配的稳定契约。错误码见下文。
- 事件先于终结响应，使用相同 ID。忽略不认识的事件类型和字段。
  `progress.data` 为 `{message:string}` 或 `{file:string,bytes:uint,total:uint}`；
  总量未知时 `total` 可为零。`pin` 事件携带 `{pin:string}`，
  供用户在手机输入；不要持久化或记录配对码。
- 字符串使用 UTF-8，布尔值使用 JSON 布尔类型。下文整数范围均含端点。
  必填字段标为**必填**；不要依赖实现偶然接受文档支持范围之外的值。
- 所有路径均指向**服务端宿主**，而非客户端进程。保留 UTF-8 编码。
  录制路径必须是绝对路径；缓存路径也应使用绝对路径，避免依赖 Server 工作目录。
  路径不会作为 shell 命令执行。
- 输入 RPC 成功只确认报告已发送，不保证应用已接受输入。
  若需要确认实际界面状态，应另外查询或截图。
- `xdb_client_request` 没有统一的请求截止时间。可在工作线程调用，
  并通过 `xdb_client_interrupt` 中断；之后该句柄不可复用。
  断开连接会请求取消，但不能撤销已经完成的设备或文件修改。

## 会话状态与调用顺序

| 状态 | 可调用的方法 |
| --- | --- |
| SDK 已连接／hello 已完成，无设备会话 | server.info、server.stop、devices、pair、ddi.download、ddi.prepare、connection.open、connection.attach、connection.close |
| 已拥有或附着设备会话 | 上述方法及 device/display/input/app/clipboard/media 方法；同一连接再次 open/attach 会失败 |
| 已通过 connection.close 脱离 | 可再次调用管理方法；open/attach 可建立新的关联 |
| 已中断、传输失败或已销毁 | 丢弃客户端，使用新句柄重新连接 |

同一 UDID 同时只能有一个拥有者会话。其他套接字可通过私密的 `attachment` 字符串
共享会话。再次打开相同 UDID 返回 `device_busy`。
附着客户端具有相同控制权限，不是只读订阅者。
会话持续到**最后一个**附着连接关闭；之后结束媒体、尝试释放按住的输入并释放设备租约。
若手机或传输已经失败，清理不能保证释放报告成功送达。

附着连接共享媒体配置。应由一个控制客户端修改媒体状态，按需另设音频读取和视频轮询客户端。
不要让多个音频客户端竞争读取：它们会消费同一个 PCM 队列。
共享会话锁用于防止读取期间拆除生产者，不是多个控制客户端之间的事务机制。

## 错误码与恢复

| 错误码 | 含义／通常的处理方式 |
| --- | --- |
| handshake_rejected | 首个方法或协议不正确；服务端关闭套接字。使用兼容的客户端和服务端。 |
| invalid_params | JSON 字段缺失、类型错误、数值越界或显式参数校验失败。修正请求；连接可能仍可使用。 |
| cancelled | 后端分发前检测到取消；断连期间的取消也可能表现为 EOF／无响应。 |
| operation_failed | 设备、发现／连接、配对、DDI、编解码、录制或其他后端失败。message 说明本次原因，尚无稳定子错误码。 |
| device_busy | 其他会话或配对操作持有设备租约。先关闭该会话及其附着连接。 |
| invalid_state | 当前连接或媒体状态不能执行该方法。修正调用顺序。 |
| invalid_session | attachment 已失效或未知。重新打开会话。 |
| not_connected | 需要设备会话的方法尚未 open/attach。先建立关联。 |
| ambiguous_device | 空 UDID 自动选择时找到多台已识别设备。明确指定 UDID。 |
| device_not_found | 空 UDID 自动选择时未找到已识别设备。刷新发现或选择设备。 |
| geometry_changed | 截图期间显示几何发生变化。待稳定后重试；不要使用过时几何映射输入。 |
| unsupported | 构建缺少所需解码能力，或 input.text 含不支持字符。改用支持的能力／路径。 |
| no_frame | 等待期间没有更新的视频帧。继续轮询或检查 media.status。 |
| unknown_method | 分发到未实现的方法。协商或使用受支持的方法。 |

错误优先级取决于分发顺序。例如，无设备会话时调用未知设备方法，可能先返回
`not_connected`，而不是 `unknown_method`；未知 `input.*` 方法还可能先初始化 HID 服务。
不要用未知调用做无副作用的能力探测；发现接口会显式报告视频解码能力。

错误分帧、超大消息和无效请求封装会关闭套接字，不保证返回 RPC 错误。
收到合法 RPC 错误不等于传输失败。C API 返回值区分“收到响应”和 RPC 的 `ok`；
详见 [C SDK](C_API_zh.md)。

## 管理

### `hello`

仅供直接使用通信协议的客户端调用；须在五秒内发出首个请求。
`protocol` **必填**，整数 `1`。返回 `{protocol:1,version:string}`，其中 version 是软件包版本。
协议或方法错误时，尽力发送 `handshake_rejected` 后关闭套接字。
帧头版本也必须为 1。SDK 用户不要再次发送 hello。

### `server.info`

不需要参数或设备会话。返回 `{protocol:1,version:string,pid:integer,device_transport:"usb-or-wifi"}`。
`version` 是 CMake 软件包版本，不能替代协议协商，也不能证明设备已连接。

### `server.stop`

不需要参数或设备会话。先发送结果 `{}`，再请求关闭服务。
所有客户端和会话被取消；不会取消手机配对或删除用户文件。
收到响应不证明进程已经退出。`xdb_client_kill_server` 还会等待端口关闭。
内嵌服务的持有方仍须调用 `xdb_server_stop` 释放服务句柄。

### `devices`

| 参数 | 类型／默认值 |
| --- | --- |
| transport | 字符串：auto（默认）、usb、wifi |
| timeout_ms | 整数 0..60000；默认 3000；是发现扫描预算，不是整个 RPC 的统一截止时间 |

返回 `{devices:Device[],warnings:string[],cancelled:bool,capabilities:{video_decode:bool}}`。
部分发现可在带有警告或 `cancelled:true` 时成功；应检查这些字段，不能直接把空列表当成最终结论。
不会认证每台列出的设备，也不会主动配对或连接。
桌面 Server 在共享的原生发现快照就绪时直接读取；超时限制冷启动或空缓存的等待，
不要求每次扫描等满该时长。缓存归属见[协议文档](PROTOCOL_zh.md)。

`Device`：`udid:string`、`name:string`（可为空）、`candidates:Candidate[]`、
`state:discovered|paired|connected`、`pairing:record_present|unknown`、
`connection:connected|unverified`。无法识别的广播可能没有 UDID；
应保留候选详情，不要编造身份。`connected` 仅表示活跃 XDB 会话，不保证手机的全部服务可用。

`Candidate`：`transport:usb|wifi`、`address:string`、`port:integer`、
`interface:string`、`connection_type:string`、`paired:bool`、`pairing:string`。
USB 地址／接口可为空，端口可为零；Wi-Fi IPv6 地址可能包含作用域。
保留所有候选项；同一 UDID 可对应多个接口和地址。
`paired` 表示完整且匹配的本地凭据。候选配对原因包括
`record_present`、`incomplete_record`、`identifier_hint`、`ambiguous`、
`invalid_advertisement`、`crypto_error`、`unknown`；应容忍未来新增的原因值。

### `pair`

| 参数 | 类型／默认值 |
| --- | --- |
| transport | 字符串 wifi（默认）或 usb |
| udid | 字符串；默认空，由配对流程选择 |
| timeout_ms | 整数 1..300000；默认 120000 |
| repair | 布尔值；默认 false；仅 USB 可请求修复 |

返回 `{udid:string,reused:bool}`，可产生进度和 Wi-Fi PIN 事件。
用户操作手机期间必须保持请求和套接字存活，可能需要信任、解锁或开发者设置。
设备正在使用时返回 `device_busy`；空 UDID 配对会防止与活跃设备租约并发。
已有可用凭据可经验证后复用。Wi-Fi 重新配对只在新配对验证成功后替换旧凭据；
普通客户端不会提供或导入任意配对文件文本。

### `ddi.download`

| 参数 | 类型／默认值 |
| --- | --- |
| os_version | 字符串，**必填**；目标 iOS/iPadOS 版本 |
| product_type | 字符串；默认空；可选的硬件产品标识 |
| cache_directory | 字符串；默认空，由平台选择缓存目录 |
| timeout_seconds | 整数 1..3600；默认 300 |

不需要设备会话。返回 `{build:string,cache_hit:bool}`；进度事件包含文件名、已收字节和总量。
只下载和校验资源，**不会安装或挂载 DDI**。清单选择成功不表示该版本的每台手机都受支持。

### `ddi.prepare`

| 参数 | 类型／默认值 |
| --- | --- |
| udid | 非空字符串，**必填** |
| wifi | 字符串；默认空，先 USB 后已配对 Wi-Fi；auto 为 Wi-Fi 发现；显式地址为 Wi-Fi 端点 |
| port | 整数 1..65535；默认 49152，用于显式 Wi-Fi 地址 |
| cache_directory | 字符串；默认空，使用内置缓存选择 |
| replace | 布尔值；默认 false |
| download_timeout_seconds | 整数 1..3600；默认 300 |
| install_timeout_seconds | 整数 1..3600；默认 300 |

返回 `{prepared:true}`。进度可包含消息，或文件名／字节数／总量。
需要独占设备租约，因此须先关闭设备会话。
复用合适的已挂载 DDI；否则可能下载、向 Apple 请求个性化签票并安装。
显式替换会改变手机已挂载的开发者镜像。取消／超时不等于事务回滚。
镜像准备成功与视频、音频、输入服务可用是不同结论；详见 DDI 和设备验收说明。

## 连接与显示

### `connection.open`

| 参数 | 类型／默认值 |
| --- | --- |
| udid | 字符串，**必填**；空值请求自动选择唯一的已识别设备 |
| transport | 字符串 auto（默认）、usb、wifi |
| wifi | 字符串；默认空；auto 或显式 RemotePairing 地址 |
| port | 整数 1..65535；默认 49152，用于显式 Wi-Fi 地址 |

当前连接不能已有关联会话。返回 `{udid:string,attachment:string}`。
auto 模式下，wifi 为空时先尝试 USB，再尝试已配对 Wi-Fi；auto／地址选择无线路径。
`transport:usb` 强制 USB；`transport:wifi` 且 wifi 为空时使用发现。
显式 UDID 的连接失败通常返回 `operation_failed`；not_found／ambiguous 选择错误
针对空 UDID 发现。连接准备共享 30 秒的有界预算，但调用方仍须管理取消。
attachment 是活跃会话能力凭据，不是持久设备标识或设备凭据导出；应保密且不记录到日志。

### `connection.attach`

`attachment` 为**必填**字符串，取自活跃的 connection.open。
当前连接不能已有关联会话。返回 `{}`，共享相同设备、会话和生产者状态。
`invalid_session` 表示已失效或未知；所有拥有者脱离后无法恢复。
多个套接字并不允许同一设备具有多个独立拥有者。

### `connection.close`

无参数，返回 `{}`，可重复调用。只脱离**当前套接字**。
其他附着连接可继续保持会话、媒体和按住的输入。
需要报告录制收尾错误时，应先调用 media.finish 再关闭；析构清理会丢弃错误。

### `device.info`

需要会话，无参数。返回 `{udid:string,os_version:string,product_type:string}`。
这些字段是设备属性，不是能力列表。

### `display.info`

需要会话，无参数。返回 `Display`：`{width:integer,height:integer,orientation:string,serial:uint}`。
启动或复用显示订阅，并在建立订阅后最多等待两秒，获取初始正数尺寸和方向。
初始几何不可用或已取消时返回 `operation_failed`。
宽高为原生面板／显示模式尺寸；方向通常为 rot0/rot90/rot180/rot270。
watcher 几何变化会改变 serial；它与视频帧 serial 独立。

### `display.rotate`

需要会话，无参数。查询有效的基本方向，只请求一次相反的横竖屏方向。
返回 `{target:"portrait"|"landscapeLeft"}`，成功要求设备报告的状态与目标一致。
不会修改手机方向锁，应用或系统策略可拒绝旋转。
未知或不匹配的回复返回 `operation_failed`。
结果不明时不要盲目重复，因为旋转是修改操作，并非幂等设置。

### `network.debug`

需要会话。`enabled` 为布尔值，默认 false；返回 `{}`。
若设备具有隧道栈，启用或关闭服务端隧道诊断，否则不做操作。
诊断输出不是结构化 API，也不是稳定的解析目标。

## 剪贴板与输入

### `clipboard.set`

需要会话。`text` 为**必填**字符串，最多 262144 个 UTF-8 字节；返回 `{}`。
显式写入手机剪贴板，不会自动粘贴或聚焦界面。不要用它静默模拟普通键盘输入。

### `clipboard.get`

返回文本与 `clipboard.set` 一样限制为 262144 字节。设备文本超限时，在复制到
RPC 结果之前返回 `operation_failed`。

需要会话，无参数。返回 `{text:string}`，包含剪贴板内容。
应将响应视为用户敏感数据；手机策略可能拒绝访问剪贴板。

### `input.tap`

需要会话。`x`、`y` 为**必填**有限数值，均为设备面板坐标轴的 0..1。
`hold_ms` 为整数 0..2000，默认 30。
按下／抬起报告发送完成后返回 `{}`，不确认界面命中。坐标不是客户端窗口像素。

### `input.touch`

需要会话。`x`、`y` 为**必填**有限数值，范围 0..1；`down` 为**必填**布尔值。
返回 `{}`。重复 down 报告用于移动单个触点，false 用于释放。
焦点或手势结束时调用方必须释放。最终会话清理会尝试释放；
保留其他附着连接也会保持共享会话存活。

### `input.keyboard`

需要会话。`usages` 为**必填**整数数组，每个 usage 为 0..239。
返回 `{}`，替换完整的 USB HID 键盘用法码集合；`[]` 释放全部按键。
这是由手机布局／输入法解释的物理按键输入，不是 Unicode 插入。
组合键应先发送仅修饰键状态，再发送修饰键＋普通键状态，最后释放；
在已测手机上，只同时发送修饰键＋普通键并不可靠。
报告发送完成不等于手机界面已经消费该报告。

### `input.text`

需要会话。`text` 为**必填**字符串，最多 4096 字节，仅支持 US 键盘布局的
可打印 ASCII 32..126、Tab 和换行。任何字符超出范围时，会在开始输入前
以 `unsupported` 拒绝整个请求。
`hold_ms` 为整数 0..1000，默认 0；是每个生成的 HID 报告之后的延迟，
包括修饰键和释放报告。`timeout_ms` 为整数 1..300000，默认 30000；
这是覆盖 HID 建连、报告发送和等待的总预算。返回 `{}`。
不使用剪贴板、不选择输入框，也不支持宿主 IME／Unicode。
焦点和合适的手机键盘布局由调用方保证；界面处理可能晚于报告发送。
较长文本或 hold 时间可能耗尽预算，在已输入部分文本后返回 operation_failed。
取消只中断本请求的 HID 端点并唤醒等待，不撤销已输入字符，也不保证最终释放送达手机。
超时后若需释放，可发送 usages 为 [] 的 input.keyboard；套接字被中断后须先重新连接／附着。
观察到取消后不会有意继续发送文本报告。

### `input.button`

需要会话。`usage` 为**必填**整数，0..65535；`down` 为**必填**布尔值；
`page` 为整数 0..65535，默认 12（consumer 页）。返回 `{}`。
按下和释放是两次调用。可用性取决于设备和服务；不能假设所有 Android 按键动作都有 iOS 对应。
最后一个会话附着连接消失时，会尽力释放按住的按钮。

## 截图与视频

### `screenshot`

需要会话。`format` 为字符串，默认 png；支持 png/bgra。
PNG 返回 `{format:"png",display:Display}` 和原始 PNG 二进制正文。
BGRA 返回 `{format:"bgra",display:Display,width:integer,height:integer,row_pitch:integer}`
及原始 BGRA 正文。BGRA 尺寸来自解码后的 PNG，须遵守 row_pitch。
PNG 转 BGRA 时，编码输入和紧凑 BGRA 输出各不得超过 64 MiB；
像素数上限在解码器分配图像之前生效。
先等待初始显示几何，再截图并核对前后几何一致性。
变化时返回 `geometry_changed`，丢弃本次截图。不需要音视频 pump。
其他 format 字符串在启动截图前返回 invalid_params。

### `video.start`

| 参数 | 类型／默认值 |
| --- | --- |
| hardware_decode | 布尔值；默认 false；在支持的平台优先使用平台解码器 |
| decode | 布尔值；默认 true |
| record_path | 字符串；默认空；非空时必须是原始 HEVC 输出绝对路径 |
| restart_on_silence | 布尔值；默认 true；启用 3000 ms 静默重启阈值 |

需要会话，返回 `{}`。启动最新帧生产者；解码模式启动不等待首帧，应通过 video.frame 等待数据。
只接收编码流时，最多等待五秒以确认录制已就绪，失败会重置视频 pump。
未编入解码器的构建对 decode=true 返回 `unsupported`。
运行中重复调用会复用现有 pump，不应用新选项；修改选项须先停止再启动。
附着连接的控制操作应串行协调。

### `video.frame`

需要会话和视频 pump。`since` 为无符号整数，默认 0；
`timeout_ms` 为整数 0..1000，默认 50；`allow_cached` 为布尔值，默认 false。
返回 `{format:"bgra",serial:uint,width:integer,height:integer,row_pitch:integer}`、
可选 `display:Display` 及原始 BGRA 像素。
解码帧必须比 since 更新；但 allow_cached 可在等待到期后返回健康缓存帧。
应比较 serial，不要把重复的缓存帧计为新帧。无帧时返回 `no_frame`。
只接收编码流时没有 BGRA 生产者，不应轮询解码帧。

像素字节顺序为 B、G、R、A。每行占 row_pitch 字节，可能有填充。
应校验尺寸和正文长度，不要假设行跨度始终是 width*4。
编解码帧尺寸可能超过可见面板尺寸。显示元数据随帧一起采集；
不要用旧像素配上刚查询的新方向来映射输入。

### `video.stop`

需要会话，无参数，返回 `{}`，可重复调用。停止共享视频 pump，不停止音频。
不报告整个录制会话的收尾错误；显式收尾和错误边界是 media.finish。

## 音频与录制

音视频共享设备停流归属，与启动顺序无关。还有另一个泵运行时，停止其中一路只停止
本地接收和 RTCP 续租；设备上的该路流可能要等租期到期才释放。部分停流或视频恢复
不会发送 `stopAll`；最后一个泵清理时才允许全局停止设备媒体。

### `audio.start`

需要会话。`decode` 为布尔值，默认 true；`duplicate` 为布尔值，默认 false；
`buffer_ms` 为整数 0..1000，默认 50。
返回 `{sample_rate:uint,channels:uint,preroll:uint}`；
preroll 是**每声道的帧数**，不是毫秒或字节。
默认请求将音频路由到电脑；duplicate 请求手机和电脑同时接收。
默认路由失败时不会回退到 duplicate。切换路由可能让播放器暂停；
XDB 不会自动继续播放或调整设备音量。调用方须实际消费并输出 PCM，电脑才会发声。
成功不证明当前存在非静音音乐。重复启动会复用现有 pump，不应用新选项；
修改选项须先停止再启动。

### `audio.read`

需要会话，且 audio 的 decode=true。`frames` 为整数 1..48000，默认 960。
返回 `{buffered:uint,frames:uint}` 及交错、有符号 16 位**小端序** PCM。
正文大小为实际帧数 * 声道数 * 2。实际帧数可能小于请求值，包括零。
启动预缓冲未填满时返回零帧且不消费。buffered 只是诊断快照，不是容量预留。
应在工作线程读取，不要在实时音频输出回调中同步请求。
共享会话队列只应由一个消费者排空。decode=false 的音频可进入录制，但不能通过本方法读取。

### `audio.stop`

需要会话，无参数，返回 `{}`，可重复调用。停止并重置音频 pump，视频可继续。
这不是容器收尾边界。路由切换后，手机恢复播放可能需要用户继续播放器。

### `recording.configure`

| 参数 | 类型／默认值 |
| --- | --- |
| path | 服务端宿主的绝对路径字符串，**必填** |
| format | 字符串 mkv（默认）或 mp4 |
| video | 布尔值；默认 true |
| audio | 布尔值；默认 false |
| orientation | 整数；默认 0；支持 0/90/180/270 |
| orientation_from_device | 布尔值；默认 false |

需要会话，且视频、音频和录制器均未建立。返回 `{}`，至少选择一条轨道。
仅音频录制要求方向为零。不支持的角度、容器专属旋转限制、缺少封装器或无效录制配置
可能返回 `operation_failed`；显式格式／路径校验失败返回 `invalid_params`。
启用 orientation_from_device 时，后端等待有效初始显示几何；
已识别的设备方向覆盖数字 orientation，竖屏时也会将其重置为零。
初始方向未知或不可用时失败，不静默采用数字回退值。
不自动旋转、重新编码或添加黑边。

配置本身不会启动媒体轨道。之后启动所选视频／音频生产者，最后调用 media.finish 刷新和收尾。
容器内保留原始编码 HEVC/AAC-ELD，不重新编码 IPC 的 BGRA/PCM。
录制器打开输出时以独占方式创建新文件，拒绝覆盖已有文件；不要假设可以原子回滚而在相同路径静默重试失败修改。
录制失败后，解码和传输仍可继续；应检查 recording_error 并显式结束录制。

同一会话内，视频丢包、损坏或超限的访问单元（AU）会被丢弃。
录制跳过依赖受损画面的后续帧，直到收到参数受支持且通过检查的 HEVC IDR，
期间音轨继续写入。音频丢包则跳过缺失的包。
保留原始时间戳，不压缩缺失区间，也不合成媒体填补；播放器可能保持上一幅画面。
可恢复的丢弃不会设置 recording_error。会话／来源变化、不支持的编码配置、
无效时钟、共享队列耗尽和文件 I/O 错误仍会结束录制。
如果启动预算内始终没有可用的初始媒体或时钟，录制启动仍会失败。

### `media.status`

需要会话，无参数。返回 `{error:string,recording_error:string,video_unusable:bool}`。
空字符串表示当前未报告终结性失败，不证明最近仍有活动。
video_unusable 可用于截图回退。录制错误单独报告，以便保存失败时继续镜像。

### `media.finish`

停止媒体 pump 时，先取消恢复／状态查询 RPC 的等待，再 join 线程。
设备停流 RPC 的建连和回复共用两秒清理预算；超时后可能要等设备 RTCP 租期释放。
此预算不代表磁盘收尾或解码也有相同的总时限。

需要会话，无参数；返回 `{}` 或 `operation_failed`。
停止接收、等待视频录制结束、完成录制器收尾，再停止音频并重置媒体对象。
其他附着连接也会看到停止状态。需要确认输出完整性和错误时，应在 connection.close 之前调用。
成功结束后重复调用安全；不会关闭设备会话。

## 应用

### `app.list`

需要会话，无参数。结果为数组 `[{bundle_id:string,name:string},...]`，不是对象包装。
可用性和列表内容取决于手机服务。列出应用不保证每次启动请求都被允许。

### `app.launch`

需要会话。非空 `bundle_id` 字符串**必填**；
`terminate_existing` 为布尔值，默认 false。返回 `{}` 表示设备启动请求被接受，
不证明界面已显示或获得焦点。本操作可改变用户前台应用。

### `app.stop`

需要会话。非空 `bundle_id` 字符串**必填**。
请求终止应用后返回 `{}`；失败为 operation_failed。启动／停止不提供回滚。

## 调用顺序示例

以下每行都表示**同一条**已握手套接字上的一个请求。
单独的 CLI `xdb rpc` 调用使用不同连接；连续操作应使用 rpc-shell 或持久 SDK 客户端。
替换占位值，不要记录真实 attachment 或 PIN。

截图与控制：

```json
{"id":2,"method":"connection.open","params":{"udid":"DEVICE_UDID","transport":"usb"}}
{"id":3,"method":"display.info","params":{}}
{"id":4,"method":"screenshot","params":{"format":"png"}}
{"id":5,"method":"input.keyboard","params":{"usages":[4]}}
{"id":6,"method":"input.keyboard","params":{"usages":[]}}
{"id":7,"method":"connection.close","params":{}}
```

视频和音频使用拥有者连接完成打开、配置和启动，再用返回的 attachment 附着第二个客户端。
分别从不同套接字轮询 video.frame 和 audio.read，均应在声音回调之外调度。
容器录制流程示例：

```json
{"id":2,"method":"connection.open","params":{"udid":"DEVICE_UDID"}}
{"id":3,"method":"display.info","params":{}}
{"id":4,"method":"recording.configure","params":{"path":"/absolute/session.mkv","video":true,"audio":true,"orientation_from_device":true}}
{"id":5,"method":"audio.start","params":{"duplicate":true,"decode":true,"buffer_ms":200}}
{"id":6,"method":"video.start","params":{"decode":true}}
{"id":7,"method":"media.status","params":{}}
{"id":8,"method":"media.finish","params":{}}
{"id":9,"method":"connection.close","params":{}}
```

## 宣布 1.0 契约冻结前待完成项

- 对照实现审查其余方法、参数和错误范围，区分真实截止时间与各阶段预算。
  截图枚举校验、初始录制方向和有界可取消的 input.text 已实现；
  新行为仍须作为候选基线的一部分评审。
- 针对固定、已发布基线运行旧客户端／新服务端兼容性夹具，不能仅依赖同源码 IPC 回归。
  软件包版本与 ABI 版本不同；hello 成功不承诺设备支持某项能力。
- 完成 C 符号稳定性、媒体／会话生命周期和具体平台／设备验收的审查；
  证据继续按范围记录在 VALIDATION.md。

录制输出（`recording.configure.path` 与 `video.start.record_path`）必须是新的本机绝对路径文件。原子创建会拒绝已有文件和符号链接；复用路径前请自行移动或删除旧录制。录制失败可能保留不完整的新文件。
