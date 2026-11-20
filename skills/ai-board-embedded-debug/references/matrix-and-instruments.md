# CH446Q 接线、协议和测量

先按 [CLI 与设备维护](board-cli-and-maintenance.md) 配置 `board_rpc` 和 `board_rpc_locked`，写入前取得租约。命令中的通道、地址和测试数据是接线示例。实际执行时使用已确认的接线和待测设备可接受的参数。

## 矩阵、GPIO 和 OUT

- CH446Q 的 X0–X15 对应 BBX00–BBX15。Y0–Y7 对应 ESP32-C3 的 GPIO18、8、6、5、19、4、3、1。H1.1 和 H1.3 为 GND，H1.2 为 3V3，H1.4 为可切换的 OUT。数字信号须共地并兼容 3.3 V，OUT 的 5 V 走供电口。
- `board_rpc workbench` 回读 `armed`、`routes`、`levels`、`modes` 和 `gpios`。未启用时先确认工具板的 DTR 已与 BOOT/GPIO9 断开。GPIO9 在工作时复用为 CH446Q 时钟。BBX 空载时可用 `board_rpc_locked arm --confirm-dtr-disconnected`。已接入外设并确认线路的重启恢复，可用网页「设备管理 → 启用矩阵」，或调用带 `X-DTR-Disconnected: yes` 的已授权 `POST /api/matrix/arm`。启用后再应用路由。
- `workbench route` 每次提交全部八个掩码 `y0` 至 `y7`。BBX 编号 n 对应掩码的第 n 位，`1 << n`。同一 Y 的多个 BBX 会导通，多条 Y 共用一个 BBX 也会导通。检查整个连通组，尤其是间接相连的输出。换路由会先释放所有 Y 的驱动，包括 PWM，再断开旧路由。工具板重启后路由清空。
- `workbench gpio` 的 `mode` 可用 `input`、`pullup`、`pulldown`、`low`、`high` 或 `pwm`。主动驱动的电气网络只允许一个 BBX，附加的 Y 可做高阻输入。可开启一路 PWM，`hz` 为 100–20000，`duty` 为整数百分比 0–100。例 `board_rpc_locked workbench gpio y=3 mode=pwm hz=250 duty=50`。默认用 `input` 旁听。
- `workbench reset` 清空矩阵和 Y 模式。启用 OUT 控制的固件支持 `board_rpc_locked workbench power mode=5v` 和 `board_rpc_locked workbench power mode=gnd`，可在矩阵未启用时执行。GND 档关掉 5 V，再以 330 Ω 电阻下拉 OUT。`out_mode` 来自 CTRL 电平读回。对目标做冷启动时，确认它的供电条件适配，并检查 USB 或其他线路是否维持供电。

例如传感器 SDA/SCL 分别接 BBX02/03，目标 MCU 的对应信号接 BBX04/05。矩阵已启用且持有租约时，可先让工具板主动访问传感器，再切给 MCU 并用 Y0/Y1 高阻采集。按器件文档替换示例的地址、写入字节和读取长度。启动最后一条采集命令后，从独立的目标板 USB 触发一次交易，或让目标固件定时发起同样的交易。

```sh
board_rpc_locked workbench route y0=4 y1=8 y2=0 y3=0 y4=0 y5=0 y6=0 y7=0
board_rpc_locked workbench i2c_transfer sda=0 scl=1 address=72 hz=100000 hex=23 read=2
board_rpc_locked workbench route y0=20 y1=40 y2=0 y3=0 y4=0 y5=0 y6=0 y7=0
board_rpc_locked workbench capture engine=gpio a=1 b=0 ms=100 trigger=falling trigger_channel=1 wait_ms=3000 > target-i2c.json
```

## 主动协议和批量接收

启动外部协议操作时，各协议角色独占不同的电气网络。每个网络须含一个 BBX，选用的 Y 处于 `input`，同组没有 GPIO 输出。确认外部器件的发送方向。I²C 总线使用与器件兼容的外部上拉。已连接目标主机时，旁听用 `capture`，切换主机前先隔离原主机。

| 操作 | CLI 示例和结果 |
| --- | --- |
| UART 8N1 收发 | `board_rpc_locked workbench uart tx=0 rx=1 baud=115200 hex=55A6 read=2 timeout=100`。1200–115200 baud，写入和读取上限各 64 字节，超时 20–500 ms。可省略 `hex` 只读。返回 `hex` 和 `received`。 |
| UART 8N1 批量接收 | `board_rpc_locked workbench uart_receive rx=1 baud=115200 bytes=64 wait_ms=1000 idle_ms=20`。9600–5000000 baud，最多 2048 字节，等待 100–5000 ms。`idle_ms=0` 禁用空闲结束，开启时为 10–1000 ms 且不超过等待时间。比较 `rx_hex`，同时看实际波特率、帧错误、超时和溢出。 |
| UART 单轮请求应答代理 | `board_rpc_locked workbench proxy host_tx=0 host_rx=1 device_tx=2 device_rx=3 baud=115200 timeout=100`。字段从外部端点看方向。用四条独立信号线分开自有主从设备，返回 `request_hex` 和 `response_hex`。速率和超时范围同 UART 收发。 |
| I²C 探测 | `board_rpc_locked workbench i2c_scan sda=0 scl=1`。返回应答的 7 位地址列表。 |
| I²C 单字节寄存器地址读 | `board_rpc_locked workbench i2c_read sda=0 scl=1 address=72 reg=35 count=2`。100 kHz，`reg` 为 0–255，读取 1–64 字节。 |
| I²C 主机通用传输 | `board_rpc_locked workbench i2c_transfer sda=0 scl=1 address=72 hz=100000 hex=2300 read=2`。地址用 7 位值 8–119，速率 5–400 kHz，写和读各至多 64 字节。只写设 `read=0`，只读省略 `hex`。同时指定写和读时，在一笔交易内用重复 START。 |
| SPI 主机收发 | `board_rpc_locked workbench spi cs=0 sck=1 mosi=2 miso=3 hz=10000 hex=9F0000A5`。Mode 0、MSB 优先、CS 低有效，1–20 kHz、1–64 字节。结果返回读到的 `hex`。 |
| SPI 从机批量收发 | `board_rpc_locked workbench spi_slave cs=0 sck=1 mosi=2 miso=3 mode=0 order=msb bytes=4 count=2 wait_ms=1000 reply=D2698B54`。Mode 0–3，`order=msb/lsb`，CS 低有效，容量 4–64 字节且为 4 的倍数，1–8 帧，等待 100–5000 ms。仅接收用 `miso=-1` 并省略 `reply`。全双工应答短于容量时补零，`reply_hex` 为配置的应答，`frames[].rx_hex` 和 `bits` 为实收证据。 |
| I²C 从机接收 | `board_rpc_locked workbench i2c_slave sda=0 scl=1 address=72 bytes=64 count=2 wait_ms=1000`。监听指定地址，写入由 START/STOP 分帧，批次 1–8 帧，每帧容量 1–64 字节，等待 100–5000 ms，超时可保留此前已结束的写入。 |
| I²C 从机固定读回 | 在 `i2c_slave` 加 `register=35 reply=A55A count=1`。主机写一个寄存器地址后发重复 START 读回 1–32 字节应答。 |
| I²C 从机寄存器表 | 在 `i2c_slave` 加 `registers=2301A59A025A00`。每项为十六进制的地址、长度和对应数据，可配置 1–8 个不同地址，每项 1–32 字节。主机可在写地址后选择该项，也可附数据更新整项，随后发重复 START 读当前值。更新限当前批次。 |

`i2c_slave` 的寄存器表可加 `auto_increment=yes`。一次先写起始地址，再按连续地址的固定分段长度读或写。最多跨 8 项、合计 32 字节，地址上限 `0xFF`。有效写入须覆盖连续、完整的寄存器。验收目标主机时，把从机报告的交易、接线和目标主机实际读回值对应起来。SPI 从机与硬件采样都占用 SPI2，安排在不同试验中。

## 逻辑采集和波形

最多采集四个不同的已接通 Y，`a` 必填，`b/c/d` 可省略或设 `-1`。采样 Y 设为输入，也可在需要时使用上下拉。运行在批次结束后一次性返回 JSON。HTTP 请求期间工作台占用矩阵，若要在等待时触发目标，使用独立的目标板控制通道。

| 模式 | 适用范围与命令 |
| --- | --- |
| GPIO 长窗口 | 10–500 ms，每路最多 256 个已记录边沿。例 `board_rpc_locked workbench capture engine=gpio a=4 ms=40 trigger=manual > capture.json`。 |
| 同步 DMA | 1–20 MS/s，窗口 100–65536 µs，采样率乘以窗口秒数最多 65536 点。例 `board_rpc_locked workbench capture engine=dma a=4 b=5 c=6 d=7 sample_hz=20000000 window_us=3000 trigger=manual > capture.json`。保存每路时间戳边沿，最多 256 个。 |
| 边沿启动 | 改用 `trigger=rising` 或 `trigger=falling`，指定 `trigger_channel=0..3` 对应 A–D 和 `wait_ms=100..5000`。所选槽须已分配 Y。`triggered=false` 表示等待超时。DMA 的 `sample_start_us` 表示触发后开始硬件采样的估算延迟，需要看清首帧时用手动窗口覆盖目标发送。 |

原始 JSON 的 `edges` 是 `[时间（µs）, 电平]`，起始电平是 `initial`。只在有效样本区间内计算周期、脉宽和占空比，核对 `window_us`、`overflow` 和 `sample_start_us`。跨协议比对时保留原始 JSON。UART 8N1 用 `baud=0` 尝试识别或给已知速率，检查帧错误与截断。I²C 解码通常按 A=SCL、B=SDA。SPI 四线按 A=CS、B=SCK、C=MOSI、D=MISO，使用实际 Mode 和位序。

网页「信号捕获」可导入 JSON、解码 UART/I²C/SPI、定位交易、导出 VCD。用滚轮改变时基、拖动波形平移，或在总览拖动窗口边界及输入起止时间。用 A/B 游标量时间差，用相邻同向边沿量周期，用完整脉冲量占空比。

## 空载板内自检和压测

确认物理 BBX 全部空载并清空路由，再运行下表相应项目。内部夹具会驱动引脚，并把多个 C3 GPIO 通过 CH446Q 的同一个 BBX 相连。软件清空矩阵无法代替现场确认外部 BBX 空载。

| 项目 | CLI 示例和判据 |
| --- | --- |
| 矩阵开关自检 | `board_rpc_locked selftest`。每个 BBX 测 28 对 Y，共 448 组。检查断开、传递高低电平、断开后重新接通和矩阵复位。以 `passed`、`failed`、`total` 判断。 |
| UART 回环 | `board_rpc_locked workbench uart_loopback confirm_empty=yes`。默认 9600 baud，Y7 通过 BBX00 发给 Y2，同时在 Y4 抓包。应收 `55A600FFC346`。 |
| I²C/SPI 参考波形 | `board_rpc_locked workbench i2c_capture_loopback confirm_empty=yes` 或 `board_rpc_locked workbench spi_capture_loopback confirm_empty=yes`。内部夹具生成已知信号，由不同的输入 Y 采集。 |
| I²C/SPI 主机采集 | `board_rpc_locked workbench i2c_host_capture confirm_empty=yes hz=100000` 或 `board_rpc_locked workbench spi_host_capture confirm_empty=yes hz=10000`。空载 I²C 探测预期地址 NACK，SPI 回环预期 MOSI/MISO 内容一致。两者均可选 DMA 和触发方式。 |
| UART / I²C 接收压测 | `board_rpc_locked workbench protocol_receive_bench confirm_empty=yes mode=uart rate=115200`，或 `mode=i2c rate=100000`。分别检验 UART 收到的实际字节和 I²C 的 ACK／写入数据。 |
| I²C 固定寄存器回环 | `board_rpc_locked workbench i2c_register_bench confirm_empty=yes address=72 rate=100000 register=35 reply=A55A`。比较夹具读回和预期。 |
| I²C 寄存器表回环 | `board_rpc_locked workbench i2c_register_table_bench confirm_empty=yes address=72 rate=100000 registers=2301A524015A writes=230100`。也可用同一 `registers`，将 `writes` 换成 `auto_increment=yes start=35 burst=1234` 测连续写入读回。 |
| SPI 从机压测 | `board_rpc_locked workbench spi_slave_bench confirm_empty=yes phase=duplex hz=1000000`。单帧 `phase=rx/duplex` 为 10 kHz–20 MHz，连续两帧 `phase=gap` 为 10 kHz–3333334 Hz 并指定 `gap_ns=150..1000000`。对比 `rx_passed`、`tx_verified`、实际时钟和实收数据。 |

内部压测的通过结果对应本次夹具、速率、时序和数据长度。用目标板实测结果判定外部接线和业务是否通过。
