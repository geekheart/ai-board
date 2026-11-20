# Lua 脚本接口

Lua 运行在 AI Board 的 ESP32-C3 上，用于编排矩阵接线、产生测试输入、采集数字信号，以及记录真板实验结果。目标 MCU 的业务固件由它自己的工具链编译和烧录。以下接口以当前仓库固件为准，调用前可通过 [CLI 文档](CLI.md) 中的 `diagnostics` 查看板卡版本。

## 快速开始

在仓库根目录运行以下命令。先设置 `AI_BOARD_URL`（板卡实际地址）、`AI_BOARD_KEY_FILE`（本机私有的设备密钥文件）和 `AI_BOARD_LEASE_FILE`（本机私有目录内分给本会话的租约文件）。脚本只读取矩阵状态，矩阵未启用时也可执行。

```sh
board_rpc() {
  python3 tools/board_rpc.py --url "${AI_BOARD_URL:?请填写板卡地址}" \
    --key-file "${AI_BOARD_KEY_FILE:?请填写本机密钥文件路径}" \
    --lease-file "${AI_BOARD_LEASE_FILE:?请填写本机租约文件路径}" "$@"
}

(
  set -e
  board_rpc control acquire --ttl 120
  trap 'board_rpc control release >/dev/null' EXIT
  board_rpc lua - --timeout-ms 1000 <<'LUA'
local state = assert(board.call("state"))
print("matrix armed", state.armed)
return {armed = state.armed, y0_mask = state.routes[1]}
LUA
)
```

也可以把 Lua 正文粘贴到板载「Lua 脚本」页面运行，或保存为本地文件后用 `lua FILE` 执行。浏览器运行要求 AI 控制租约已释放。

## 板载 Lua API

### `board.call(op [, params [, format]])`

同步调用工作台操作，完成后返回结果。

| 参数 | 类型与取值 |
| --- | --- |
| `op` | 1–31 字符的操作名，格式为 `[a-z_][a-z0-9_]*`，例如 `"state"`、`"i2c_transfer"`。 |
| `params` | 可省略或传 `nil`。使用 Lua 表时，键与操作名同格式，保留键 `op` 由第一个参数提供。值可用整数、布尔值或可打印 ASCII 字符串，字符串须避开 `&`、`=`、`%`、`+`。布尔值会编码为 `yes` 或 `no`，整数转为十进制文本。二进制数据使用对应操作的十六进制字段，例如 `hex="00A5"`。内部表单缓冲区为 1280 字节，含结尾空字节。 |
| `format` | 省略或传 `nil` 时，工作台 JSON 自动转成 Lua 值。传 `"json"` 时返回原始 JSON 字符串，适合保留较大的采集记录并交给电脑侧解析。 |

操作失败会返回 `nil, error`，例如矩阵忙、路由条件不满足或外设驱动失败。可用 `assert(board.call(...))` 将错误报告为脚本失败，也可用 `local value, err = board.call(...)` 判断后续分支。参数类型、参数名、返回格式错误，以及内部结果无法解码等情况会直接产生 Lua 错误。

#### `state` 读回字段

`board.call("state")` 不需要操作参数。Lua 数组从 1 开始，Y 通道和 BBX 编号从 0 开始，例如 `state.routes[1]` 对应 Y0。

| 字段 | 说明 |
| --- | --- |
| `armed` | 矩阵是否已启用。 |
| `routes` | 8 个 Y 通道的 16 位掩码，位 `n` 对应 BBX 通道 `n`。 |
| `levels` | 8 个 Y 通道的 GPIO 数字读回值。 |
| `modes` | 8 个 Y 通道的模式，取值为 `input`、`pullup`、`pulldown`、`low`、`high` 或 `pwm`。 |
| `gpios` | Y0–Y7 在 ESP32-C3 上对应的 GPIO 编号，依次为 18、8、6、5、19、4、3、1。 |
| `out_mode` | 启用了 OUT 控制的固件才返回。取值为 `5v` 或 `gnd`，反映 CTRL 控制状态。 |

#### 可调用的工作台操作

下表列出 Lua 支持的操作名。各操作的参数名、单位、范围及响应字段与 [CLI 的工作台操作](CLI.md#工作台操作) 一致。将 CLI 的 `KEY=VALUE` 转成 Lua 表中的 `KEY=VALUE`，字符串加引号，例如 `engine="gpio"`。硬件操作沿用相同的电气与接线条件。

| 类别 | `op` |
| --- | --- |
| 矩阵与 IO | `route`、`reset`、`gpio`、`power`（取决于该固件的 OUT 配置） |
| 主动收发 | `uart`、`proxy`、`i2c_scan`、`i2c_read`、`i2c_transfer`、`spi` |
| 从机与接收 | `uart_receive`、`i2c_slave`、`spi_slave` |
| 数字采集 | `capture` |
| 板内夹具 | `uart_loopback`、`i2c_capture_loopback`、`spi_capture_loopback`、`i2c_host_capture`、`spi_host_capture`、`protocol_receive_bench`、`i2c_register_bench`、`i2c_register_table_bench`、`spi_slave_bench` |

除 `state` 和已配置的 `power` 外，工作台操作需要先启用矩阵。板内夹具要求 BBX 实际空载并清空路由，确认后可传 `confirm_empty=true`。矩阵启用、448 组矩阵自检、配网、租约和 OTA 由 [CLI 或设备管理入口](CLI.md) 控制。

例如矩阵已启用，Y0 和 Y1 已分别接到两条确认过的信号网络并设置为输入时，可用下面的脚本读回一段波形。A、B 分别对应 Y0、Y1。直接返回原始 JSON 时，采集内容位于执行响应的字符串型 `result` 字段中，电脑侧先解析外层响应，再解析该字符串。大型采集可用 [CLI 直接保存数据](CLI.md#数字信号采集)，避免完整波形受 Lua 结果大小限制。

```lua
local raw = assert(board.call("capture", {
  a = 0, b = 1, engine = "gpio", ms = 100, trigger = "manual"
}, "json"))
return raw
```

### `board.sleep(ms)`

`ms` 为 0–10000 的整数毫秒，且须小于脚本当前剩余时间。大于 0 时按 FreeRTOS tick 向上取整，0 只检查剩余时间，调用无返回值。用于实验步骤间等待。精确总线时序由固件的对应外设操作生成。

### `board.null` 与结果类型

`board.null` 是 JSON `null` 的标记，和 Lua `nil` 不同。读取含 `null` 的数组时，该标记会保留数组位置。返回结果中也可显式使用它。

```lua
return {samples = {12, board.null, 15}}
```

上例的 `result` 为 `{"samples":[12,null,15]}`。执行器读取脚本的第一个返回值。无返回值或返回 `nil` 时，`result` 为 `null`。对象表使用字符串键，数组表使用从 1 开始且连续的整数键。新建的空表 `{}` 编码为空对象，从 JSON 解码得到的空数组仍保留数组类型。循环引用、键类型混用、稀疏数组及 NaN 或无穷大的数值会导致结果编码失败。

## 运行限制与错误

| 项目 | 当前限制和行为 |
| --- | --- |
| 运行时 | Espressif Lua 5.5.1。每次执行创建独立的 VM，Lua 整数为 32 位，浮点数为单精度。成功应用的硬件状态在运行结束或出错后仍生效。 |
| 源码 | 1–8192 字节的 UTF-8 Lua 文本。CLI 和网页按字节校验，保存脚本时固件拒绝含 NUL 的源码。 |
| 超时 | `timeout_ms` 100–10000，默认 5000。按实测操作耗时留出余量。工作台调用会预留显式的 `wait_ms`、`ms`、`timeout` 预算，`proxy` 的 `timeout` 按两轮计算。驱动完成后会再次检查脚本截止时间。 |
| 内存与指令 | Lua VM 堆 64 KiB。每 1000 条 VM 指令触发一次检查，累计 4000 次触发指令上限。 |
| 返回与日志 | JSON 响应上限 24 KiB，包含结果和日志，JSON 值的嵌套深度受 16 层限制。`print(...)` 用制表符分隔参数并换行，日志最多记录 1024 字节。 |
| 标准库 | 提供基础函数和 `table`、`string`、`math`，移除 `dofile`、`loadfile`、`load`、`pcall`、`xpcall`、`warn`、`getmetatable`、`setmetatable`，以及 `string.dump/find/gmatch/gsub/match`。文件、网络和外设接入由固件与电脑侧工具负责。 |

执行结果使用 JSON。成功响应包含 `ok: true`、`result`、`log`、`elapsed_ms`。脚本失败时，`ok` 为 `false`，含 `kind` 和 `error`，同时保留已有的 `log` 和 `elapsed_ms`。

| `kind` | 含义 |
| --- | --- |
| `syntax` | 源码解析失败。 |
| `runtime` | 脚本运行错误或 `assert` 失败。 |
| `timeout` | 达到脚本截止时间或等待超过剩余时间。 |
| `limit` | 达到指令数限制。 |
| `memory` | Lua VM 分配内存失败。 |
| `result` | 返回值无法编码成有界 JSON，或结果过大。 |

脚本执行失败也会返回 HTTP 200，调用者应检查 `ok`。CLI 将 Lua 的 JSON 写到标准输出，`ok=false` 时退出码为 1。HTTP 传输或权限错误由 CLI 写到标准错误。HTTP 400 表示请求参数或请求体错误，401 表示设备密钥缺失或错误，409 常见于控制忙，423 表示租约不满足入口要求，500 表示固件内部出错。

## 保存和运行板上脚本

脚本保存在工具板 384 KiB 的 `workspace` LittleFS 分区内，与网页包共享空间，普通重启和应用 OTA 切槽后可按名称继续运行。支持最多 64 份脚本，名字长 5–48 个 ASCII 字符，首字符为英文字母或数字，后续允许字母、数字、点、下划线和连字符，并以小写 `.lua` 结尾。

| CLI 子命令 | 参数和行为 |
| --- | --- |
| `lua FILE\|-` | 执行本地 UTF-8 Lua 文件。用 `-` 从标准输入读取源码。可选 `--timeout-ms 100..10000`，默认 5000，须持有效接管租约。 |
| `script list` | 列出脚本的 `name`、`bytes`。需要设备密钥，允许只读访问。 |
| `script upload NAME FILE\|-` | 把文件或标准输入保存为 `NAME`。须持有效接管租约。写完后原子替换同名旧文件，响应含 `saved`、`name`、`bytes`。 |
| `script run NAME` | 执行已保存的 `NAME`。可选 `--timeout-ms 100..10000`，默认 5000，须持有效接管租约。 |

全局 `--url`、`--key-file`、`--lease-file` 放在子命令之前。从标准输入读取 Lua 源码时用 `--key-file` 提供设备密钥，避免和 `--key-stdin` 占用同一输入流。定义「快速开始」中的 `board_rpc` 函数，再把其中的 Lua 正文保存为 `check-state.lua`，即可用以下命令上传并按名称执行。

```sh
(
  set -e
  board_rpc control acquire --ttl 120
  trap 'board_rpc control release >/dev/null' EXIT
  board_rpc script upload check-state.lua ./check-state.lua
  board_rpc script list
  board_rpc script run check-state.lua --timeout-ms 1000
)
```

## HTTP 接口

以下接口使用工具板实际地址。所有 Lua 相关请求均携带 `X-Board-Key`，AI 调用的写接口还须携带 `X-Board-Lease`。发送源码请求时用 `Content-Type: text/plain; charset=utf-8`，请求体为原始文本。

| 方法与路径 | 查询参数、请求体和结果 |
| --- | --- |
| `POST /api/lua` | 可选 `timeout_ms`，请求体为源码。须持租约。返回运行结果。 |
| `GET /api/scripts` | 无参数。返回 `{"scripts":[{"name":"check-state.lua","bytes":120}]}`。 |
| `GET /api/scripts/source` | 必填 `name`。返回源码文本和引号包围的 SHA-256 `ETag` 响应头。 |
| `POST /api/scripts` | 必填 `name`，请求体为源码。须持租约，支持直接覆盖同名文件。返回 `saved`、`name`、`bytes`。 |
| `POST /api/scripts/run` | 必填 `name`，可选 `timeout_ms`，使用空请求体。须持租约。返回运行结果。 |
| `POST /api/workbench/lua` | 网页写入口，要求无活动租约，省略 `X-Board-Lease`。执行用 `op=run` 和可选 `timeout_ms`。保存用 `op=save` 和必填 `name`。两者请求体均为源码。 |

`POST /api/lua` 省略整个查询串时使用默认时限。发送查询串时须包含合法的 `timeout_ms`。其余执行入口即使包含其他必填查询参数，也可独立省略 `timeout_ms`。

网页保存时，创建文件携带 `If-None-Match: *`，更新文件携带从读取源码获得的完整 `If-Match`（含引号）。二者择一。版本不匹配、待更新文件已消失或新名字已存在时返回 412。保存成功返回 `saved`、`name`、`bytes`、`etag`。文件不存在返回 404，存储不可用返回 503，存储满或文件数已满返回 507。

AI 执行入口的结果会发布到 `GET /api/workbench/result` 的最新快照，`op` 为 `lua`。网页 Lua 面板也会显示本页发起的运行结果。

## 网页工作流

打开板载「Lua 脚本」页，在左侧文件树新建或打开脚本。「保存到板卡」持久化当前源码，「运行」直接执行编辑区内容。快捷键为 Ctrl／Cmd+S 与 Ctrl／Cmd+Enter。有行号的错误可跳转回对应源码。版本冲突时可保留草稿并重新打开板上的版本。AI 接管时页面保留草稿、脚本列表和查看功能，租约释放后可再次编辑和运行。Ace 编辑器加载失败时可继续使用文本框。
