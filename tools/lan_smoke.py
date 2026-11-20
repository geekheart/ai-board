#!/usr/bin/env python3
"""Push an application over the host LAN and verify the resulting boot slot."""

import argparse
import functools
import getpass
import http.client
import ipaddress
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


class SourceHTTPHandler(urllib.request.HTTPHandler):
    def __init__(self, source_ip):
        super().__init__()
        self.connection = functools.partial(http.client.HTTPConnection, source_address=(source_ip, 0))

    def http_open(self, request):
        return self.do_open(self.connection, request)


def request(opener, url, headers=None, body=None, timeout=5):
    req = urllib.request.Request(url, data=body, headers=headers or {})
    with opener.open(req, timeout=timeout) as response:
        return json.load(response)


def verify_station(status, board_ip, host_network, ssid):
    if status.get("project") != "ai_board" or status.get("station_ip") != str(board_ip):
        raise ValueError(f"{board_ip} did not identify itself as the AI Board station interface")
    if ssid and status.get("station_ssid") != ssid:
        raise ValueError(f"Board Wi-Fi is {status.get('station_ssid')!r}; expected {ssid!r}")
    netmask = status.get("station_netmask")
    if netmask:
        board_network = ipaddress.IPv4Interface(f"{board_ip}/{netmask}").network
        if board_network != host_network:
            raise ValueError(f"Board subnet {board_network} differs from host subnet {host_network}")


def elf_sha256(image):
    # A generated ESP-IDF app has its descriptor just after the image and first segment headers.
    descriptor = image[32:208]
    if (len(descriptor) != 176 or image[0] != 0xE9 or
            descriptor[:4] != bytes.fromhex("3254cdab") or
            descriptor[48:80].split(b"\0", 1)[0] != b"ai_board"):
        raise ValueError("Expected the ai_board ESP-IDF application image")
    return descriptor[144:176].hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board_ip", type=ipaddress.IPv4Address)
    parser.add_argument("image", type=Path, help="ESP-IDF application binary")
    parser.add_argument("--host-cidr", required=True, type=ipaddress.IPv4Interface,
                        help="Current host Wi-Fi address and prefix, e.g. 192.168.1.10/24")
    parser.add_argument("--ssid", help="Also confirm the board joined this exact Wi-Fi SSID")
    parser.add_argument("--key-stdin", action="store_true", help="Read the board key from stdin")
    parser.add_argument("--boot-timeout", type=float, default=60, help="Seconds to wait for the new app")
    args = parser.parse_args()
    if args.board_ip not in args.host_cidr.network or args.board_ip == args.host_cidr.ip:
        parser.error("Board and host must have distinct addresses on the same subnet")
    if args.boot_timeout <= 0:
        parser.error("--boot-timeout must be positive")

    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), SourceHTTPHandler(str(args.host_cidr.ip))
    )
    base = f"http://{args.board_ip}"
    try:
        image = args.image.read_bytes()
        expected_sha256 = elf_sha256(image)
        before = request(opener, base + "/api/status")
        verify_station(before, args.board_ip, args.host_cidr.network, args.ssid)
        previous = before["partition"]
        print(f"Before OTA: {args.board_ip} on {args.host_cidr.network}, slot {previous}.", flush=True)
        key = sys.stdin.readline().strip() if args.key_stdin else getpass.getpass("Board access key: ")
        if not key:
            raise ValueError("Board access key is empty")
        accepted = request(
            opener, base + "/api/ota",
            headers={"X-Board-Key": key, "Content-Type": "application/octet-stream"},
            body=image, timeout=90,
        )
        if accepted.get("accepted") is not True:
            raise ValueError(f"Board did not accept OTA: {accepted}")
        print(f"Board accepted {len(image)} bytes. Waiting for a different, confirmed slot.", flush=True)
        deadline = time.monotonic() + args.boot_timeout
        while time.monotonic() < deadline:
            time.sleep(1)
            try:
                after = request(opener, base + "/api/status", timeout=2)
            except (OSError, ValueError):
                continue
            verify_station(after, args.board_ip, args.host_cidr.network, args.ssid)
            current = after["partition"]
            if current.startswith("ota_") and current != previous and not after.get("ota_pending"):
                if after.get("app_sha256") != expected_sha256:
                    raise ValueError("Running ELF SHA-256 does not match the uploaded image")
                print(f"Verified OTA: {previous} → {current} on {args.board_ip}, ELF {expected_sha256[:16]}.")
                return
        raise ValueError(f"Timed out waiting for a confirmed OTA slot different from {previous}")
    except urllib.error.HTTPError as error:
        parser.exit(1, f"HTTP {error.code}: {error.read().decode(errors='replace')}\n")
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"LAN OTA verification failed: {error}\n")


if __name__ == "__main__":
    main()
