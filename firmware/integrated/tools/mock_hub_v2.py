#!/usr/bin/env python3
"""Deterministic loopback-only mock for the LamaBuddy Hub v2 command contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
import uuid
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Mapping, Sequence
from urllib.parse import parse_qs, urlparse


DEFAULT_TTL_MS = 8_000
MAX_TTL_MS = 8_000
MAX_BODY_BYTES = 16_384
SCENARIO_DELAYS_MS = {
    "success": 300,
    "partialApply": 450,
    "unknownOutcome": 450,
    "expireBeforeStart": 9_000,
    "disconnect": 300,
}


@dataclass(frozen=True)
class MockCommand:
    request_id: str
    canonical_hash: str
    accepted_at_ms: int
    deadline_at_ms: int
    accepted_ttl_ms: int
    scenario: str
    terminal_at_ms: int


class MockHubV2Engine:
    """In-memory v2 journal with deterministic time supplied by the caller."""

    def __init__(self, *, scenario: str = "success") -> None:
        if scenario not in SCENARIO_DELAYS_MS:
            raise ValueError("unsupported_scenario")
        self.scenario = scenario
        self._commands: dict[str, MockCommand] = {}

    @staticmethod
    def _request_id(value: Any) -> str:
        if not isinstance(value, str):
            raise ValueError("invalidRequest")
        parsed = uuid.UUID(value)
        normalized = str(parsed)
        if value.lower() != normalized:
            raise ValueError("invalidRequest")
        return normalized

    @staticmethod
    def _accepted_ttl(payload: Mapping[str, Any]) -> int:
        value = payload.get("clientTtlMs", DEFAULT_TTL_MS)
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError("invalidTtl")
        if value < 1 or value > MAX_TTL_MS:
            raise ValueError("invalidTtl")
        return value

    @staticmethod
    def _canonical_hash(
        *,
        route: str,
        machine_serial: str,
        payload: Mapping[str, Any],
    ) -> str:
        action = {
            key: value
            for key, value in payload.items()
            if key not in {"requestId", "clientTtlMs", "clientSurface"}
        }
        canonical = json.dumps(
            {
                "route": route,
                "machineSerial": machine_serial,
                "action": action,
            },
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    def submit(
        self,
        *,
        route: str,
        machine_serial: str,
        payload: Mapping[str, Any],
        now_ms: int,
    ) -> tuple[int, dict[str, Any]]:
        request_id = self._request_id(payload.get("requestId"))
        ttl_ms = self._accepted_ttl(payload)
        canonical_hash = self._canonical_hash(
            route=route,
            machine_serial=machine_serial,
            payload=payload,
        )
        existing = self._commands.get(request_id)
        if existing is not None:
            if existing.canonical_hash != canonical_hash:
                return 409, {
                    "apiVersion": 2,
                    "ok": False,
                    "requestId": request_id,
                    "errorCode": "requestIdPayloadConflict",
                }
            return self.lookup(request_id=request_id, now_ms=now_ms)

        if self.scenario == "expireBeforeStart" and ttl_ms < DEFAULT_TTL_MS:
            return 503, {
                "apiVersion": 2,
                "ok": False,
                "requestId": request_id,
                "errorCode": "commandDeadlineCapacity",
                "upstreamAttempts": 0,
            }

        delay_ms = SCENARIO_DELAYS_MS[self.scenario]
        command = MockCommand(
            request_id=request_id,
            canonical_hash=canonical_hash,
            accepted_at_ms=now_ms,
            deadline_at_ms=now_ms + ttl_ms,
            accepted_ttl_ms=ttl_ms,
            scenario=self.scenario,
            terminal_at_ms=now_ms + delay_ms,
        )
        self._commands[request_id] = command
        return 202, {
            "apiVersion": 2,
            "ok": True,
            "requestId": request_id,
            "status": "accepted",
            "pending": True,
            "acceptedTtlMs": ttl_ms,
            "pollAfterMs": 150,
        }

    def lookup(self, *, request_id: str, now_ms: int) -> tuple[int, dict[str, Any]]:
        command = self._commands.get(request_id)
        if command is None:
            return 404, {
                "apiVersion": 2,
                "ok": False,
                "errorCode": "notFound",
            }
        remaining_ms = max(0, command.deadline_at_ms - now_ms)
        if now_ms < command.terminal_at_ms and remaining_ms > 0:
            return 202, {
                "apiVersion": 2,
                "ok": True,
                "requestId": request_id,
                "status": "running",
                "pending": True,
                "acceptedTtlMs": command.accepted_ttl_ms,
                "remainingMs": remaining_ms,
            }

        if command.scenario == "partialApply":
            return 200, {
                "apiVersion": 2,
                "ok": False,
                "requestId": request_id,
                "status": "partialApply",
                "pending": False,
                "errorCode": "dose_write_unconfirmed",
                "upstreamAttempts": 2,
            }
        if command.scenario in {"unknownOutcome", "disconnect"}:
            return 200, {
                "apiVersion": 2,
                "ok": False,
                "requestId": request_id,
                "status": "unknownOutcome",
                "pending": False,
                "errorCode": "upstream_outcome_unknown",
                "upstreamAttempts": 1,
            }
        if command.scenario == "expireBeforeStart":
            return 200, {
                "apiVersion": 2,
                "ok": False,
                "requestId": request_id,
                "status": "completed",
                "pending": False,
                "errorCode": "commandExpired",
                "upstreamAttempts": 0,
            }
        return 200, {
            "apiVersion": 2,
            "ok": True,
            "requestId": request_id,
            "status": "completed",
            "pending": False,
            "acceptedTtlMs": command.accepted_ttl_ms,
            "remainingMs": remaining_ms,
            "upstreamAttempts": 1,
        }


class _Handler(BaseHTTPRequestHandler):
    engine: MockHubV2Engine

    def log_message(self, format: str, *args: object) -> None:
        return

    def _send(self, status: int, payload: Mapping[str, Any]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        if self.headers.get("X-LamaBuddy-API-Version") != "2":
            self._send(400, {"ok": False, "errorCode": "invalidApiVersion"})
            return
        machine_serial = self.headers.get("X-LamaBuddy-Machine-Serial", "").strip()
        if not machine_serial:
            self._send(409, {"ok": False, "errorCode": "machineTargetMismatch"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY_BYTES:
            self._send(400, {"ok": False, "errorCode": "invalidRequest"})
            return
        try:
            payload = json.loads(self.rfile.read(length))
            if not isinstance(payload, Mapping):
                raise ValueError("invalidRequest")
            status, response = self.engine.submit(
                route=urlparse(self.path).path,
                machine_serial=machine_serial,
                payload=payload,
                now_ms=int(time.monotonic() * 1000),
            )
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._send(400, {"ok": False, "errorCode": "invalidRequest"})
            return
        except ValueError as exc:
            error_code = str(exc)
            if error_code not in {"invalidRequest", "invalidTtl"}:
                error_code = "invalidRequest"
            self._send(400, {"ok": False, "errorCode": error_code})
            return
        if self.engine.scenario == "disconnect":
            self.close_connection = True
            return
        self._send(status, response)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/diagnostics/commands":
            self._send(404, {"ok": False, "errorCode": "notFound"})
            return
        request_id = parse_qs(parsed.query).get("id", [""])[0]
        status, response = self.engine.lookup(
            request_id=request_id,
            now_ms=int(time.monotonic() * 1000),
        )
        self._send(status, response)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8789)
    parser.add_argument("--scenario", choices=sorted(SCENARIO_DELAYS_MS), default="success")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.port < 1 or args.port > 65535:
        raise SystemExit("invalid port")
    engine = MockHubV2Engine(scenario=args.scenario)
    handler = type("MockHubV2Handler", (_Handler,), {"engine": engine})
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"mock-hub-v2 listening on 127.0.0.1:{args.port} scenario={args.scenario}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
