import sys
import time

import serial
from esp_pylib.serial_reset import set_dtr_rts
from esptool import cmds
from esptool.targets.esp32c3 import ESP32C3ROM

port = serial.Serial(baudrate=115200, timeout=0.5)
port.port = sys.argv[1]
port.dtr = port.rts = False
with port:
    set_dtr_rts(port, True, False)
    time.sleep(0.1)
    set_dtr_rts(port, True, True)
    time.sleep(0.15)
    port.reset_input_buffer()
    set_dtr_rts(port, True, False)
    print(port.read(192).decode(errors="replace"), flush=True)
    chip = ESP32C3ROM(port)
    chip.connect(mode="no-reset", attempts=3, warnings=False)
    chip = chip.run_stub()
    chip.change_baud(460800)
    cmds.write_flash(
        chip,
        [(0, "bootloader/bootloader.bin"),
         (0x8000, "partition_table/partition-table.bin"),
         (0xD000, "ota_data_initial.bin"),
         (0x10000, "ai_board.bin")],
        flash_mode="dio", flash_freq="80m", flash_size="detect",
    )
    port.baudrate = 115200
    set_dtr_rts(port, False, False)
    time.sleep(0.1)
    set_dtr_rts(port, False, True)
    time.sleep(0.15)
    port.reset_input_buffer()
    set_dtr_rts(port, False, False)
    port.timeout = 8
    result = port.read_until(b"AP password and X-Board-Key: ", 16384) + port.readline()
    print(result.decode(errors="replace")[-1200:])
    sys.exit(0 if b"AP password and X-Board-Key:" in result else 2)
