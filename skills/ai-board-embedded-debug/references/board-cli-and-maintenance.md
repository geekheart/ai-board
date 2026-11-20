# 联机、接管与维护

## 连接工具板

在能访问板卡局域网的电脑或已授权的本地代理上运行命令。ChatGPT 能否直连取决于当前会话是否具备这条执行路径。缺少路径时，给用户可直接在本机运行的命令，请其返回脱敏后的 JSON。使用用户告知的设备地址或现有配置，确认 `status` 的 `project` 为 `ai_board`，并核对实际板卡；多个网卡时，可在子命令前加 `--source-ip "$BOARD_SOURCE_IP"` 绑定宿主机的源地址。

`BOARD_RPC` 指向仓库的 `tools/board_rpc.py`，导入 ZIP 时指向技能包内的 `scripts/board_rpc.py`。`BOARD_URL` 是确认后的完整地址，`BOARD_KEY_FILE` 是本机的私有密钥文件，`BOARD_LEASE_FILE` 是本机可写私有目录中的租约文件。下面的函数适用于 Bash 或 Zsh。

```sh
board_rpc() {
  python3 "$BOARD_RPC" --url "$BOARD_URL" --key-file "$BOARD_KEY_FILE" "$@"
}

board_rpc_locked() {
  board_rpc --lease-file "$BOARD_LEASE_FILE" "$@"
}

board_rpc status
board_rpc diagnostics
board_rpc workbench
board_rpc control status
board_rpc logs --limit 32
board_rpc result
```

`status` 和 `control status` 可匿名读，其他命令需要设备密钥。首选本机 `--key-file`，也可通过标准输入供给 `--key-stdin`，两者互斥。`lua -` 和 `script upload NAME -` 要从标准输入读取 Lua 源码，配合 `--key-file` 使用。设备访问密钥、Wi‑Fi 密码和租约令牌保存在私人位置，返回结果前清理其中的敏感内容。固件的 HTTP RPC 适用于可信局域网。

| 命令 | 用于确认的状态 |
| --- | --- |
| `status` | 工具板身份、当前分区、应用 ELF SHA‑256、OTA 确认状态、LAN 地址、矩阵已启用状态 |
| `diagnostics` | 固件版本、运行时间、复位原因、当前和最低可用堆、Wi‑Fi 链路 |
| `workbench` | 当前 `routes`、GPIO 模式和读回电平，及受支持固件的 `out_mode` |
| `control status` | 接管是否活动、剩余时间、操作结果版本号 |
| `logs --limit 32` / `logs --after "$LAST_SEQ" --limit 64` | RAM 中最近的结构化事件，最多 64 条。检查 `dropped`，重启后重新确定游标 |
| `result` | 最近一次发布的 AI 工作台结果。用本次命令的直接输出作为证据，按需另存 JSON |

## 接管、启用与释放

AI 执行会改变板卡的操作时使用自己的短时租约。租约范围为 30–1800 秒，接管期间浏览器与串口控制台的写操作受限，仍可读取状态。租约文件绑定 URL，换用 AP 或新 LAN 地址时应选用适配该 URL 的文件。CLI 使用 POSIX 文件标志验证 `0600` 租约，优先在 macOS、Linux 或具备相应文件语义的执行环境使用。

```sh
board_rpc_locked control acquire --ttl 300
# 保持租约期间，使用 board_rpc_locked 执行写入。按需续租。
board_rpc_locked control renew --ttl 300
# 完成写入、清理和状态读回后释放。
board_rpc_locked control release
```

在实际操作序列中，于第一次写操作前取得租约，到期前按需续租，结束或失败时释放自己持有且仍有效的租约，并读回状态。重启和到期会使旧令牌失效。遇到锁定或忙碌时确认是否已有操作运行，先检查状态再决定下一步。

`arm` 会启用矩阵。工具板 GPIO9 同时连接 CH446Q 时钟和 ESP32-C3 的 BOOT，使用矩阵前应已实际确认 DTR→BOOT 断开。CLI 的该选项同时代表已确认 BBX 空载。矩阵自检及板内夹具都需要 BBX 空载、路由清空；`board_rpc selftest` 会返回 `passed`、`failed` 和 `total`，正常总数为 448。以下两个命令示范已实际核对空载的板内自检场景。

```sh
board_rpc_locked arm --confirm-dtr-disconnected
board_rpc_locked selftest
```

工具板重启后矩阵初始为未启用且路由清空。已接线时，在实际确认工具板 DTR 已与 GPIO9 断开后，可在接管前从网页「设备管理 → 启用矩阵」启用，或在自己的租约内调用 `POST /api/matrix/arm`，附上已授权的设备密钥、租约和 `X-DTR-Disconnected: yes`。这个 RPC 仅启用矩阵。随后根据实际接线应用路由。`workbench reset` 打开交叉点并释放所有 Y 为输入。

直接调用 RPC 时，板卡认证使用 `X-Board-Key`，租约令牌通过 `X-Board-Lease` 提交。`GET /api/workbench` 读回状态，`POST /api/workbench` 用 `application/x-www-form-urlencoded` 提交 `op` 和参数。优先用 CLI 管理令牌和编码。

目标板的正常复位与烧录走目标板 USB 或预接的 EN、BOOT。若目标使用工具板的 OUT，`workbench power mode=gnd` 关闭 5 V 并经 330 Ω 放电，`mode=5v` 恢复供电。`out_mode` 表示 CTRL 读回，冷重启还要核对目标是否被 USB 或其他路径继续供电。

## 首次配网与串口回退

- 工具板软 AP 为 `AI-Board-xxxxxx`，在已连接该 AP 的本机打开 `http://192.168.4.1/setup`。使用已有设备密钥配对，填入可达的 2.4 GHz Wi‑Fi。保存后工具板重启，可访问该 AP 的 `/api/status` 或查看串口日志确认新 LAN 地址。
- 有已验证的局域网连接时，`board_rpc scan` 扫描可见 Wi‑Fi，`board_rpc_locked wifi "$WIFI_SSID" --password-file "$WIFI_PASSWORD_FILE"` 修改网络并重启。正在持有租约时用 `board_rpc_locked scan`。确认新网络可从执行端访问再修改设备地址。串口控制台同样支持 `status`、`scan` 和 `wifi`。
- 串口控制台使用 UART0 115200，可用 `python3 -m serial.tools.miniterm --dtr 0 --rts 0 "$BOARD_SERIAL_PORT" 115200` 打开。`wifi` 会交互询问 SSID 和密码，密码无回显。已完成 DTR 和空载核对时也可用 `arm dtr-disconnected` 与 `selftest`。控制台不是 Wi‑Fi RPC 的完整替代。
- Wi‑Fi 不可用时，在已配置路由且矩阵已启用的条件下，用本机 `pyserial` 读取 UART0。让 `BOARD_SERIAL_CAPTURE` 指向仓库的 `tools/serial_capture.py`，导入 ZIP 时用包内的 `scripts/serial_capture.py`。脚本支持长窗口 GPIO 采集，采完输出一份 JSON。A–D 由 Y 通道的参数顺序决定，示例中的 Y0、Y1 应替换为实际接线。

```sh
python3 "$BOARD_SERIAL_CAPTURE" "$BOARD_SERIAL_PORT" 0 1 \
  --trigger falling --trigger-channel A --wait-ms 3000 --ms 150 --output capture.json
```

## 更新工具固件和工作台

| 更新对象 | 操作与验收 |
| --- | --- |
| 工具板应用固件 | `board_rpc_locked ota "$AI_BOARD_APP_BIN"` 写下一 OTA 槽并安排重启。先记录当前槽位，重启后查 `status` 核对新槽位、`ota_pending=false` 与实际 ELF SHA‑256，再重新读回工作台状态 |
| 网页包 | 持租约运行 `board_rpc_locked web upload "$WEB_DIR"`。用 `board_rpc web status` 读回活动版本，按需刷新网页验证 |
| 网页恢复 | 打开工具板的 `/recovery` 使用固件内置工作台。可从本机重新上传已保存的旧版网页包 |
| 目标板固件 | 使用所选目标 SDK 和其 USB、下载接口或已验证的恢复流程 |

`AI_BOARD_APP_BIN` 是构建出的 ESP-IDF 应用 `ai_board.bin`。当前 4 MiB Flash 布局包含两个 1.75 MiB 应用 OTA 槽、384 KiB LittleFS 工作区及 NVS。调整分区表属于单独的串口烧录或迁移流程，先按源码仓库已有流程备份 NVS、确认所用镜像和设备。初次对工具板进行 USB 烧录时，有源码仓库与匹配接线可在 ESP-IDF 环境下进入编译输出 `build/`，运行 `python3 "$AI_BOARD_REPO/tools/serial_bootstrap.py" "$BOARD_SERIAL_PORT"`。该命令依赖对应的编译产物及已核对的 DTR、RTS 接线，启动输出含设备密钥，应私下处理。

需要自动验证同网段 OTA 时，让 `BOARD_LAN_SMOKE` 指向仓库的 `tools/lan_smoke.py`，导入 ZIP 时用包内的 `scripts/lan_smoke.py`。该脚本适用于当前无活动租约的板卡，可交互读取设备密钥，或以 `--key-stdin` 从本机私有文件读入。`BOARD_HOST_CIDR` 是实际源网卡的 IP 和网络前缀，可额外用 `--ssid "$WIFI_SSID"` 校验板卡当前连接的 Wi‑Fi。

```sh
python3 "$BOARD_LAN_SMOKE" "$BOARD_IP" "$AI_BOARD_APP_BIN" \
  --host-cidr "$BOARD_HOST_CIDR" --key-stdin < "$BOARD_KEY_FILE"
```

`WEB_DIR` 应含 `index.html`、`style.css` 和 `app.js`，HTML 中要各引用一次本地样式和脚本。CLI 为资源生成版本路径，gzip 打包后上限为 96 KiB。支持此功能的固件在校验后切换为新网页，网页与 Lua 文件占用同一个 LittleFS，0.10.9 起板上仅保留活动网页包。应用 OTA 负责工具驱动和 RPC 的变更，纯网页更新可走网页包入口。
