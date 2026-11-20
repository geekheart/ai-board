# AI Board 命令行接口

用命令行操作局域网上的 AI Board，并保存可复查的实板结果。目标板固件仍由目标板的 SDK、烧录工具和日志接口处理。示例从项目根目录运行，通道、地址与数据按实际接线和器件手册填写。

[连接和接管](#连接和接管) · [顶层命令](#顶层命令) · [工作台操作](#工作台操作) · [串口和 OTA 辅助脚本](#串口和-ota-辅助脚本) · [结果与错误](#结果与错误)

## 连接和接管

`tools/board_rpc.py` 使用 Python 3 标准库。常用语法如下。

```sh
python3 tools/board_rpc.py [全局选项] 命令 [命令参数]
```

| 全局选项 | 参数与行为 |
| --- | --- |
| `--url URL` | 板卡的 HTTP 或 HTTPS 基础地址。默认 `http://192.168.4.1`，电脑需接入板卡 AP 才能使用这个默认地址。接入同一局域网后，填写工具板实际的 LAN 地址。URL 采用无凭据、查询和片段的形式。 |
| `--source-ip IP` | 绑定电脑本机的源 IP，适用于有多个网卡的电脑。省略时由系统决定路由。 |
| `--key-file PATH` | 读取本机私有文件中的 UTF‑8 设备密钥，去掉首尾空白。与 `--key-stdin` 互斥。 |
| `--key-stdin` | 从标准输入的首行读取密钥。两个密钥选项都省略时，对需要鉴权的命令交互询问密钥。用标准输入传 Lua 源码时，配合 `--key-file`。 |
| `--lease-file PATH` | 本机租约文件。放在可写私有目录。CLI 以 `0600` 创建文件，读取时要求普通文件、拒绝符号链接与组或其他用户的访问权限。文件绑定到板卡基础 URL。 |

`status` 和 `control status` 可匿名执行，其余命令需要设备密钥。密钥通过 `X-Board-Key` 请求头发送，租约通过 `X-Board-Lease` 发送。CLI 直连板卡，单次 HTTP 请求超时为 60 s。将板卡 HTTP 接口用于可信局域网，将密钥、租约文件和 Wi‑Fi 密码保存在本机私有位置。

```sh
BOARD_URL='http://192.0.2.10'  # 替换为工具板的实际地址
BOARD_KEY_FILE="$HOME/.config/ai-board/key"
BOARD_LEASE_FILE="$HOME/.config/ai-board/lease.json"

board_rpc() {
  python3 tools/board_rpc.py --url "$BOARD_URL" --key-file "$BOARD_KEY_FILE" "$@"
}

board_rpc_locked() {
  board_rpc --lease-file "$BOARD_LEASE_FILE" "$@"
}

board_rpc status
board_rpc control status
board_rpc workbench
board_rpc_locked control acquire --ttl 300
# 在此执行写入或实验，用 board_rpc_locked 携带租约。
board_rpc_locked control release
```

全局选项放在命令前。租约生效时，只有持有对应令牌的客户端能执行受接管约束的操作，网页与串口仍可读状态。空闲时，固件允许经设备密钥鉴权的普通工作台操作。Lua 执行、脚本上传、按名运行、网页包上传始终需要租约。重启后租约失效。如果租约文件残留，先查 `control status`，再决定是否获取新租约。释放租约只修改板端控制状态，文件保留在本机。

## 顶层命令

| 命令 | 参数、结果和使用条件 |
| --- | --- |
| `status` | 无参数。返回 `project`、`built`、`partition`、`app_sha256`、`ota_pending`、`ap_ssid`、`station_ssid`、`station_ip`、`station_netmask`、`matrix_armed`。OTA 验收同时核对运行分区、镜像 ELF 摘要和待确认状态。 |
| `diagnostics` | 无参数。返回版本 `version`、`uptime_ms`、`reset_reason`、`free_heap_bytes`、`minimum_free_heap_bytes`、`station_connected`、`rssi_dbm` 和 `wifi_channel`。 |
| `logs [--limit N] [--after SEQ]` | `N` 为 1–64，默认 16。省略游标时读取最近日志，给出 `SEQ`（0–4294967295）时按序读取游标之后的事件。返回 `latest`、`dropped`、`events[]`，每条含 `seq`、`uptime_ms`、`kind`、`code`、`detail`。日志缓存在 RAM，重启后要重新确定游标。 |
| `control status` | 返回 `active`、`remaining_s` 和工作台结果的 `revision`。 |
| `control acquire [--ttl S]` | 需要全局 `--lease-file`。租约时长 30–1800 s，默认 300 s。成功后 CLI 把令牌写入租约文件，命令输出只显示状态。进行中的写操作或已活动的租约会阻挡获取。 |
| `control renew [--ttl S]` | 需要有效租约文件。时长范围与默认值和获取时一致，从续租时刻重新计时。 |
| `control release` | 需要有效租约文件。写操作进行中时会返回忙，完成后再释放。 |
| `scan` | 扫描可见 Wi‑Fi，最多返回 12 项。每项含 `ssid`、`channel`、`rssi` 和 `auth`。已有租约时由持有者携带租约执行。 |
| `wifi SSID [--password-file PATH]` | SSID 为 1–32 字节。密码为 8–63 字节，开放网络用空密码。密码文件只去掉末尾换行，省略该选项时以无回显方式询问。返回 `saved`、`rebooting`。重启后用 AP、串口或新 LAN 地址回读 `status`。 |
| `arm [--confirm-dtr-disconnected]` | 工具板 GPIO9 是 BOOT 与 CH446Q 时钟的复用脚。确认工具板的 DTR→BOOT 已断开，且符合 CLI 提示的 BBX 空载条件后才使用确认选项。省略时交互输入 `yes`。返回 `matrix_armed`。工具板重启后需重新启用。 |
| `selftest` | 矩阵须已启用、路由清空、外部 BBX 实际空载。驱动板内回环，返回 `passed`、`failed` 和 `total`，总数为 448。 |
| `workbench` | 无参数时读取状态。返回 `armed`，以及按 Y0–Y7 排列的 `routes`、`levels`、`modes`、`gpios`。启用 OUT 的固件还返回 `out_mode`。 |
| `workbench OP KEY=VALUE ...` | 执行下文的工作台操作。先用 `workbench` 读回实际路由，写入后核对对应响应。 |
| `result` | 读板卡最近一次已发布的 AI 操作，返回 `revision`、`op`、`result`。持有租约的工作台调用可发布结果。`route`、`reset`、`gpio`、`power` 只更新状态。结果缓存在 RAM，发布结果超过 24 KiB 时正文可能为 `null`。需要留存时保存原命令输出。 |
| `lua FILE\|- [--timeout-ms MS]` | 从文件或标准输入读取 1–8192 字节的 UTF‑8 源码。脚本在工具板本地运行，超时为 100–10000 ms，默认 5000。详见 [Lua 接口](Lua.md)。 |
| `script list` | 列出 LittleFS 脚本，返回 `scripts[]`，每项包含 `name` 和 `bytes`。 |
| `script upload NAME FILE\|-` | 上传脚本，已有同名文件会被替换。文件名共 5–48 个 ASCII 字符，首字符为字母或数字，以 `.lua` 结尾，中间可用字母、数字、点、下划线或连字符。源码上限与 `lua` 相同。返回 `saved`、`name` 和 `bytes`。 |
| `script run NAME [--timeout-ms MS]` | 运行板上已保存的脚本。超时范围与默认值和 `lua` 一致。 |
| `web status` | 返回活动网页版本 `active`，内置页面显示为 `embedded`。 |
| `web upload DIR` | 打包、发布目录中的 `index.html`、`style.css` 和 `app.js`。HTML 里须各引用一次本地样式和脚本。打包上限为 96 KiB，包含包头和 gzip 资源。固件校验整包后切换，返回 `active`。刷新网页验收，可从 `/recovery` 打开固件内置页面。 |
| `ota BINARY` | 上传工具板的 ESP-IDF 应用镜像 `ai_board.bin`。固件写入下一 OTA 槽，接受后返回 `accepted`、`rebooting`。重新连接后用 `status` 核对分区与 ELF 摘要。修改 Flash 分区表走独立的串口迁移流程。 |

工具板既可从 LAN 使用，也可通过板卡自己的 Wi‑Fi AP 配置网络。网络变更、OTA、重启会改变连接状态，恢复连接后重新读回租约、矩阵和引脚模式。已接外设的工具板重启后，若已确认 DTR 断开，可在接管前从网页「设备管理 → 启用矩阵」恢复矩阵启用状态。持有租约的 RPC 客户端也可向 `POST /api/matrix/arm` 提交空请求体、有效密钥和租约，并带 `X-DTR-Disconnected: yes`。该接口设置启用状态，板内自检仍要求物理 BBX 空载。

## 工作台操作

`KEY=VALUE` 字段名须唯一，整数用十进制，字节串用连续、偶数长度的十六进制字符，如 `55A600FF`。除有条件注明的参数外，按表填写。把命令表中的操作放到 `board_rpc_locked workbench` 后。CLI 把 `op` 和参数编码为表单，请求体最多 1279 字节。固件在处理工作台操作时会占用矩阵，实验中的目标触发可以从独立 USB、编程器或目标板接口发出。

### 路由、GPIO 与目标供电

CH446Q 的 X0–X15 对应 BBX00–BBX15。Y0–Y7 对应 GPIO18、8、6、5、19、4、3、1。H1.1、H1.3 为 GND，H1.2 为 3V3，H1.4 为 OUT。信号须共地并兼容 3.3 V。先计算整组连通关系，避免把两个主动输出接到同一网络。

| 操作 | 参数 | 结果与约束 |
| --- | --- | --- |
| `route` | 必填 `y0` 至 `y7` 的全部八个掩码，每个为 0–65535。连接 BBXn 对应 `1 << n`，多个点的掩码按位或。 | 返回工作台状态。应用新路由前会释放所有 Y 的驱动，重设为 `input`。一个 Y 可连多个 BBX，多个 Y 也可共用同一 BBX，各连接的共点整个电气网络都会贯通。 |
| `reset` | 无。 | 清空工具板的所有路由，所有 Y 恢复 `input`。这是矩阵复位。 |
| `gpio` | `y=0..7`，`mode=input/pullup/pulldown/low/high/pwm`。PWM 还需 `hz=100..20000` 和整数 `duty=0..100`。 | 返回状态。主动驱动的网络须只有一个 BBX 和一个输出，可有多个输入 Y。整块板同时只启用一路 PWM。 |
| `power` | `mode=5v/gnd`。 | 需要启用 OUT 的固件，可在矩阵未启用时执行。H1.4 在 `5v` 档供电，`gnd` 档关断 5 V 并经 330 Ω 下拉。状态中的 `out_mode` 是 CTRL 读回。做冷重启时，还需确认目标实际掉电且 USB 供电路径不会维持它运行。 |

例如用 Y3 和 Y4 连接同一 BBX02，Y3 产生 250 Hz PWM，Y4 作高阻输入。以下操作会向 BBX02 输出，按实际接线确认该点兼容主动驱动。

```sh
board_rpc_locked workbench route y0=0 y1=0 y2=0 y3=4 y4=4 y5=0 y6=0 y7=0
board_rpc_locked workbench gpio y=3 mode=pwm hz=250 duty=50
board_rpc_locked workbench capture a=4 ms=40 > pwm.json
board_rpc_locked workbench gpio y=3 mode=input
```

### 主动协议和接收

主动协议操作所选的 Y 均为 `input`，各协议角色使用彼此独立的电气网络。每个网络只有一个 BBX，并允许附加高阻输入 Y。网络中应无其他 GPIO 输出。I²C 外接设备按其规格提供上拉。将目标主控与外设通过矩阵接通后，使用高阻采集旁听。

| 操作 | 参数 | 响应 |
| --- | --- | --- |
| `uart` | `tx`、`rx`，`baud=1200..115200`，`read=1..64`，`timeout=20..500` ms。可选 `hex` 为 0–64 字节，省略时仅接收。8N1。 | `hex`、`received`。 |
| `proxy` | `host_tx`、`host_rx`、`device_tx`、`device_rx`。名称以外部端点为准。`baud=1200..115200`，`timeout=20..500` ms。隔开自有主机和设备，分别接入四条线。最多 64 字节一侧。 | 一轮 UART 8N1 请求与应答，返回 `request_hex`、`response_hex`。 |
| `uart_receive` | `rx`，`baud=9600..5000000`，`bytes=1..2048`，`wait_ms=100..5000`，`idle_ms` 设 0 禁用或设 10–1000，且不超过 `wait_ms`。8N1。 | `rx_hex`、`requested_baud`、`actual_baud`、`received`、`timed_out`、`idle`、`limit_reached`、`overflow`、`events_lost`、`frame_errors`、`parity_errors`、`break_events`。 |
| `i2c_scan` | `sda`、`scl`。探测 7 位地址 8–119。 | `addresses`。 |
| `i2c_read` | `sda`、`scl`，`address=8..119`，`reg=0..255`，`count=1..64`。时钟 100 kHz。 | 先写一个寄存器地址，再读数据。返回 `hex`、`received`。 |
| `i2c_transfer` | `sda`、`scl`，`address=8..119`，`hz=5000..400000`，`read=0..64`，可选 `hex` 最大 64 字节。至少读或写其中之一。 | 单次读、写或重复 START 写读，返回 `hex`、`received`。多字节寄存器地址用 `hex` 传入。 |
| `spi` | `sck`、`mosi`、`miso`、`cs`，`hz=1000..20000`，`hex` 为 1–64 字节。Mode 0、MSB 优先、CS 低有效。 | `hex`、`received`。 |
| `spi_slave` | `cs`、`sck`、`mosi`，`miso=-1..7`，`mode=0..3`，`order=msb/lsb`，`bytes=4..64` 且为 4 的倍数，`count=1..8`，`wait_ms=100..5000`。单收设 `miso=-1` 并省略 `reply`。全双工指定 MISO 和 `reply`，应答长度为 1 至 `bytes`，不足容量的部分补零。 | `mode`、`order`、`reply_hex`、`queued`、`captured`、`timed_out`。每帧有 `bits`、`rx_hex`、`truncated` 和 `length_saturated`。 |
| `i2c_slave` | `sda`、`scl`，`address=8..119`，`bytes=1..64`，`count=1..8`，`wait_ms=100..5000`。可追加下方的固定应答或寄存器表参数。 | `address`、`queued`、`captured`、`timed_out`。写帧 `frames[]` 包含 `bytes`、`total_bytes`、`rx_hex` 和 `truncated`。超时也可携带此前已捕获的帧。 |

`i2c_slave` 支持以下应答方式。

- 单寄存器固定应答。加 `register=0..255` 和 `reply`（1–32 字节），`count` 固定为 1。主控写寄存器地址并重复 START 读取，响应另含 `register`、`read_requests`、`reply_loaded`、`reply_loaded_bytes`、`read_completed`、`reply_hex`。
- 寄存器表。用 `registers` 提供 1–8 个不同地址。每项按「一字节寄存器地址、一字节长度、1–32 字节的数据」连接成十六进制串。比如 `2301A59A025A00` 表示 0x23 返回 `A5`，0x9A 返回 `5A00`。主控可写入同一寄存器的完整数据，更新作用于当前采集批次。返回 `register_table` 和 `transactions[]`，逐笔给出 `register`、`matched`、`write_applied`、`reply_loaded_bytes`、`read_completed`、`write_hex`、`reply_hex`。
- 自动递增。寄存器表可加 `auto_increment=yes`，一次按相邻地址访问最多 8 项、合计 32 字节。地址最多到 0xFF。写入需覆盖完整的相邻寄存器。带自动递增的结果补充 `auto_increment` 和交易的 `registers[]`。普通单寄存器与寄存器表应答二选一。

### 数字信号采集

`capture` 选用已接通、处于 `input`、`pullup` 或 `pulldown` 的 Y。结果在捕获结束后一次返回，经重定向可保存为 JSON，在网页导入、解码和导出 VCD。用独立目标板控制通道触发被测交易。SPI 从机与 DMA 波形采集共享 SPI2，分开执行。

| 参数 | 范围或默认值 |
| --- | --- |
| `a`、`b`、`c`、`d` | A 必填，通道为 Y0–Y7。B–D 可省略或填 `-1`。所选 Y 各不相同。 |
| `engine` | 默认 `gpio`，也可用 `dma`。 |
| `ms` | GPIO 长窗口必填，10–500 ms。 |
| `sample_hz`、`window_us` | DMA 必填，分别为 1000000–20000000 Hz、100–65536 µs。两数相乘最多 `65536 × 1000000`，即最多 65536 个采样点。 |
| `baud` | UART 解码提示，默认 0 表示自动尝试。手动指定 1200–4000000 baud。 |
| `trigger` | `manual`（默认）、`rising` 或 `falling`。 |
| `trigger_channel`、`wait_ms` | 边沿模式必填。触发通道按 A–D 填 0–3，须是已选通道。等待 100–5000 ms。 |

输出含 `duration_ms`、`engine`、`trigger`、`trigger_channel`、`triggered` 和 `channels`。边沿等待超时用 `triggered=false` 表示，通道数组为空。DMA 另含 `sample_hz`、`sample_start_us`。后者表示相对于触发点的硬件采样起始估计。

每路包含 `slot`、`y`、`initial`、`edge_count`、`window_us`、`edges`、`overflow`。边沿格式为 `[时间（µs）, 电平]`，每路最多 256 个。UART 解码另含 `baud`（无法识别时为 `null`）、`hex`、`framing_errors`、`uart_truncated`，最多解码 64 字节。判定时同时检查触发、窗口和截断标志。

### 板内回环和压测

以下操作均需矩阵已启用、清空路由、外部 BBX 实际空载，并提交 `confirm_empty=yes`。内部夹具会主动驱动 BBX。`selftest` 本身是顶层命令。

| 操作 | 参数和默认值 | 判定依据 |
| --- | --- | --- |
| `uart_loopback` | `baud` 可选，默认 9600，范围 1200–4000000。支持 `engine=gpio/dma`、DMA 的 `sample_hz` 与 `window_us`、`trigger=manual/rising/falling`。GPIO 窗口固定为 100 ms，边沿等待固定为 250 ms，采集通道固定为 Y4。 | Y7 经 BBX00 发给 Y2 和 Y4。核对 `passed`、`capture_passed` 和期望 `55A600FFC346`，并检查 `capture_baud`、`capture_hex` 和 `framing_errors`。 |
| `i2c_capture_loopback`、`spi_capture_loopback` | GPIO 引擎，固定 80 ms 窗口。可选 `trigger`，默认下降沿。边沿模式可选 `trigger_channel`，I²C 为 0–1，SPI 为 0–3。默认触发 I²C 的 SDA 或 SPI 的 CS。边沿等待固定为 250 ms。 | 输出捕获 JSON，夹具标识分别为 `i2c_7bit`、`spi_mode0`。 |
| `i2c_host_capture`、`spi_host_capture` | 窗口、触发和槽位与参考波形相同，GPIO 之外还可用 DMA 并指定采样率与窗口。I²C `hz` 默认 5000，范围 5000–400000。SPI `hz` 默认 10000，GPIO 模式支持 1000–39000 或 40000–1000000，DMA 模式支持 10000–3333334。 | 输出捕获 JSON。空载 I²C 预期地址 NACK，SPI 预期 MOSI 和 MISO 内容相同。 |
| `protocol_receive_bench` | `mode=uart/i2c`，`rate` 为 UART 9600–5000000 baud 或 I²C 10000–400000 Hz。 | 比对内置发送样本和接收数据。看 `passed`、`requested_rate`、`fixture_rate`、`fixture_status` 与 `expected_hex`。 |
| `i2c_register_bench` | `rate=10000..400000`，`address=8..119`，`register=0..255`，`reply` 为 1–32 字节。 | 主机重复 START 读回固定应答，核对 `passed`、`readback_hex` 及从机的读请求和完成状态。 |
| `i2c_register_table_bench` | `rate=10000..400000`，`address=8..119`，必填 `registers`。可用同编码的 `writes` 更新表中已有地址。或加 `auto_increment=yes`、`start=0..255`、`burst`（1–32 字节），连续写满相邻寄存器再读回。自动递增与 `writes` 二选一。 | 核对整体 `passed`，以及每笔交易的 `passed`、`fixture_status`、`readback_hex`。 |
| `spi_slave_bench` | `phase=rx/duplex/gap`，前两种单帧测试的 `hz=10000..20000000`。双帧 `gap` 阶段的 `hz=10000..3333334`，并指定 `gap_ns=150..1000000`。夹具以 Mode 0 测固定 4 字节。 | 核对 `passed`、`rx_passed`、`tx_verified`、`requested_hz`、`clock_hz`、`gap_ns`、`fixture`（`rmt` 或 `i2s`）、`frames` 及 `observed_reply`。 |

## 串口和 OTA 辅助脚本

### 串口采集

`tools/serial_capture.py` 需要 `pyserial`，使用工具板 UART0 控制台，波特率固定为 115200。预先启用矩阵并设置采集路由，确认工具板无活动租约，再打开串口。

| 参数 | 范围和默认值 |
| --- | --- |
| `PORT Y [Y ...]` | 串口设备名，以及 1–4 个不同 Y（如 `0` 或 `Y0`），位置顺序为 A–D。 |
| `--ms MS` | 窗口 10–500 ms，默认 150 ms。 |
| `--baud BAUD` | UART 解码速率，默认 0（自动），或 1200–115200 baud。 |
| `--trigger manual\|rising\|falling` | 默认 `manual`。 |
| `--trigger-channel A\|B\|C\|D` | 边沿触发通道，默认 A。 |
| `--wait-ms MS` | 边沿等待 100–5000 ms，默认 5000 ms。 |
| `--output FILE`、`-o FILE` | 默认 `-`，输出到 stdout。路径指向本机 JSON 文件。 |

```sh
python3 tools/serial_capture.py "$BOARD_SERIAL_PORT" 0 1 \
  --trigger falling --trigger-channel B --ms 100 --wait-ms 3000 -o capture.json
```

手动进入串口控制台可查看 `help`、`status`、`scan`，交互执行 `wifi`，或在确认 DTR 断开后执行 `arm dtr-disconnected`。控制台也提供 `selftest` 与 `capture <form>`。

### 同网段 OTA 验收

`tools/lan_smoke.py` 验证工具板身份和网络，上传应用镜像并轮询到运行分区切换、OTA 完成确认、运行 ELF 摘要匹配。该脚本没有租约参数，执行时板卡须无活动租约。

| 参数 | 范围和默认值 |
| --- | --- |
| `BOARD_IP IMAGE` | 工具板的实际 LAN IPv4 和构建得到的 `ai_board.bin`。 |
| `--host-cidr IP/PREFIX` | 必填，执行电脑的 IPv4 和网络前缀。两台设备须在同一子网，且地址不同。 |
| `--ssid SSID` | 可选，核对板卡连接的 Wi‑Fi 名称。 |
| `--key-stdin` | 从 stdin 首行读取密钥。省略时以无回显方式询问。 |
| `--boot-timeout S` | 大于零的秒数，默认 60。这里表示镜像被接受后等待新分区启动并确认的时间。 |

```sh
python3 tools/lan_smoke.py "$BOARD_IP" "$AI_BOARD_APP_BIN" \
  --host-cidr "$BOARD_HOST_CIDR" --key-stdin < "$BOARD_KEY_FILE"
```

首次串口安装工具板时，使用 ESP-IDF 环境，从构建输出目录调用 `python3 "$AI_BOARD_REPO/tools/serial_bootstrap.py" "$BOARD_SERIAL_PORT"`。脚本只有串口位置参数，假定 DTR→BOOT 与 RTS→EN 已按板级设计接好。它写入 bootloader、分区表、OTA 初始数据和应用镜像，退出前会读取包含设备密钥的启动日志。安装与迁移步骤见 [固件说明](../README.md)。

## 结果与错误

成功的 HTTP CLI 请求以 JSON 输出，命令行解析错误退出码为 2，通信、HTTP 与 Lua 执行失败通常为 1。工具板返回 Lua 的 `ok=false` 时，CLI 仍会先输出 JSON。`selftest` 的 `failed`、压测的 `passed`、采集的 `triggered` 和协议的 `timed_out` 须根据 JSON 判定。自动化时保留失败证据和临时路由的恢复步骤。

| HTTP 状态 | 排查方向 |
| --- | --- |
| 400 | 核对必填字段、数值范围、镜像大小或者命令对应的固件版本。 |
| 401 | 核对设备访问密钥和实际板卡身份。 |
| 404 | 核对脚本名或运行固件支持的接口。 |
| 408 | 网页包上传超时，先查已生效版本再决定是否重传。 |
| 409 | 检查矩阵是否已启用、路由和模式是否满足驱动条件，以及是否有其他工作台操作或重启正在进行。 |
| 422 | 查看底层协议错误，例如目标 NACK、收发超时或资源错误。 |
| 423 | 核对当前接管状态、租约地址和令牌有效期。 |
| 503、507 | 根据响应检查工具板当前服务状态或 LittleFS 可用空间。 |
