# Lua 与网页工作台

在需要把多步实验放到 AI Board 上执行、保存可重跑的脚本，或查看板卡状态与实测波形时读本页。先配置宿主机的 `board_rpc` 命令，按需核对接线约束、命令参数和采集条件。 [CLI 与设备维护](board-cli-and-maintenance.md) · [矩阵与仪器](matrix-and-instruments.md)

## 在 AI Board 上执行 Lua

Lua 5.5.1 运行于 AI Board 的 ESP32-C3。每次运行创建独立虚拟机。适合有限步数的接线、激励、应答和检查。目标 MCU 上的业务固件仍走目标 MCU 的构建与烧录流程。

| 操作 | 用法 |
| --- | --- |
| 读状态 | `board.call("state")` 返回接线掩码 `routes`、通道模式 `modes`、`levels` 和 `armed`。Lua 数组从 1 开始，通道编号从 0 开始。 |
| 调用硬件操作 | `board.call(op, params)` 使用工作台的操作名和参数，例如 `board.call("gpio", {y=4, mode="input"})`。键为操作参数名，值可用整数、合规 ASCII 字符串和布尔值。布尔值转为 `yes` 或 `no`。操作失败返回 `nil, error`，可用 `assert` 立即结束实验。 |
| 保留原始响应 | `board.call(op, params, "json")` 返回 JSON 字符串，可用于较大的采集结果。`board.null` 表示 JSON 的 `null`。 |
| 等待与输出 | `board.sleep(ms)` 按 FreeRTOS tick 等待，精确总线时序由固件驱动产生。`print()` 写入运行日志。`return` 返回可表示为 JSON 的标量、数组或对象。 |

每次接收 1–8192 字节的 UTF‑8 文本，Lua 堆上限 64 KiB，执行超时可设为 100–10000 ms，默认 5000 ms，另有指令数与返回体限制。提供基础函数和部分 `table`、`string`、`math` 接口，`pcall`、`xpcall`、`load` 和字符串模式匹配函数在当前固件中已移除。复杂解析或长时间等待放在宿主机。成功应用的硬件改动在脚本出错后仍生效，读回状态再决定如何恢复。

持有 AI 租约时可以直接发送文件或标准输入。以下脚本只读回状态。

```sh
board_rpc_locked lua - --timeout-ms 5000 <<'LUA'
local state = assert(board.call("state"))
local connected = {}
for index, mask in ipairs(state.routes) do
    if mask ~= 0 then connected[#connected + 1] = index - 1 end
end
print("Connected channels", #connected)
return {armed = state.armed, connected = connected}
LUA
```

CLI 输出包含 `ok`、`elapsed_ms`、`log`，成功时提供 `result`，脚本失败时提供 `kind` 与 `error`，并以非零状态退出。例如需要保存证据时，可将标准输出重定向到文件，即便脚本报错也保留返回的 JSON。

## 保存与重跑

```sh
board_rpc script list
board_rpc_locked script upload workflow.lua ./workflow.lua
board_rpc_locked script run workflow.lua --timeout-ms 5000
```

`script list` 只需设备密钥。上传和运行需要有效租约。`script upload NAME -` 支持从标准输入读源码，命令同时读取密钥时用 `--key-file`。脚本存于 `workspace` LittleFS 分区，应用 OTA 与重启后可以按名称运行。名称须以英文字母或数字开头，以 `.lua` 结尾，最多 48 字符，中间还允许点、下划线、连字符。单份 8 KiB，最多 64 份，空间与网页包共用。确认保存意图后再用同名上传替换已有文件。

如果已有直接 RPC 客户端，`GET /api/scripts` 列出文件，`GET /api/scripts/source?name=NAME` 返回文本与 `ETag`。CLI 的运行、上传和按名运行分别走 `POST /api/lua`、`POST /api/scripts?name=NAME` 和 `POST /api/scripts/run?name=NAME&timeout_ms=5000`。源码请求使用 `text/plain; charset=utf-8`，按名运行用空请求体。

## 网页查看与操作

从能够访问板卡的浏览器打开实际板卡地址。设备密钥保存在当前浏览器会话，别名、预设及操作历史保存在当前浏览器。AI 持有租约时，网页切成只读模式，可以查看状态、波形、协议结果和脚本。已有草稿保留，释放租约后恢复编辑。操作权限回归后，应用旧草稿前先核对接线状态。

| 页面 | 关键操作 |
| --- | --- |
| 接线矩阵 | 单击交叉点或用「从接线说明填入草稿」批量解析映射，例如 `BBX02→Y1, BBX02→Y3`。确认信号组电气拓扑后点击「应用接线」。用「读回状态」确认实机状态。通道控制负责 GPIO 模式和 PWM，线名用于当前浏览器标注端口。启用 OUT 控制的固件显示「OUT 供电」卡片。 |
| 协议工具 | UART 收发、I²C 扫描和寄存器读、SPI 交换、单轮 UART 代理、板内 UART 回环。查看或复制操作结果。 |
| 信号捕获 | 选择通道、GPIO 长窗或 DMA 同步采样，以及手动、上升沿或下降沿触发。板内采集自检包括 I²C／SPI 参考波形和主机采集。 |
| 协议接收 | I²C 从机可设固定应答、寄存器表、地址自增与板内回环。SPI 从机支持只收 MOSI、固定响应的全双工和板内压测。UART 接收可设批次、总等待与空闲停止条件。结果可查看、复制 JSON。 |
| Lua 脚本 | 从「工作区」文件树打开或新建草稿，切换含未保存改动的文档会确认。运行使用编辑区当前源码，保存写入 LittleFS。出现版本冲突时保留草稿并提示重新打开或另存。Ace 编辑器提供高亮、缩进、折叠和查找，CDN 不可达时可用文本框。快捷键为 Ctrl／Cmd+S 保存、Ctrl／Cmd+Enter 运行，可从有行号的错误跳回源码。 |
| 测试记录 | 查看历史并导出 JSON。预设可保存草稿接线和端口别名，也可附带用相同接线成功执行过的最近一次协议操作及预期结果。「载入草稿」后核对再应用。运行回归会应用保存的接线、重发协议请求并比较结果。 |
| 设备管理 | 矩阵启用与 448 组板内自检、2.4 GHz Wi‑Fi 配网、工具板应用固件 OTA、运行诊断和 Lua 工作台入口。操作前确认设备和接线条件。 [CLI 与设备维护](board-cli-and-maintenance.md) |

带租约运行的 CLI 波形与协议采集可同步到对应网页面板。使用 CLI 的标准输出或 `board_rpc result` 获取最近的已发布结果。网页 Lua 面板显示本页触发的运行结果，CLI Lua 的原始响应由 CLI 保留。

## 波形查看与证据交付

- 使用「信号捕获」的「导入 JSON」打开已保存的板卡采集，选择 UART、I²C、SPI 或原始波形。I²C 用 A=SCL、B=SDA，SPI 用 A=CS、B=SCK、C=MOSI、D=MISO，按实际模式和位序解码。UART 的波特率 `0` 表示自动估计。
- 用缩放键或滚轮缩放时间轴，拖动波形或总览平移。可拖动总览边界，或输入单位为毫秒的显示窗口起点和终点。「全览」恢复完整记录。
- A、B 游标会吸附附近边沿，可测时间差、频率和升降沿计数。指向完整脉冲可看脉宽、周期和占空比。I²C、SPI 解码列表可定位对应交易，UART 字节能和采集边沿对齐时会标在时间轴上。
- 用有效采样区间支持结论，并注明触发后尚未开始采样的间隔、边沿超限或超时。可复制原始 JSON，或导出 1 ns 时间基准的 VCD。测量分辨率取决于采集引擎和实际采样间隔。
