#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import socket
import stat
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = ROOT / "build" / "lamabuddy_esp32_s3_hub.bin"
MAX_TOKEN_FILE_BYTES = 65_536
SAFE_BOOLEAN_RESPONSE_FIELDS = {
    "ok",
    "enabled",
    "restartRequired",
    "restartScheduled",
}
SAFE_INTEGER_RESPONSE_FIELDS = {"nextUpdateSize", "bytesWritten"}
SAFE_PARTITION_RESPONSE_FIELDS = {
    "runningPartition",
    "bootPartition",
    "nextUpdatePartition",
    "partition",
}
SAFE_PARTITION_VALUES = {"factory", "ota_0", "ota_1"}
SAFE_FIRMWARE_VERSION_PATTERN = re.compile(
    r"(?:[0-9]+(?:\.[0-9]+){2}(?:[-+][0-9A-Za-z.-]+)?|[0-9a-f]{7,12}(?:-dirty)?)\Z"
)


@dataclass
class Response:
    status: int
    body: Any
    elapsed_ms: int


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(
        self,
        req: Any,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> None:
        return None


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


def _is_allowed_ip_address(address: ipaddress.IPv4Address | ipaddress.IPv6Address) -> bool:
    if address.is_loopback:
        return True
    if isinstance(address, ipaddress.IPv4Address):
        return any(
            address in network
            for network in (
                ipaddress.ip_network("10.0.0.0/8"),
                ipaddress.ip_network("172.16.0.0/12"),
                ipaddress.ip_network("192.168.0.0/16"),
            )
        )
    return address in ipaddress.ip_network("fc00::/7")


def validate_base_url(raw_url: str) -> str:
    try:
        parsed = urlsplit(raw_url)
        port = parsed.port
    except ValueError as exc:
        raise RuntimeError("invalid_base_url") from exc

    if parsed.scheme not in {"http", "https"}:
        raise RuntimeError("invalid_base_url_scheme")
    if not parsed.hostname or parsed.username is not None or parsed.password is not None:
        raise RuntimeError("invalid_base_url_authority")
    if parsed.path not in {"", "/"} or parsed.query or parsed.fragment:
        raise RuntimeError("base_url_must_be_origin_only")
    if port is not None and not 1 <= port <= 65_535:
        raise RuntimeError("invalid_base_url_port")

    host = parsed.hostname.rstrip(".").lower()
    if host == "localhost":
        return raw_url.rstrip("/")
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        if not host.endswith(".local"):
            raise RuntimeError("ota_target_not_local") from None
        try:
            answers = socket.getaddrinfo(
                parsed.hostname,
                port or (443 if parsed.scheme == "https" else 80),
                type=socket.SOCK_STREAM,
            )
        except OSError as exc:
            raise RuntimeError("ota_target_resolution_failed") from exc
        if not answers:
            raise RuntimeError("ota_target_resolution_failed")
        for answer in answers:
            try:
                resolved_text = str(answer[4][0]).split("%", 1)[0]
                resolved_address = ipaddress.ip_address(resolved_text)
            except (IndexError, TypeError, ValueError) as exc:
                raise RuntimeError("ota_target_resolution_not_local") from exc
            if not _is_allowed_ip_address(resolved_address):
                raise RuntimeError("ota_target_resolution_not_local")
        return raw_url.rstrip("/")
    if not _is_allowed_ip_address(address):
        raise RuntimeError("ota_target_not_local")
    return raw_url.rstrip("/")


def read_token_physical_line(path: Path, line_number: int) -> str:
    if line_number < 1:
        raise RuntimeError("invalid_token_line")
    try:
        metadata = path.stat()
        if (
            path.is_symlink()
            or not path.is_file()
            or metadata.st_size > MAX_TOKEN_FILE_BYTES
            or stat.S_IMODE(metadata.st_mode) & 0o077
        ):
            raise RuntimeError("invalid_token_file")
        raw = path.read_bytes()
    except RuntimeError:
        raise
    except OSError as exc:
        raise RuntimeError("token_file_unreadable") from exc

    lines = raw.split(b"\n")
    if line_number > len(lines):
        raise RuntimeError("token_line_missing")
    selected = lines[line_number - 1]
    if selected.endswith(b"\r"):
        selected = selected[:-1]
    try:
        token = selected.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError("token_line_not_utf8") from exc
    return validate_token_value(token)


def validate_token_value(token: str) -> str:
    if not token or token != token.strip() or len(token) > 4_096:
        raise RuntimeError("invalid_token_value")
    if any(not 0x21 <= ord(character) <= 0x7E for character in token):
        raise RuntimeError("invalid_token_value")
    return token


def resolve_token(args: argparse.Namespace) -> str:
    if args.token and args.token_file is not None:
        raise RuntimeError("multiple_token_sources")
    if args.token_file is not None:
        if args.token_line is None:
            raise RuntimeError("token_line_required")
        return read_token_physical_line(args.token_file, args.token_line)
    if args.token_line is not None:
        raise RuntimeError("token_file_required")
    token = args.token or os.environ.get("LAMABUDDY_HUB_TOKEN", "")
    if not token:
        raise RuntimeError("missing_token")
    return validate_token_value(token)


def decode_json(raw: str) -> Any:
    try:
        return json.loads(raw or "{}")
    except json.JSONDecodeError:
        return {"raw": raw}


def request(base_url: str, method: str, path: str, token: str, payload: bytes | None, headers: dict[str, str] | None, timeout: float) -> Response:
    request_headers = {"Accept": "application/json"}
    if token:
        request_headers["Authorization"] = f"Bearer {token}"
    if headers:
        request_headers.update(headers)

    started = time.perf_counter()
    req = urllib.request.Request(f"{base_url.rstrip('/')}{path}", data=payload, method=method, headers=request_headers)
    try:
        with OPENER.open(req, timeout=timeout) as res:
            raw = res.read().decode("utf-8")
            return Response(res.status, decode_json(raw), int((time.perf_counter() - started) * 1000))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8")
        return Response(exc.code, decode_json(raw), int((time.perf_counter() - started) * 1000))
    except (OSError, TimeoutError, urllib.error.URLError) as exc:
        raise RuntimeError("network_request_failed") from None


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def print_response(label: str, response: Response) -> None:
    safe_body: dict[str, bool | int | str] = {}
    if isinstance(response.body, dict):
        for key, value in response.body.items():
            if key in SAFE_BOOLEAN_RESPONSE_FIELDS and isinstance(value, bool):
                safe_body[key] = value
            elif (
                key in SAFE_INTEGER_RESPONSE_FIELDS
                and isinstance(value, int)
                and not isinstance(value, bool)
                and 0 <= value <= 16_777_216
            ):
                safe_body[key] = value
            elif (
                key in SAFE_PARTITION_RESPONSE_FIELDS
                and isinstance(value, str)
                and value in SAFE_PARTITION_VALUES
            ):
                safe_body[key] = value
            elif (
                key == "firmwareVersion"
                and isinstance(value, str)
                and SAFE_FIRMWARE_VERSION_PATTERN.fullmatch(value) is not None
            ):
                safe_body[key] = value
    print(
        f"{label}: http={response.status} elapsedMs={response.elapsed_ms} "
        f"body={json.dumps(safe_body, ensure_ascii=False, sort_keys=True)}"
    )


def wait_for_health(base_url: str, timeout_seconds: int) -> int:
    started = time.perf_counter()
    last_error = ""
    while time.perf_counter() - started < timeout_seconds:
        try:
            response = request(base_url, "GET", "/health", "", None, None, timeout=5)
            if response.status == 200 and isinstance(response.body, dict) and response.body.get("status") == "ok":
                return int((time.perf_counter() - started) * 1000)
            last_error = f"http_{response.status}"
        except Exception:  # noqa: BLE001 - update helper should keep polling until timeout.
            last_error = "network_request_failed"
        time.sleep(1)
    raise RuntimeError(f"Hub did not recover within {timeout_seconds}s: {last_error}")


def run(args: argparse.Namespace) -> int:
    if args.preflight_only and (args.restart or args.wait_health):
        raise RuntimeError("preflight_only_conflicts_with_restart_or_wait")
    base_url = validate_base_url(args.base_url)
    token = resolve_token(args)

    image_path = Path(args.image).expanduser().resolve()
    require(image_path.exists(), f"image not found: {image_path}")
    image = image_path.read_bytes()
    require(len(image) > 0, f"image is empty: {image_path}")

    status = request(base_url, "GET", "/diagnostics/ota", token, None, None, timeout=args.timeout)
    print_response("ota-status-before", status)
    require(status.status == 200 and isinstance(status.body, dict) and status.body.get("ok") is True, "OTA status check failed")

    if args.preflight_only:
        print("OTA preflight completed; no upload or restart performed")
        return 0

    print(f"uploading: image={image_path} bytes={len(image)}")
    update = request(
        base_url,
        "POST",
        "/diagnostics/ota/update",
        token,
        image,
        {
            "Content-Type": "application/octet-stream",
            "X-LamaBuddy-OTA-Confirm": "true",
        },
        timeout=args.upload_timeout,
    )
    print_response("ota-update", update)
    require(update.status == 200 and isinstance(update.body, dict) and update.body.get("ok") is True, "OTA update failed")

    if args.restart:
        restart = request(base_url, "POST", "/diagnostics/restart", token, b"{}", {"Content-Type": "application/json"}, timeout=args.timeout)
        print_response("restart", restart)
        require(restart.status == 200 and isinstance(restart.body, dict) and restart.body.get("restartScheduled") is True, "restart scheduling failed")
        if args.restart_settle > 0:
            time.sleep(args.restart_settle)

    if args.wait_health:
        recovery_ms = wait_for_health(base_url, args.wait_timeout)
        print(f"health-recovered-ms={recovery_ms}")
        after = request(base_url, "GET", "/diagnostics/ota", token, None, None, timeout=args.timeout)
        print_response("ota-status-after", after)

    print("Local OTA upload completed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload a local ESP32-S3 Hub firmware image through the protected Wi-Fi OTA route.")
    parser.add_argument("--base-url", default=os.environ.get("LAMABUDDY_ESP_HUB_BASE_URL", "http://lamabuddy-hub.local:8787"))
    parser.add_argument("--token")
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--token-line", type=int)
    parser.add_argument("--image", default=str(DEFAULT_IMAGE))
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--restart", action="store_true")
    parser.add_argument("--wait-health", action="store_true")
    parser.add_argument("--restart-settle", type=float, default=3.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--upload-timeout", type=float, default=120.0)
    parser.add_argument("--wait-timeout", type=int, default=60)
    args = parser.parse_args()
    try:
        return run(args)
    except Exception as exc:  # noqa: BLE001 - CLI helper should print the failure reason.
        print(f"Local OTA upload failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
