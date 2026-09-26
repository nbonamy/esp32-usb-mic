#!/usr/bin/env python3
"""Ask the USB microphone's CDC interface to enter ESP32-S3 ROM download mode."""

import argparse
import time

import serial
from serial.tools import list_ports


USB_VID = 0x303A
MIC_PID = 0x8000
ROM_PID = 0x1001


def matching_port(pid: int, requested: str | None = None) -> str | None:
    for port in list_ports.comports():
        if port.vid == USB_VID and port.pid == pid:
            if requested is None or port.device == requested:
                return port.device
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="microphone CDC port; auto-detected by VID:PID")
    args = parser.parse_args()

    mic_port = matching_port(MIC_PID, args.port)
    if not mic_port:
        raise SystemExit("Microphone CDC port not found; use BOOT/PWR for the existing audio-only image")

    with serial.Serial(mic_port, 115200, timeout=2) as device:
        device.write(b"MIC BOOTLOADER\n")
        device.flush()
        reply = device.readline().decode("ascii", errors="replace").strip()
    if reply != "Entering ROM download mode":
        raise SystemExit(f"Unexpected device response: {reply!r}")

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        rom_port = matching_port(ROM_PID)
        if rom_port:
            print(f"ROM download port: {rom_port}")
            return
        time.sleep(0.2)
    raise SystemExit("The device acknowledged reboot, but the ROM serial port did not appear")


if __name__ == "__main__":
    main()
