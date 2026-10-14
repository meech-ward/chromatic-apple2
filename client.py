#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Control the local Chromatic without printing its private build credentials."""

import argparse
from http.client import HTTPException
import json
import math
from pathlib import Path
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        return None


def disk_name(value):
    if not 5 <= len(value) <= 63 or not value[0].isascii() or not value[0].isalnum():
        raise argparse.ArgumentTypeError("disk name must be a safe 5–63 character basename ending in .dsk")
    if not value.isascii() or any(not (c.isalnum() or c in " _-.") for c in value) or ".." in value:
        raise argparse.ArgumentTypeError("disk name cannot contain paths or unsafe characters")
    if not value.lower().endswith(".dsk"):
        raise argparse.ArgumentTypeError("disk name must end in .dsk")
    return value


def sensitive_key(key):
    return any(part in str(key).lower() for part in ("token", "ssid", "password", "secret"))


def private_strings(value):
    result = []
    if isinstance(value, dict):
        for key, item in value.items():
            if sensitive_key(key) and isinstance(item, str) and item:
                result.append(item)
            result.extend(private_strings(item))
    elif isinstance(value, list):
        for item in value:
            result.extend(private_strings(item))
    return result


def public_fields(value):
    if isinstance(value, dict):
        return {key: public_fields(item) for key, item in value.items() if not sensitive_key(key)}
    if isinstance(value, list):
        return [public_fields(item) for item in value]
    return value


def redacted(text, secrets):
    try:
        value = json.loads(text)
        if isinstance(value, (dict, list)):
            text = json.dumps(public_fields(value), indent=2)
    except ValueError:
        pass
    for secret in sorted(set(secrets), key=len, reverse=True):
        text = text.replace(secret, "[redacted]")
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="device IP address or http://address")
    parser.add_argument("--config", type=Path,
                        default=Path(__file__).resolve().parent / "build/device-local.json")
    parser.add_argument("--timeout", type=float, help="seconds (default: 15; upload: 900)")
    parser.add_argument("--name", type=disk_name, default="Workshop.dsk", help="uploaded disk basename")
    parser.add_argument("action", choices=("status", "text", "command", "upload"))
    parser.add_argument("command", nargs="?")
    args = parser.parse_args(argv)
    if (args.action in ("command", "upload")) != (args.command is not None):
        parser.error("command requires quoted text; upload requires a disk file; status/text take no argument")
    if args.action == "command" and len(args.command.encode()) > 255:
        parser.error("command exceeds the device's 255-byte limit")
    timeout = args.timeout if args.timeout is not None else (900 if args.action == "upload" else 15)
    if not math.isfinite(timeout) or timeout <= 0:
        parser.error("timeout must be a positive finite number")
    base = args.host if "://" in args.host else "http://" + args.host
    url = urlsplit(base)
    if url.scheme != "http" or not url.hostname or url.username or url.password:
        parser.error("host must be an HTTP device address without credentials")
    if url.path not in ("", "/") or url.query or url.fragment:
        parser.error("host must not contain a path, query, or fragment")
    try:
        configuration = json.loads(args.config.read_text())
        if not isinstance(configuration, dict):
            raise ValueError("configuration must be an object")
        token = configuration["api_token"]
        if not isinstance(token, str) or not token or "\r" in token or "\n" in token:
            raise ValueError("invalid token")
    except (OSError, ValueError, KeyError, TypeError):
        parser.exit(2, "Cannot read the private device token from the config file.\n")
    secrets = private_strings(configuration)
    data = args.command.encode() if args.action == "command" else None
    path = "/" + args.action
    content_type = "text/plain"
    if args.action == "upload":
        try:
            source = Path(args.command)
            if source.stat().st_size != 143360:
                parser.exit(2, "Upload source must contain exactly 143360 bytes (140 KiB).\n")
            data = source.read_bytes()
            if len(data) != 143360:
                parser.exit(2, "Upload source changed while being read.\n")
        except OSError:
            parser.exit(2, "Cannot read the disk image file.\n")
        path = "/disk?" + urlencode({"name": args.name})
        content_type = "application/octet-stream"
    request = Request(base.rstrip("/") + path, data=data,
                      headers={"X-Device-Token": token, "Content-Type": content_type})
    opener = build_opener(ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=timeout) as response:
            body = response.read(65537)
            if len(body) > 65536:
                message = "Device response exceeded the expected size."
                if args.action == "upload": message += " Upload outcome is unconfirmed; no automatic retry was made."
                parser.exit(1, message + "\n")
            output = body.decode("utf-8", errors="replace")
            if args.action == "status":
                output = json.dumps(public_fields(json.loads(output)), indent=2)
            output = redacted(output, secrets)
            print(output, end="" if output.endswith("\n") else "\n")
    except HTTPError as error:
        try:
            detail = error.read(2048).decode("utf-8", errors="replace")
        except (HTTPException, OSError):
            detail = "response body unavailable"
        parser.exit(1, redacted(f"Device returned HTTP {error.code}: {detail}\n", secrets))
    except (URLError, TimeoutError, OSError, HTTPException, ValueError) as error:
        if args.action == "upload":
            parser.exit(1, "Upload outcome is unconfirmed after a connection/response failure. Check the device before retrying; no automatic retry was made.\n")
        parser.exit(1, redacted(f"Device request failed: {error}\n", secrets))


if __name__ == "__main__":
    main()
