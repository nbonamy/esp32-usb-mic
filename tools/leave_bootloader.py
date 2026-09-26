#!/usr/bin/env python3
"""Reset an ESP32-S3 ROM serial port back into the USB microphone app."""

import argparse
import subprocess
import sys
import time

import esptool
import serial
from serial.tools import list_ports


USB_VID = 0x303A
ROM_PID = 0x1001
MIC_PID = 0x8000


def matching_port(pid: int, requested: str | None = None) -> str | None:
    for port in list_ports.comports():
        if port.vid == USB_VID and port.pid == pid:
            if requested is None or port.device == requested:
                return port.device
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="ROM serial port; auto-detected by VID:PID")
    args = parser.parse_args()
    rom_port = matching_port(ROM_PID, args.port)
    if not rom_port:
        raise SystemExit("ESP32-S3 ROM serial port not found")

    # Our CDC recovery command sets RTC_CNTL_FORCE_DOWNLOAD_BOOT. It survives
    # USB Serial/JTAG resets, so clear that one bit before asking ROM to boot.
    is_esptool_5 = int(esptool.__version__.split(".")[0]) >= 5
    option = "no-reset" if is_esptool_5 else "no_reset"
    action = "write-mem" if is_esptool_5 else "write_mem"
    clear_force_download = [
        sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", rom_port,
        "--before", option, "--no-stub", "--after", option, action,
        "0x6000812C", "0", "1",
    ]
    subprocess.run(clear_force_download, check=True, capture_output=True, text=True)

    # Closing esptool's port may itself reset the USB Serial/JTAG controller.
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        mic_port = matching_port(MIC_PID)
        if mic_port:
            print(f"USB microphone and CDC port: {mic_port}")
            return
        time.sleep(0.2)

    rom_port = matching_port(ROM_PID)
    if not rom_port:
        raise SystemExit("ROM port disappeared, but the USB microphone did not reappear")

    device = serial.Serial()
    device.port = rom_port
    device.baudrate = 115200
    device.timeout = 0.2
    # Set the line state before opening: DTR asserted during reset holds GPIO0
    # low and sends the board straight back into ROM download mode.
    device.dtr = False
    device.rts = False
    try:
        device.open()
        time.sleep(0.1)
        device.rts = True
        time.sleep(0.2)
        device.rts = False
    except (OSError, serial.SerialException):
        # The ROM port can disappear as soon as reset switches to USB audio.
        pass
    finally:
        try:
            device.close()
        except (OSError, serial.SerialException):
            pass

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        mic_port = matching_port(MIC_PID)
        if mic_port:
            print(f"USB microphone and CDC port: {mic_port}")
            return
        time.sleep(0.2)
    raise SystemExit("Reset sent, but the USB microphone did not reappear")


if __name__ == "__main__":
    main()
