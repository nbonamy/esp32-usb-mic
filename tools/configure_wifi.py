#!/usr/bin/env python3
"""Provision the microphone's Wi-Fi credentials over its USB CDC control port."""

import argparse
import getpass

import serial

from enter_bootloader import MIC_PID, matching_port


def request(command: bytes, port: str) -> str:
    with serial.Serial(port, 115200, timeout=3) as device:
        device.reset_input_buffer()
        device.write(command + b"\n")
        device.flush()
        return device.readline().decode("ascii", errors="replace").strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="microphone CDC port; detected automatically")
    parser.add_argument("--ssid", help="Wi-Fi network name; prompted if omitted")
    parser.add_argument("--clear", action="store_true", help="erase Wi-Fi credentials")
    parser.add_argument("--status", action="store_true", help="show configured state")
    args = parser.parse_args()
    port = matching_port(MIC_PID, args.port)
    if not port:
        raise SystemExit("Microphone CDC port not found; connect the board by USB")

    if args.status:
        print(request(b"MIC WIFI STATUS", port))
        return
    if args.clear:
        print(request(b"MIC WIFI CLEAR", port))
        return

    ssid = args.ssid if args.ssid is not None else input("Wi-Fi network name: ")
    password = getpass.getpass("Wi-Fi password (empty for open network): ")
    ssid_bytes = ssid.encode("utf-8")
    password_bytes = password.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= 32:
        raise SystemExit("Wi-Fi network name must contain 1–32 UTF-8 bytes")
    if password_bytes and not 8 <= len(password_bytes) <= 63:
        raise SystemExit("Wi-Fi password must contain 8–63 UTF-8 bytes")
    command = (b"MIC WIFI SET " + ssid_bytes.hex().encode("ascii") + b" " +
               password_bytes.hex().encode("ascii"))
    reply = request(command, port)
    print(reply)
    if reply != "Wi-Fi saved; restarting":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
