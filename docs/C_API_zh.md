# XDB C SDK 参考——1.0

[English](C_API.md) | 简体中文

导出头文件为 [xdb.h](../include/xdb/xdb.h)。本文定义 XDB 1.0 已发布的 C ABI 1 接口约定。
RPC 参数和结果见 [API_zh.md](API_zh.md)，分帧和版本策略见 [PROTOCOL_zh.md](PROTOCOL_zh.md)。
`client.hpp` 是可选的源码级 C++ 便利适配器；nlohmann-json 和 C++ 对象均不跨越 C 二进制边界。

## 库与类型

| 库 | 导出 API | 宿主依赖 |
| --- | --- | --- |
| xdb_client | xdb_client_*、xdb_response_*、xdb_last_error | 本机套接字／JSON／平台运行库；无设备加密和媒体后端 |
| xdb_server | xdb_server_* | 设备协议、DDI、加密和媒体；仅内嵌服务的宿主需要 |

`xdb_client`、`xdb_response`、`xdb_server` 均为不透明类型。
不要自行分配、复制、检查其布局，或使用调用方分配器释放。
每个库必须销毁自己创建的对象，尤其要注意 Windows CRT 边界。
内部 `src/` C++ 类不提供稳定 ABI。
构建／打包产物须保留依赖的动态库，不要只复制单个可执行文件或 DLL。

所有字符串都是 NUL 结尾的 UTF-8。C API 的方法名／JSON／路径参数无法表示内嵌 NUL。
JSON 可表达转义字符，但仍受下游方法校验约束。
除明确称为帧数的字段外，大小均以字节计。`size_t` 随目标架构变化，不是固定 32 位。
使用头文件提供的平台导入／导出声明及原生 C 调用 ABI。

## 版本信息

`xdb.h` 包含 CMake 生成的 `xdb/version.h`，提供 `XDB_VERSION_MAJOR`、
`XDB_VERSION_MINOR`、`XDB_VERSION_PATCH`、`XDB_VERSION_STRING` 和 `XDB_ABI_VERSION`。
头文件版本描述编译时 SDK；动态库版本应在运行时查询。动态库的 SOVERSION 使用 ABI 主版本。

### `xdb_client_version`

```c
const char *xdb_client_version(void);
```

返回当前加载的客户端库版本。字符串具有静态生命周期，无需连接 Server，也无需释放。

### `xdb_client_server_version`

```c
const char *xdb_client_server_version(const xdb_client *client);
```

返回握手时 Server 上报的版本，借用到客户端销毁为止；NULL 客户端返回 NULL。
兼容的旧 Server 若未提供版本字段，返回空字符串。版本不同本身不会拒绝协议兼容的 Server。

## 生命周期与线程

1. 调用方持有返回的客户端，直到 `xdb_client_destroy`；持有每个响应，直到 `xdb_response_destroy`。
   每个成功返回的响应都必须销毁，**包括 `ok:false` 的 RPC 错误响应**。
2. 响应的 JSON／二进制访问器借用响应自身的内存；销毁后仍需使用时应先复制。
   响应是自包含对象，可以比客户端活得更久。
3. 每个客户端一次只允许一个待完成请求。不要将实现中的互斥锁当成并发调用的顺序保证。
   并发音频、视频和控制应使用不同的附着客户端，仍须遵守共享会话规则。
4. 事件回调在请求调用线程上同步执行，先于终结响应。
   回调 JSON 仅在回调期间有效；userdata 由调用方持有。
   不要在回调内重入、中断或销毁同一客户端；应将取消派发到其他线程。
   不要让异常跨越 C 回调边界。
5. 请求阻塞时，另一线程可调用 `xdb_client_interrupt`。
   它中断套接字并使客户端不可再用。
   必须等待请求调用和中断调用**都**返回后，才能销毁或替换句柄。
   C++ 适配器也遵守此规则；其原子指针不等于生命周期等待。
6. `destroy` 不是并发取消机制。Server stop 会等待工作线程结束；
   不要从回调调用，也不要在另一线程访问句柄时调用。
7. last-error 字符串是线程局部、借用的诊断信息。
   应在发生错误的线程、下一次 API 操作之前读取或复制；不能释放，也不能永久保留指针。
   不要按诊断文本分支。

## 客户端 API

### `xdb_last_error`

```c
const char *xdb_last_error(void);
```

返回当前线程的客户端诊断。用于 client connect/start/kill/request 返回 NULL／0 之后。
合法 RPC 失败记录在响应 JSON 中；本函数不会把 `ok:false` 转成错误字符串。

### `xdb_client_connect`

```c
xdb_client *xdb_client_connect(uint16_t port, xdb_event_callback event, void *userdata);
```

`port` 为 1..65535 时选择对应回环端口。零表示优先使用非空 `XDB_PORT`
（十进制 1..65535），否则为 17423；无效环境文本会失败。
`event` 是可选回调，可为 NULL；`userdata` 原样传递，不转移所有权。
返回已握手、由调用方持有的客户端，失败为 NULL，诊断见 xdb_last_error。

桌面端先连接已有 Server；只有确实没有监听者时才自动启动后台服务。
先尝试 `XDB_EXECUTABLE`，无效覆盖值会直接失败；之后依次查找客户端模块目录、
相邻 `bin` 和 PATH。启动重试最多五秒，握手读取也有五秒超时。
这些是不同阶段，不是统一的端到端截止时间。
不会杀死或替换占用端口的非 XDB／不兼容服务；升级时应显式用其旧版可执行文件执行 kill-server。

Android 需要应用持有的内嵌 Server，不自动创建进程。
销毁桌面客户端不会停止已脱离的后台服务。

### `xdb_client_connect_cancellable`

```c
typedef int (*xdb_cancel_callback)(void *userdata);
xdb_client *xdb_client_connect_cancellable(
    uint16_t port, xdb_event_callback event, void *userdata,
    xdb_cancel_callback cancel, void *cancel_userdata);
```

端口选择与句柄所有权遵循 `xdb_client_connect`。传入非 NULL 的 `cancel` 时，
启动锁等待、Server 启动及重试、回环连接和 hello 握手共享 15 秒预算。
取消回调返回非零值表示请求取消。NULL 沿用普通连接入口的行为。

取消回调可能在作用域内的 watchdog 线程执行，必须线程安全、不能抛异常，
也不能重入本次连接操作。调用方持有 `cancel_userdata`，直到函数返回。
返回前会等待所有 watchdog 线程结束，此后不会再访问取消回调或其 userdata。

失败返回 NULL，通过调用线程的 `xdb_last_error` 读取诊断。取消会关闭待完成连接的
客户端套接字；期间启动的共享 Server 可以继续运行。取消与成功完成发生竞争时，
函数可能返回有效客户端，调用方应在无需使用时销毁该句柄。

### `xdb_client_start_server`

```c
int xdb_client_start_server(uint16_t port, const char *data_directory);
```

确保可连接兼容 Server；成功返回 1，失败返回 0。端口语义与 connect 相同。
`data_directory` 为 NULL／空时，新启动的桌面 Server 使用 CLI 的平台用户数据默认目录；
非空路径必须绝对。复用现有服务时**不会改变其目录**，
也不会仅因安装了不同版本或路径就重启。Android 只能使用已可连接的内嵌 Server。

### `xdb_client_kill_server`

```c
int xdb_client_kill_server(uint16_t port);
```

端口语义与 connect 相同。服务不存在或已停止时返回 1，错误返回 0。
不会启动不存在的服务。请求 server.stop 后，最多等待五秒让监听端口关闭。
不会强杀无关进程，也不会修改手机配对数据。现有会话和客户端会被取消，
调用方须先与其他消费者协调。检查的是端口已空闲，不证明所有外部设备操作已回滚。

### `xdb_client_request`

```c
int xdb_client_request(xdb_client *client, const char *method,
                       const char *params_json, xdb_response **response);
```

`client`、`method`、`response` 不能为 NULL。
`params_json` 为 NULL 时表示 `{}`，否则必须可解析为 JSON 对象。
字符串只需在同步调用期间有效。函数先将 `*response` 设为 NULL，
收到合法终结响应后再填入调用方持有的响应。不要传入尚未销毁的旧响应。

**返回 1 只表示收到响应，不表示 RPC 成功。** 应解析 JSON 的 `ok` 布尔值。
`ok:false` 时检查 `error.code`／`message`，然后正常销毁响应。
返回 0 表示本地参数、传输、分帧、响应或异常失败；
读取 xdb_last_error，不要据此断言修改操作未到达手机。
传输／协议失败后应重连。本地错误参数可能在发送前失败；
API 尚未提供独立的强类型传输状态枚举。

没有通用的单次调用超时参数。发现、DDI、配对、帧和文本各自的预算
不能覆盖全部设备服务操作。请求应离开 UI／音频线程；
应用截止时间到期时，可由另一线程 interrupt。

### `xdb_client_interrupt`

```c
void xdb_client_interrupt(xdb_client *client);
```

NULL 时不做操作。中断套接字，不进行事务撤销；该客户端不可再用。
不保证收到响应或取消回调。设备／服务端已经接受的配对、安装或文件修改可能保留。
销毁前先等待调用完成。

### `xdb_client_destroy`

```c
void xdb_client_destroy(xdb_client *client);
```

NULL 时不做操作。关闭本地连接并释放句柄，之后不能再使用。
相对于本地调用返回，服务端清理是异步的；其他套接字仍附着时继续保留设备租约。
显式 media.finish 可报告录制收尾错误；断连／析构清理不会返回这些错误。

## 响应 API

### `xdb_response_json`

```c
const char *xdb_response_json(const xdb_response *response);
```

返回借用的、NUL 结尾 UTF-8 响应封装，直到响应销毁前有效。
NULL 响应返回空字符串，不是 JSON 响应。封装可能表示成功或失败。
不要在销毁响应后保留这个指针。

### `xdb_response_data`

```c
const uint8_t *xdb_response_data(const xdb_response *response);
```

借用的二进制正文，直到销毁前有效。无正文或 NULL 响应时为 NULL。
不要把它当成 NUL 结尾文本，也不要释放。布局取决于 RPC 结果，
例如 PNG 字节、含行填充的 BGRA 或交错小端 PCM。

### `xdb_response_size`

```c
size_t xdb_response_size(const xdb_response *response);
```

二进制字节数；空正文／NULL 响应时为零。
读取或复制前，须与声明的像素尺寸／行跨度或帧数／声道数核对。
语言绑定应使用带溢出检查的算术，不要盲目把 size_t 截断为 int。

### `xdb_response_destroy`

```c
void xdb_response_destroy(xdb_response *response);
```

NULL 时不做操作。使全部借用的 JSON／二进制指针失效。
每个持有的响应只能销毁一次。不同响应独立于客户端之后的请求。

## 内嵌 Server API

### `xdb_server_last_error`

```c
const char *xdb_server_last_error(void);
```

当前线程的服务端诊断，为借用值。xdb_server_start 返回 NULL 后使用，
与客户端库的 xdb_last_error 独立。

### `xdb_server_start`

```c
xdb_server *xdb_server_start(uint16_t port, const char *data_directory);
```

启动调用方持有的多线程服务，只绑定 IPv4 回环，不要求手机已连接。
端口零选择空闲端口，与客户端端口零含义不同。
`data_directory` 必须为应用控制的非空、绝对 UTF-8 路径；NULL／相对路径会被拒绝。
父目录或数据目录可能延后到需要持久化的操作才创建。
服务不接管调用方字符串的所有权。返回调用方持有的句柄，失败为 NULL，
诊断见 server_last_error。当前实现最多接受 16 个客户端工作线程，不应无限打开附着套接字。
绑定失败不会驱逐其他监听者。

### `xdb_server_port`

```c
uint16_t xdb_server_port(const xdb_server *server);
```

实际绑定端口，可在 start(0,...) 后使用。NULL 时返回零。
仅在句柄存在期间有效；自身不能证明监听者健康。

### `xdb_server_is_running`

```c
int xdb_server_is_running(const xdb_server *server);
```

句柄尚未进入停止状态时返回非零，NULL／停止中返回零。
这是生命周期标志，不是手机／传输健康检查。
远程 server.stop 可在持有方仍持有已分配句柄时改变此值。

### `xdb_server_stop`

```c
void xdb_server_stop(xdb_server *server);
```

NULL 时不做操作。停止监听者、中断客户端、等待工作线程并释放句柄。
即使已远程停止，仍必须调用一次。可能等待设备清理；
不能从回调或并发访问中调用，也不保证瞬间强制终止。
不删除已有手机配对记录或 DDI 缓存。

## 最小 C 示例

可编译版本见 [examples/client_info.c](../examples/client_info.c)。
示例展示所有权和收到响应的处理；设备操作的业务 `ok` 应使用真正的 JSON 解析器检查，
不要对子字符串或诊断消息做匹配。

```c
xdb_client *client = xdb_client_connect(0, NULL, NULL);
if (!client) { /* copy xdb_last_error() on this thread */ }
xdb_response *response = NULL;
if (client && xdb_client_request(client, "server.info", "{}", &response)) {
    /* Parse xdb_response_json(response); inspect `ok` and `result`. */
    /* Consume/copy any binary body BEFORE destroying response. */
    xdb_response_destroy(response);
}
xdb_client_destroy(client);
```

## 1.0 冻结检查清单

- 冻结导出符号／签名清单，并在承诺跨版本混用二进制之前确定显式 ABI 修订／兼容机制。
- 审查上述线程、取消、错误语义和有界设备操作。
- 验证旧基线客户端／新服务端，以及绑定中的大小和生命周期处理。
- 在 CI 保留纯 C 示例编译及 RPC／符号文档覆盖检查；
  它们检查文档漂移，不是硬件验收或语义兼容性验收。
