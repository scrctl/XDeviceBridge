# XDB API 参考

[English](API.md) | 简体中文

本文定义当前源码的公开 RPC 接口约定，已对照 `xdb.h`、`Client.cpp`、`Server.cpp`、
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
| SDK 已连接／hello 已完成，无设备会话 | server.info、server.stop、devices、mdns.check、mdns.services、pair、connect、disconnect、ddi.download、ddi.prepare、ddi.status、ddi.remove、connection.open、connection.attach、connection.close |
| 已拥有或附着设备会话 | 上述方法及 device/display/input/app/clipboard/media 方法；同一连接再次 open/attach 会失败 |
| 已通过 connection.close 脱离 | 可再次调用管理方法；open/attach 可建立新的关联 |
| 已中断、传输失败或已销毁 | 丢弃客户端，使用新句柄重新连接 |

默认 control 模式下，同一 UDID 只允许一个独立控制拥有者；其 attachment 可以共享控制权。
`connection.open` 指定 `access:"read"` 则可并行截图和查询，返回单独的只读 attachment。
attachment 继承创建时的读／控制类型，不能把只读 attachment 附着成控制客户端。
最后一个控制客户端退出时结束媒体并尝试释放输入，旧控制 attachment 失效；
读取客户端可继续保留设备用于截图，后续控制客户端可重新取得控制租约。
所有客户端都退出后释放设备会话；手动 `connect` 的基础连接仍可继续保留。
传输失败时不能保证输入释放报告成功送达。

附着连接共享媒体配置。应由一个控制客户端修改媒体状态，按需另设音频读取和视频轮询客户端。
不要让多个音频客户端竞争读取：它们会消费同一个 PCM 队列。
共享会话锁用于防止读取期间拆除生产者，不是多个控制客户端之间的事务机制。

## 错误码与恢复

| 错误码 | 含义／通常的处理方式 |
| --- | --- |
| handshake_rejected | 首个方法或协议不正确；服务端关闭套接字。使用兼容的客户端和服务端。 |
| invalid_params | JSON 字段缺失、类型错误、数值越界或显式参数校验失败。修正请求；连接可能仍可使用。 |
| cancelled | 后端分发前检测到取消；断连期间的取消也可能表现为 EOF／无响应。 |
| operation_failed | 设备、发现／连接、配对、DDI、编解码、录制或其他后端失败。message 说明本次原因，原因明确时使用下列传输／配对分类。 |
| device_busy | 其他会话／操作持有设备租约，或手动连接阻止管理操作。先关闭会话／附着连接，或 disconnect 手动连接。 |
| exclusive_session_required | 共享控制组请求录制或全局媒体关闭；使用独占会话，或关闭本组关联。 |
| media_conflict | 共享视频的参数与当前生产者不一致；使用相同参数。 |
| input_busy | 其他共享组正在持有触摸手势，或共享键盘仍有按住的键。 |
| read_only_session | 当前设备关联为只读，拒绝输入、媒体配置、音频队列读取及管理修改；不是本机进程鉴权。 |
| not_paired | 找不到可用本地 Wi-Fi 配对记录。使用 `xdb -e pair` 完成配对。 |
| unauthorized | 设备明确拒绝主机或已有配对记录。USB 请解锁并确认“信任”，然后使用 `xdb -d pair`；Wi-Fi 请用 `xdb -e pair` 重新配对。 |
| device_locked | 设备明确返回锁屏错误。解锁并保持亮屏后重试；这不表示配对失效或需要重装 DDI。 |
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
`not_connected`，而不是 `unknown_method`；未知 `input.*` 方法在初始化 HID 服务前拒绝。
不要用未知调用做无副作用的能力探测；发现接口会显式报告视频解码能力。

错误分帧、超大消息和无效请求封装会关闭套接字，不保证返回 RPC 错误。
收到合法 RPC 错误不等于传输失败。C API 返回值区分“收到响应”和 RPC 的 `ok`；
详见 [C SDK](C_API_zh.md)。

## 管理

连接错误分类来自设备回复或本地凭据检查，不匹配终端文案。网络超时、传输失败、格式／签名／密码学错误及未知设备错误仍为 `operation_failed`，不推断为未授权。自动连接的两个传输均失败时，保留 USB 和 Wi-Fi 原因；分类无法归一时使用 `operation_failed`。不会自动删除或重建配对。

### `hello`

仅供直接使用通信协议的客户端调用；须在五秒内发出首个请求。
`protocol` **必填**，整数 `1`。返回 `{protocol:1,version:string}`，其中 version 是软件包版本。
协议或方法错误时，尽力发送 `handshake_rejected` 后关闭套接字。
帧头版本也必须为 1。SDK 用户不要再次发送 hello。

### `server.info`

不需要参数或设备会话。返回 `{protocol:1,version:string,pid:integer,device_transport:"usb-or-wifi"}`。
`version` 是 CMake 软件包版本，不能替代协议协商，也不能证明设备已连接。

`capabilities.pairing_display_brand:true` 表示支持 `pair.display_brand`。
缺少此能力字段时按不支持处理；旧服务端可能忽略未知请求字段。

`capabilities.manual_connections:true` 表示支持 1.2 新增的 `connect`／`disconnect`。
CLI 会在发送前检查此能力，缺失时明确提示升级 Server；旧版未实现这些方法。通信协议和 C ABI 仍为 1。

`capabilities.read_only_sessions:true` 表示 1.3 起支持 `connection.open.access`。
SDK 调用方须先检查此能力；1.2.0 及更早的 Server 会忽略未知字段，不能依赖它们执行只读限制。
CLI 截图会探测此能力，旧服务端保留原有行为。
`capabilities.shared_control_sessions:true` 另行表示支持下述显式 shared 控制组；公共 C ABI 与通信协议仍为 1。

`capabilities.mdns_diagnostics:true` 表示支持 `mdns.check` 和 `mdns.services`；缺失时不支持。

### `server.stop`

不需要参数或设备会话。先发送结果 `{}`，再请求关闭服务。
所有客户端和会话被取消；不会取消手机配对或删除用户文件。
收到响应不证明进程已经退出。`xdb_client_kill_server` 还会等待端口关闭。
内嵌服务的持有方仍须调用 `xdb_server_stop` 释放服务句柄。

### `devices`

手动连接按 UDID 与发现结果合并。即使没有广播也按 `connected` 列出，
包含 `manual_connection:{address,port}` 和对应 Wi-Fi 候选；仍遵循传输方式筛选。

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

### `connect`

不要求设备会话。认证明确指定的 Wi-Fi 端点，并由 Server 保留基础连接；
不执行发现、重新配对或 DDI 准备。

| 参数 | 类型／默认值 |
| --- | --- |
| address | **必填**字符串：设备 IP 或主机名；接受 HOST[:PORT]／[IPv6%scope]:PORT |
| port | 可选整数 1..65535，覆盖端点中的端口；默认 49152，即手机 RemotePairing 端口 |
| udid | 可选字符串：选择可用的本地配对记录；存在多台设备的记录时必须指定 |
| timeout_ms | 整数 1..300000；连接和认证默认限时 30000 毫秒 |

返回 `{udid:string,address:string,port:integer,reused:bool}`。新连接成功表示已经完成
pair-verify、TLS 隧道和 RSD 建连；仅找到本地记录不代表连接成功。
对同一 UDID 重复 connect 会复用已有连接，返回其实际端点，即使本次传了另一个地址，
也不会把现有连接迁移到该地址。

调用方退出后基础连接由 Server 持有。设备会话复用它；最后一个附着客户端退出时
释放媒体和 HID 服务，基础连接继续空闲保留，不主动采集媒体。默认 control 接入仍独占；已有控制组时，
另一个独占 open 返回 `device_busy`。传输掉线、`disconnect` 或 Server 关闭时
使附着会话失效并关闭连接；不自动重连，重启后也不恢复。空闲 TCP keepalive 用于
检测没有 FIN 的静默断网，检测时间受操作系统影响，不是固定的 RPC 截止时间。

### `disconnect`

不要求设备会话。可选 `address`、`port` 与 `connect` 语法一致；可选 `udid` 按身份筛选。
不传任何筛选条件时，关闭这个 Server 的**全部手动连接**；同时指定端点与 UDID 时必须都匹配。
返回 `{disconnected:[{udid:string,address:string,port:integer}]}`；没有匹配连接时返回空数组，
也视为成功。使用这些连接的活跃会话失效，之后设备调用返回 `not_connected`，旧 attachment
也无法复用。保留配对记录、DDI 和其他会话。重新配对或管理 DDI 前须先关闭／断开手动连接。

### `mdns.check` / `mdns.services`

无需设备会话，通过 `server.info.capabilities.mdns_diagnostics` 探测支持。
可选 `timeout_ms` 为整数 1..60000，默认 3000；使用 Server 的持续订阅缓存，不可用时进行有界查询。
仅查询 `_remotepairing._tcp.local.`，不读取配对凭据、不认证设备、不启动媒体。
两者返回 `{backend:string,available:bool,cancelled:bool,warnings:string[],service_type:string}`。
`backend` 为实际尝试的 `bonjour`、`avahi`、`windows-dnssd`、`android-nsd` 或 `direct`。
`available` 仅表示后端可以查询；服务列表为空并不表示后端不可用。
`mdns.services` 另返回 `services` 数组，每项为
`{instance:string,name:string,target:string,endpoints:[{address:string,port:uint16,interface:string,interface_index:uint}]}`。
不返回原始 TXT、配对匹配结果或认证结论。取消可能保留已收到的服务快照。

### `pair`

| 参数 | 类型／默认值 |
| --- | --- |
| transport | 字符串 wifi（默认）或 usb |
| udid | 字符串；默认空，由配对流程选择 |
| timeout_ms | 整数 1..300000；默认 120000 |
| repair | 布尔值；默认 false；仅 USB 可请求修复 |
| display_brand | 字符串；默认 xdb；1..64 个 UTF-8 字节，禁止控制字符及首尾空格 |

返回 `{udid:string,reused:bool}`，可产生进度和 Wi-Fi PIN 事件。
用户操作手机期间必须保持请求和套接字存活，可能需要信任、解锁或开发者设置。
设备正在使用时返回 `device_busy`；空 UDID 配对会防止与活跃设备租约并发。
已有可用凭据可经验证后复用。Wi-Fi 重新配对只在新配对验证成功后替换旧凭据；
普通客户端不会提供或导入任意配对文件文本。

`display_brand` 只改变新配对时的主机名称／型号前缀，例如 `scrctl - <主机名>`，
Android 为 `scrctl - <设备型号>`。Wi-Fi 广播、配对信息及 USB 配对信息使用同一品牌。
显示序列号仍为 `XDB-...`；密钥、主机标识和已有记录不变。复用记录不会重命名
手机上已保存的电脑。连接独立安装的 Server 时，先检查
`server.info.capabilities.pairing_display_brand`。命令行用法为
`xdb pair --display-brand scrctl`，遇到不支持的 Server 会在配对前报错。

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
| wifi | 字符串；默认空，使用 USB；auto 为已配对 Wi-Fi 发现；显式地址为 Wi-Fi 端点 |
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

### `ddi.status`

| 参数 | 类型／默认值 |
| --- | --- |
| udid | 非空字符串，**必填** |
| wifi | 字符串；默认空，使用 USB；auto 为已配对 Wi-Fi 发现；显式地址为 Wi-Fi 端点 |
| port | 整数 1..65535；默认 49152，用于显式 Wi-Fi 地址 |

不需要设备会话，但需要独占设备租约，因此须先关闭已有会话。
连接后查询镜像挂载服务；若设备公布了 cryptexd 服务，还会独立查询已安装的 DDI Cryptex。
不下载、签票、安装、移除，也不与下载目录中的镜像比较。返回：
`{udid:string,os_version:string,kind:string,image_type:string,mounted:bool,
verification:string,cryptex_installed:bool|null,cryptex_identifier:string,
cryptex_version:string}`。

`kind` 是按系统版本选择的下载类别（`classic`、`personalized`、`cryptex`），
不是已挂载镜像的构建号或身份。`image_type` 是查询的服务镜像类型：
`Developer` 或 `Personalized`；Cryptex 的镜像也表现为 `Personalized`。
挂载时 `verification` 为 `LookupImage signature` 或 `CopyDevices entry`，否则为空字符串。
这表示挂载证据，不保证它与某份下载镜像兼容。

`cryptex_installed` 通过 cryptexd 独立查询；DDI 可以已安装但尚未挂载。
安装时返回标识与版本，否则相应字符串为空。未公布服务时该字段为 null，
不能视作已查询且未安装；Cryptex 类别的系统若无法进行该查询，则返回失败。
现代系统的 USB 连接也会建立 RSD，以查询实际安装情况，不仅依赖下载类别判断。
建连阶段和共享的查询／移除阶段各有 30 秒预算，并支持取消。
无法验证的回复或查询失败返回 `operation_failed`，不会伪造“未安装”；
设备被占用时返回 `device_busy`。

### `ddi.remove`

参数与独占租约要求同 `ddi.status`。执行设备准备的反向操作，保留配对记录和宿主缓存：

- 若 cryptexd 报告已安装 `com.apple.MobileAsset.DDI`，按查询到的实际版本卸载，
  并验证 DDI 安装已不存在。不移除其他 Cryptex，也不从下载目录猜测版本。
- 否则通过镜像挂载服务卸载 Classic／Personalized 镜像（`/Developer` 或
  `/System/Developer`），并验证挂载已不存在。支持 Cryptex 的系统上，
  若使用的仍是镜像挂载服务 DDI，也走此路径。

已经不存在的 DDI 会成功返回，不再执行移除修改。移除后验证无挂载，并在服务可用时
复查 Cryptex 安装。卸载 Cryptex 后若仍有挂载，则返回错误，不会再尝试第二种移除机制。
返回上述状态字段，其中 `mounted:false`、未报告已安装的 Cryptex，
verification／identifier／version 为空，另含 `changed:bool`。
`changed:true` 表示本次请求执行了移除并验证结果。

不会更新个性化 nonce、下载资源或卸载应用。再次准备设备请调用 `ddi.prepare`。
这会撤销 DDI 准备的结果，不会恢复此前被 `prepare --replace` 替换掉的旧镜像。
取消或回复丢失可能发生在设备已修改之后；重试前先查 `ddi.status`。
不自动重试，也不自动回滚。

## 连接与显示

### `connection.open`

auto／wifi 会复用所选 UDID 的已有手动 Wi-Fi 连接。UDID 为空时，先选择唯一手动连接，
没有时才执行发现。显式 usb 要求先 disconnect 该设备的手动连接。

| 参数 | 类型／默认值 |
| --- | --- |
| udid | 字符串，**必填**；空值请求自动选择唯一的已识别设备 |
| transport | 字符串 auto（默认）、usb、wifi |
| wifi | 字符串；默认空；auto 或显式 RemotePairing 地址 |
| port | 整数 1..65535；默认 49152，用于显式 Wi-Fi 地址 |
| access | 字符串 control（默认）、read 或 shared；非默认模式须先检查对应能力 |

当前连接不能已有关联会话。返回 `{udid:string,attachment:string,access:"control"|"read"|"shared"}`。
auto 模式下，wifi 为空时先尝试 USB，再尝试已配对 Wi-Fi；auto／地址选择无线路径。
`transport:usb` 强制 USB；`transport:wifi` 且 wifi 为空时使用发现。
显式 UDID 的连接失败通常返回 `operation_failed`；not_found／ambiguous 选择错误
针对空 UDID 发现。连接准备共享 30 秒的有界预算，但调用方仍须管理取消。
attachment 是活跃会话能力凭据，不是持久设备标识或设备凭据导出；应保密且不记录到日志。

read 模式可复用既有设备会话，不会建立第二路视频。支持 `device.info`、`display.info`、
`screenshot`、`app.list`、`clipboard.get`、`media.status`，以及读取已启动视频的 `video.frame`。
不允许输入、媒体启停／录制配置、`audio.read`、配对／DDI／connect/disconnect 或 `server.stop`；
`audio.read` 会消费共享队列，不能当成无副作用查询。
没有既有会话时可单独创建只读会话；没有控制拥有者时，后续普通 open 可取得控制权。
只读复用须与显式 transport 匹配。只读客户端关闭不会停止控制拥有者的媒体。
这些约束针对当前关联，不能替代本机进程／用户鉴权；关闭关联后可重新请求控制权。

shared 模式须先检查 `capabilities.shared_control_sessions:true`，旧 Server 可能忽略未知字段。
多个显式 shared open 各有独立控制组和私密 attachment，共用设备、解码器及视频生产者；
不能加入既有的独占控制会话，独占 open 也不能抢占已有共享组。
相同 `video.start` 参数复用视频，解码方式或恢复参数冲突返回 `media_conflict`。
共享组不支持录制、`video.stop` 或 `media.finish`，这些调用返回 `exclusive_session_required`；
退出时关闭该组所有套接字，最后一组退出才结束视频。轮询／作业套接字 attach 到原组，
不会成为新的独立组。退出一组会释放其输入；各组按键取并集，触摸手势占用冲突返回 `input_busy`。
`input.text` 要求共享键盘没有按住的键。音频只允许一个控制组消费，其他组的音频请求返回
`device_busy`；音频拥有者退出会停音频，但不会停止剩余组的视频，也不自动转移音频。

### `connection.attach`

`attachment` 为**必填**字符串，取自活跃的 connection.open。
当前连接不能已有关联会话。返回 `{}`，共享设备及会话，并继承该 attachment 的读／控制类型。
`invalid_session` 表示已失效或未知；所有拥有者脱离后无法恢复。
独占 control 模式仍只允许一个拥有者；shared attachment 仅延长其自身控制组的生命周期。

### `connection.close`

最后一个附着客户端退出后，手动连接的基础隧道仍空闲保留在 Server。
无参数，返回 `{}`，可重复调用。只脱离**当前套接字**。
其他控制附着连接可继续保持媒体和按住的输入；只读客户端不会延长控制媒体的生命周期。
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
Server 1.4.0 起支持可选整数 `contact`：0..4，默认 0。返回 `{}`。
重复 down 报告移动指定触点，false 只释放该触点；每份 HID 报告保留其他仍按下的触点。
焦点或手势结束时调用方必须释放。最终会话清理会尝试释放全部触点；
附着连接保留其控制组的生命周期。

多点调用方必须要求 Server 1.4.0 或更新版本，旧 Server 可能静默忽略 `contact`；
不新增能力标记。共享组在全部触点抬起前持有整个手势。`input.tap` 使用触点 0。

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

shared 模式只接受解码视频；相同参数复用生产者，冲突则拒绝，不会重启另一窗口的视频。

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

## 兼容性与验证范围

XDB 1.0 已确立 C ABI 1 与线协议 1。软件包版本与这些契约分别演进，
详见[版本与升级策略](PROTOCOL_zh.md)。

- 已发布的 1.0.0／1.1.0 客户端与服务端的四种组合，已在 macOS ARM64 上通过
  指定的 C API 与 RPC 检查。这不代表跨版本实时媒体或 Windows／Linux 验收；
  详见[验证记录](VALIDATION.md)。
- hello 成功仅确认协议兼容，不保证设备能力或新方法可用。
  客户端须处理操作失败；需要新方法时，应升级旧服务端。
- 设备、媒体、取消与截止时间的验证，仍以验证记录中列出的平台、设备和操作为限。

## 录制输出路径

录制输出（`recording.configure.path` 与 `video.start.record_path`）必须是新的本机绝对路径文件。原子创建会拒绝已有文件和符号链接；复用路径前请自行移动或删除旧录制。录制失败可能保留不完整的新文件。
