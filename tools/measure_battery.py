#!/usr/bin/env python3
"""Log the board's PMU voltage while it runs unplugged and idle."""

import argparse
import socket
import time


QUERY = b"WMIC_POWER_V1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="board IPv4 address")
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--interval", type=int, default=15)
    args = parser.parse_args()
    if args.seconds < 0 or args.interval < 1:
        parser.error("seconds must be nonnegative and interval must be positive")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(1)
        start = time.monotonic()
        print("elapsed_s,voltage_mv,charge_percent,vbus")
        sample = 0
        while True:
            elapsed = time.monotonic() - start
            if elapsed > args.seconds + 0.5:
                break
            for attempt in range(3):
                sock.sendto(QUERY, (args.host, 24242))
                try:
                    data, _ = sock.recvfrom(128)
                except socket.timeout:
                    continue
                fields = data.decode("ascii", errors="replace").split()
                if len(fields) == 4 and fields[0] == QUERY.decode():
                    print(f"{elapsed:.1f},{fields[1]},{fields[2]},{fields[3]}",
                          flush=True)
                    break
            else:
                print(f"{elapsed:.1f},,,", flush=True)
            sample += 1
            next_elapsed = sample * args.interval
            if next_elapsed > args.seconds:
                break
            delay = start + next_elapsed - time.monotonic()
            if delay > 0:
                time.sleep(delay)


if __name__ == "__main__":
    main()
