#!/usr/bin/env python3
"""Run AI Board commands or read results directly over HTTP RPC."""

import argparse
import functools
import getpass
import gzip
import hashlib
from html import escape
from html.parser import HTMLParser
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import stat
import struct
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request


MAX_LUA_SOURCE_BYTES = 8 * 1024
LUA_SOURCE_HEADERS = {"Content-Type": "text/plain; charset=utf-8"}
MAX_WEB_BUNDLE_BYTES = 96 * 1024
WEB_ASSETS = ("index.html", "style.css", "app.js")
WEB_BUNDLE_HEADER = struct.Struct("<8s16sIII32s")
WEB_BUNDLE_HEADERS = {"Content-Type": "application/octet-stream"}


class RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class SourceHTTPHandler(urllib.request.HTTPHandler):
    def __init__(self, source_ip):
        super().__init__()
        self.connection = functools.partial(http.client.HTTPConnection, source_address=(str(source_ip), 0))

    def http_open(self, request):
        return self.do_open(self.connection, request)


class SourceHTTPSHandler(urllib.request.HTTPSHandler):
    def __init__(self, source_ip):
        super().__init__()
        self.connection = functools.partial(http.client.HTTPSConnection, source_address=(str(source_ip), 0))

    def https_open(self, request):
        return self.do_open(self.connection, request, context=self._context)


class Board:
    def __init__(self, url, key, source_ip=None):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username is not None:
            raise ValueError("Provide an HTTP or HTTPS board URL without credentials")
        if parsed.query or parsed.fragment:
            raise ValueError("Board URL must have no query or fragment")
        self.url = url.rstrip("/")
        self.key = key
        handlers = [urllib.request.ProxyHandler({}), RejectRedirects()]
        if source_ip is not None:
            if parsed.scheme == "http":
                handlers.append(SourceHTTPHandler(source_ip))
            else:
                handlers.append(SourceHTTPSHandler(source_ip))
        self.opener = urllib.request.build_opener(*handlers)

    def request(self, route, body=None, *, headers=None, token=None):
        request_headers = dict(headers or {})
        if self.key is not None:
            request_headers["X-Board-Key"] = self.key
        if token is not None:
            request_headers["X-Board-Lease"] = token
        request = urllib.request.Request(self.url + route, data=body, headers=request_headers)
        with self.opener.open(request, timeout=60) as response:
            return json.load(response)

    def form(self, route, fields, *, token=None):
        body = urllib.parse.urlencode(fields).encode()
        return self.request(route, body, headers={"Content-Type": "application/x-www-form-urlencoded"}, token=token)


def valid_token(token):
    return isinstance(token, str) and bool(token) and token.isascii() and token.isprintable() and " " not in token


def lease_token(path, board_url):
    # A lease can authorize hardware changes, so only accept a private
    # regular file. Open without following a symlink.
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, encoding="utf-8") as file:
        mode = os.fstat(file.fileno()).st_mode
        if not stat.S_ISREG(mode) or mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise ValueError("Lease file must be a private regular file (mode 0600)")
        saved = json.load(file)
    if not isinstance(saved, dict) or saved.get("board_url") != board_url:
        raise ValueError("Lease file belongs to a different board URL")
    token = saved.get("lease_token")
    if not valid_token(token):
        raise ValueError("Lease file has an invalid token")
    return token


def acquire_lease(board, path, ttl):
    # Require the parent directory to be writable before asking the board
    # to enter exclusive control. NamedTemporaryFile creates mode 0600.
    replace_existing = True
    try:
        lease_token(path, board.url)
    except FileNotFoundError:
        replace_existing = False
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as file:
        temporary = Path(file.name)
        token = None
        try:
            result = board.form("/api/control", {"action": "acquire", "ttl_s": ttl})
            token = result.pop("lease_token", None) if isinstance(result, dict) else None
            if not valid_token(token):
                raise ValueError("Board did not return a valid lease token")
            json.dump({"board_url": board.url, "lease_token": token}, file)
            file.flush()
            os.fsync(file.fileno())
            if replace_existing:
                os.replace(temporary, path)
            else:
                os.link(temporary, path)
        except (OSError, ValueError):
            if valid_token(token):
                try:
                    board.form("/api/control", {"action": "release"}, token=token)
                except (OSError, ValueError):
                    pass
            raise
        finally:
            temporary.unlink(missing_ok=True)
    return result


def control(board, args):
    if args.action == "status":
        return board.request("/api/control")
    if args.lease_file is None:
        raise ValueError("Provide --lease-file for control acquire, renew or release")
    if args.action == "acquire":
        return acquire_lease(board, args.lease_file, args.ttl)
    token = lease_token(args.lease_file, board.url)
    fields = {"action": args.action}
    if args.action == "renew":
        fields["ttl_s"] = args.ttl
    result = board.form("/api/control", fields, token=token)
    if not isinstance(result, dict):
        raise ValueError("Board returned an invalid control response")
    result.pop("lease_token", None)
    return result


def workbench_fields(operation, parameters):
    fields = {"op": operation}
    for parameter in parameters:
        name, equals, value = parameter.partition("=")
        if not name or not equals or name in fields:
            raise ValueError(f"Expected a unique workbench KEY=VALUE argument, got {parameter!r}")
        fields[name] = value
    return fields


def lua_source(script):
    if script == "-":
        source = sys.stdin.buffer.read(MAX_LUA_SOURCE_BYTES + 1)
    else:
        with open(script, "rb") as file:
            source = file.read(MAX_LUA_SOURCE_BYTES + 1)
    if not source or len(source) > MAX_LUA_SOURCE_BYTES:
        raise ValueError("Lua source must contain 1–8192 bytes")
    try:
        source.decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("Lua source must be UTF-8 text") from None
    return source


def run_lua(board, route, params, body, token):
    result = board.request(route + "?" + urllib.parse.urlencode(params), body,
                           headers=LUA_SOURCE_HEADERS if body else None, token=token)
    if not isinstance(result, dict) or not isinstance(result.get("ok"), bool):
        raise ValueError("Board returned an invalid Lua response")
    return result


def web_index(source, version):
    line_starts = [0, *(match.end() for match in re.finditer("\n", source))]
    replacements = []
    counts = dict.fromkeys(WEB_ASSETS[1:], 0)

    class AssetLinks(HTMLParser):
        def handle_starttag(self, tag, attributes):
            if tag == "link" and any(
                name == "rel" and "stylesheet" in (value or "").lower().split()
                for name, value in attributes
            ):
                attribute, filename = "href", "style.css"
            elif tag == "script":
                attribute, filename = "src", "app.js"
            else:
                return

            urls = [value for name, value in attributes if name == attribute]
            local_urls = {filename, "/" + filename, "./" + filename}
            if not any(url in local_urls for url in urls):
                return
            if len(urls) != 1:
                raise ValueError(f"index.html has ambiguous {attribute} attributes for {filename}")

            path = f"/_web/{version}/{filename}"
            fields = [name if value is None else f'{name}="{escape(path if name == attribute else value, quote=True)}"'
                      for name, value in attributes]
            original = self.get_starttag_text()
            replacement = "<" + " ".join([tag, *fields]) + (" />" if original.endswith("/>") else ">")
            line, column = self.getpos()
            offset = line_starts[line - 1] + column
            replacements.append((offset, offset + len(original), replacement))
            counts[filename] += 1

    links = AssetLinks()
    links.feed(source)
    links.close()
    for filename, count in counts.items():
        if count != 1:
            raise ValueError(f"index.html must reference {filename} exactly once with a local asset link")
    for start, end, replacement in reversed(replacements):
        source = source[:start] + replacement + source[end:]
    return source.encode("utf-8")


def web_bundle(directory):
    version = secrets.token_hex(8)
    index = directory / WEB_ASSETS[0]
    try:
        source = index.read_bytes().decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("index.html must be UTF-8 text") from None
    assets = [web_index(source, version)]
    assets.extend((directory / filename).read_bytes() for filename in WEB_ASSETS[1:])
    compressed = [gzip.compress(asset, compresslevel=9, mtime=0) for asset in assets]
    if WEB_BUNDLE_HEADER.size + sum(map(len, compressed)) > MAX_WEB_BUNDLE_BYTES:
        raise ValueError("Compressed web bundle exceeds 96 KiB")
    payload = b"".join(compressed)
    header = WEB_BUNDLE_HEADER.pack(b"AIWEB001", version.encode("ascii"), *map(len, compressed),
                                    hashlib.sha256(payload).digest())
    return header + payload


def execute(board, args):
    if args.command == "control":
        return control(board, args)
    reads = {
        "status": "/api/status",
        "diagnostics": "/api/diagnostics",
        "result": "/api/workbench/result",
    }
    if args.command in reads:
        return board.request(reads[args.command])
    if args.command == "logs":
        params = {"limit": args.limit}
        if args.after is not None:
            params["after"] = args.after
        return board.request("/api/logs?" + urllib.parse.urlencode(params))
    if args.command == "workbench" and args.operation is None:
        return board.request("/api/workbench")
    if args.command == "script" and args.action == "list":
        return board.request("/api/scripts")
    if args.command == "web" and args.action == "status":
        return board.request("/api/web")

    token = lease_token(args.lease_file, board.url) if args.lease_file is not None else None
    if args.command == "web":
        return board.request("/api/web", web_bundle(args.directory), headers=WEB_BUNDLE_HEADERS, token=token)
    if args.command == "script":
        params = {"name": args.name}
        if args.action == "upload":
            return board.request("/api/scripts?" + urllib.parse.urlencode(params), lua_source(args.script),
                                 headers=LUA_SOURCE_HEADERS, token=token)
        params["timeout_ms"] = args.timeout_ms
        return run_lua(board, "/api/scripts/run", params, b"", token)
    if args.command == "lua":
        return run_lua(board, "/api/lua", {"timeout_ms": args.timeout_ms}, lua_source(args.script), token)
    if args.command == "workbench":
        return board.form("/api/workbench", workbench_fields(args.operation, args.parameters), token=token)
    if args.command == "arm":
        if not args.confirm_dtr_disconnected and input(
            "DTR disconnected and BBX pins unloaded? Type yes: "
        ) != "yes":
            raise ValueError("Matrix was not armed.")
        return board.request("/api/matrix/arm", b"", headers={"X-DTR-Disconnected": "yes"}, token=token)
    if args.command == "wifi":
        password = (args.password_file.read_text(encoding="utf-8").rstrip("\r\n") if args.password_file
                    else getpass.getpass("Wi-Fi password (blank for open networks): "))
        return board.form("/api/wifi", {"ssid": args.ssid, "password": password}, token=token)
    if args.command == "ota":
        return board.request("/api/ota", args.binary.read_bytes(),
                             headers={"Content-Type": "application/octet-stream"}, token=token)
    if args.command == "scan":
        return board.request("/api/wifi/scan", b"", token=token)
    return board.request("/api/matrix/selftest", b"", token=token)


def parse_ttl(value):
    seconds = int(value)
    if not 30 <= seconds <= 1800:
        raise argparse.ArgumentTypeError("Lease TTL must be 30–1800 seconds")
    return seconds


def parse_lua_timeout(value):
    milliseconds = int(value)
    if not 100 <= milliseconds <= 10000:
        raise argparse.ArgumentTypeError("Lua timeout must be 100–10000 milliseconds")
    return milliseconds


def parse_log_limit(value):
    count = int(value)
    if not 1 <= count <= 64:
        raise argparse.ArgumentTypeError("Log limit must be 1–64")
    return count


def parse_log_cursor(value):
    sequence = int(value)
    if not 0 <= sequence <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("Log cursor must be 0–4294967295")
    return sequence


def parse_script_name(name):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,43}\.lua", name):
        raise argparse.ArgumentTypeError("Script name must be 5–48 ASCII characters, start with a letter or digit, "
                                         "and end in .lua; only letters, digits, dots, underscores and hyphens are allowed")
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.4.1")
    parser.add_argument("--source-ip", type=ipaddress.ip_address, help="Use this host interface address for HTTP")
    credentials = parser.add_mutually_exclusive_group()
    credentials.add_argument("--key-file", type=Path, help="Read the board access key from a file")
    credentials.add_argument("--key-stdin", action="store_true", help="Read the board access key from stdin")
    parser.add_argument("--lease-file", type=Path, help="Store or use an exclusive control lease in this file")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("status", help="Read board and network status")
    commands.add_parser("diagnostics", help="Read uptime, reset reason, heap, and Wi-Fi link data")
    logs = commands.add_parser("logs", help="Read recent board events over Wi-Fi")
    logs.add_argument("--limit", type=parse_log_limit, default=16,
                      help="Maximum number of events to return (1–64, default: 16)")
    logs.add_argument("--after", type=parse_log_cursor, metavar="SEQ",
                      help="Return events newer than this sequence number in ascending order")
    commands.add_parser("scan", help="List visible 2.4 GHz networks")
    arm = commands.add_parser("arm", help="Arm the CH446Q matrix")
    arm.add_argument("--confirm-dtr-disconnected", action="store_true",
                     help="Confirm that DTR is physically disconnected and all BBX pins are unloaded")
    commands.add_parser("selftest", help="Exercise the matrix with all BBX pins unloaded")
    wifi = commands.add_parser("wifi", help="Configure the board's Wi-Fi")
    wifi.add_argument("ssid")
    wifi.add_argument("--password-file", type=Path, help="Read the Wi-Fi password from a file")
    ota = commands.add_parser("ota", help="Install an ESP-IDF application image")
    ota.add_argument("binary", type=Path)
    workbench = commands.add_parser("workbench", help="Read routing and pin modes, or run a workbench operation")
    workbench.add_argument("operation", nargs="?", metavar="OP")
    workbench.add_argument("parameters", nargs="*", metavar="KEY=VALUE")
    lua = commands.add_parser("lua", help="Run UTF-8 Lua source on the board while holding its control lease")
    lua.add_argument("script", metavar="FILE|-", help="Read a Lua file, or use - to read stdin")
    lua.add_argument("--timeout-ms", type=parse_lua_timeout, default=5000,
                     help="Script time budget in milliseconds (default: 5000)")
    scripts = commands.add_parser("script", help="Upload, list or run Lua scripts stored on the board")
    script_actions = scripts.add_subparsers(dest="action", required=True)
    script_actions.add_parser("list", help="List stored Lua scripts")
    script_upload = script_actions.add_parser("upload", help="Upload UTF-8 Lua source while holding the control lease")
    script_upload.add_argument("name", type=parse_script_name, metavar="NAME")
    script_upload.add_argument("script", metavar="FILE|-", help="Read a Lua file, or use - to read stdin")
    script_run = script_actions.add_parser("run", help="Run a stored Lua script while holding the control lease")
    script_run.add_argument("name", type=parse_script_name, metavar="NAME")
    script_run.add_argument("--timeout-ms", type=parse_lua_timeout, default=5000,
                            help="Script time budget in milliseconds (default: 5000)")
    web = commands.add_parser("web", help="Manage workbench web assets stored on the board")
    web_actions = web.add_subparsers(dest="action", required=True)
    web_actions.add_parser("status", help="Read the active web version")
    web_upload = web_actions.add_parser("upload", help="Replace the active workbench web bundle")
    web_upload.add_argument("directory", metavar="DIRECTORY", type=Path,
                            help="Directory containing index.html, style.css and app.js")
    commands.add_parser("result", help="Read the latest workbench operation and result")
    control_parser = commands.add_parser("control", help="Manage exclusive board control")
    actions = control_parser.add_subparsers(dest="action", required=True)
    actions.add_parser("status", help="Read current control status")
    actions.add_parser("release", help="Release your current exclusive control lease")
    for action in ("acquire", "renew"):
        command = actions.add_parser(action)
        command.add_argument("--ttl", type=parse_ttl, default=300, help="Lease duration in seconds (default: 300)")

    args = parser.parse_args()
    uploads_script = args.command == "script" and args.action == "upload"
    runs_lua = args.command == "lua" or (args.command == "script" and args.action == "run")
    if args.command == "web" and args.action == "upload" and args.lease_file is None:
        parser.error("web upload requires --lease-file")
    if runs_lua or uploads_script:
        operation = "lua" if args.command == "lua" else f"script {args.action}"
        if args.lease_file is None:
            parser.error(f"{operation} requires --lease-file")
        if (args.command == "lua" or uploads_script) and args.script == "-" and args.key_stdin:
            parser.error(f"{operation} - and --key-stdin both require stdin; use --key-file")
    try:
        key = None
        public = args.command == "status" or (args.command == "control" and args.action == "status")
        if not public:
            if args.key_file:
                key = args.key_file.read_text(encoding="utf-8").strip()
            elif args.key_stdin:
                key = sys.stdin.readline().strip()
            else:
                key = getpass.getpass("Board access key: ")
            if not key:
                raise ValueError("Board access key is empty")
        result = execute(Board(args.url, key, args.source_ip), args)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if runs_lua and not result["ok"]:
            parser.exit(1)
    except urllib.error.HTTPError as error:
        parser.exit(1, f"HTTP {error.code}: {error.read().decode(errors='replace')}\n")
    except urllib.error.URLError as error:
        parser.exit(1, f"Board unavailable: {error.reason}\n")
    except (EOFError, OSError, ValueError) as error:
        parser.exit(1, f"Board command failed: {error}\n")


if __name__ == "__main__":
    main()
