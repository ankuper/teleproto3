#!/usr/bin/env python3
"""
Type3 bench client — throughput measurement primitive for Epic 1a.

Connects to a bench-enabled server (command_type=0x04), drives sink/echo/source
sub-modes, measures throughput, and emits CSV rows.

Usage:
    python3 bench_client.py --mode echo --fixture fixture-1mb.bin
    python3 bench_client.py --mode sink --fixture fixture-10mb.bin --runs 3
    python3 bench_client.py --mode source --size 1048576
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import hashlib
import io
import os
import struct
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

# Allow running as script or as module
_BENCH_DIR = Path(__file__).resolve().parent
_TELEPROTO3_DIR = _BENCH_DIR.parent.parent
if str(_TELEPROTO3_DIR) not in sys.path:
    sys.path.insert(0, str(_TELEPROTO3_DIR))

from teleproto3.bench.type3_protocol import (
    T3_CMD_BENCH,
    Type3Connection,
    connect_type3,
    parse_secret_hex,
)

BENCH_SUB_MODE_SINK = 0x01
BENCH_SUB_MODE_ECHO = 0x02
BENCH_SUB_MODE_SOURCE = 0x03

CSV_COLUMNS = [
    "ts_iso",
    "mode",
    "size_bytes",
    "run_index",
    "ttfb_ms",
    "duration_ms",
    "throughput_mbps",
    "sha256_match",
    "error_class",
]

DEFAULT_CHUNK_SIZE = 65536
DEFAULT_OUTPUT = str(_BENCH_DIR / "results" / "runs.csv")


def load_fixture(path: str) -> dict:
    """Load a binary fixture file. Returns {data, size_bytes, sha256}."""
    data = Path(path).read_bytes()
    return {
        "data": data,
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def resolve_endpoint(
    server: Optional[str] = None,
    path: Optional[str] = None,
    secret_env: Optional[str] = None,
) -> dict:
    """Resolve bench endpoint from CLI flags, BENCH_* env, or WS_* env fallback."""
    if server and path and secret_env:
        secret_hex = os.environ.get(secret_env, "")
        if not secret_hex:
            raise ValueError(f"env var {secret_env} not set")
        return {
            "server": server,
            "path": path,
            "secret": bytes.fromhex(secret_hex),
        }

    bench_domain = os.environ.get("BENCH_DOMAIN", "")
    bench_path = os.environ.get("BENCH_PATH", "")
    bench_secret = os.environ.get("BENCH_SECRET", "")

    if bench_domain and bench_secret:
        return {
            "server": server or bench_domain,
            "path": path or bench_path or "/",
            "secret": bytes.fromhex(bench_secret),
        }

    ws_domain = os.environ.get("WS_DOMAIN", "")
    ws_path = os.environ.get("WS_PATH", "")
    ws_secret = os.environ.get("WS_SECRET", "")

    if ws_domain and ws_secret:
        return {
            "server": server or ws_domain,
            "path": path or ws_path or "/",
            "secret": bytes.fromhex(ws_secret),
        }

    raise ValueError(
        "no endpoint configured: set --server/--path/--secret-env, "
        "BENCH_* env vars, or WS_* env vars"
    )


@dataclass
class RunResult:
    ts_iso: str
    mode: str
    size_bytes: int
    run_index: int
    ttfb_ms: float
    duration_ms: float
    throughput_mbps: float
    sha256_match: str
    error_class: str

    def as_csv_row(self) -> dict:
        return {
            "ts_iso": self.ts_iso,
            "mode": self.mode,
            "size_bytes": str(self.size_bytes),
            "run_index": str(self.run_index),
            "ttfb_ms": f"{self.ttfb_ms:.2f}",
            "duration_ms": f"{self.duration_ms:.2f}",
            "throughput_mbps": f"{self.throughput_mbps:.2f}",
            "sha256_match": self.sha256_match,
            "error_class": self.error_class,
        }


class CsvEmitter:
    """Append-only CSV writer with atomic flush per row."""

    def __init__(self, output: io.TextIOBase | str = DEFAULT_OUTPUT):
        if isinstance(output, str):
            path = Path(output)
            path.parent.mkdir(parents=True, exist_ok=True)
            self._needs_header = not path.exists() or path.stat().st_size == 0
            self._file = open(path, "a", newline="")
            self._owns_file = True
        else:
            self._file = output
            self._needs_header = True
            self._owns_file = False
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_COLUMNS)

    def write_header(self):
        self._writer.writeheader()
        self._file.flush()

    def write_row(self, result: RunResult):
        if self._needs_header:
            self.write_header()
            self._needs_header = False
        self._writer.writerow(result.as_csv_row())
        self._file.flush()

    def close(self):
        if self._owns_file:
            self._file.close()


async def mode_sink(
    writer: object,
    payload: bytes,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> dict:
    """Sink mode: send N bytes, no response expected.

    Args:
        writer: object with .write(data) and async .drain() methods
        payload: bytes to send
        chunk_size: max bytes per write call

    Returns dict with throughput metrics.
    """
    t_start = time.monotonic_ns()
    t_first_byte = None
    total_sent = 0

    offset = 0
    while offset < len(payload):
        chunk = payload[offset : offset + chunk_size]
        writer.write(chunk)
        await writer.drain()
        if t_first_byte is None:
            t_first_byte = time.monotonic_ns()
        total_sent += len(chunk)
        offset += len(chunk)

    t_end = time.monotonic_ns()
    if t_first_byte is None:
        t_first_byte = t_start

    duration_ms = (t_end - t_start) / 1_000_000
    ttfb_ms = (t_first_byte - t_start) / 1_000_000
    throughput_mbps = (total_sent * 8) / (duration_ms * 1000) if duration_ms > 0 else 0

    return {
        "bytes_sent": total_sent,
        "ttfb_ms": ttfb_ms,
        "duration_ms": duration_ms,
        "throughput_mbps": throughput_mbps,
        "sha256_match": "na",
        "error_class": "ok",
    }


async def mode_echo(
    reader: object,
    writer: object,
    payload: bytes,
    ack_mode: str = "streaming",
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> dict:
    """Echo mode: send N bytes, read N bytes back, verify SHA-256.

    Args:
        reader: object with async .readexactly(n) method
        writer: object with .write(data) and async .drain() methods
        payload: bytes to send
        ack_mode: "streaming" or "sequential"
        chunk_size: max bytes per write call

    Returns dict with throughput metrics and SHA-256 match status.
    """
    fixture_sha256 = hashlib.sha256(payload).hexdigest()
    t_start = time.monotonic_ns()
    t_first_byte = None
    total_sent = 0
    received_chunks = []

    if ack_mode == "sequential":
        offset = 0
        while offset < len(payload):
            chunk = payload[offset : offset + chunk_size]
            writer.write(chunk)
            await writer.drain()
            total_sent += len(chunk)

            echo_data = await reader.readexactly(len(chunk))
            if t_first_byte is None:
                t_first_byte = time.monotonic_ns()
            received_chunks.append(echo_data)
            offset += len(chunk)
    else:
        # Streaming: send all, then read all
        offset = 0
        while offset < len(payload):
            chunk = payload[offset : offset + chunk_size]
            writer.write(chunk)
            await writer.drain()
            total_sent += len(chunk)
            offset += len(chunk)

        bytes_remaining = len(payload)
        while bytes_remaining > 0:
            to_read = min(bytes_remaining, chunk_size)
            echo_data = await reader.readexactly(to_read)
            if t_first_byte is None:
                t_first_byte = time.monotonic_ns()
            received_chunks.append(echo_data)
            bytes_remaining -= len(echo_data)

    t_end = time.monotonic_ns()
    if t_first_byte is None:
        t_first_byte = t_start

    received = b"".join(received_chunks)
    received_sha256 = hashlib.sha256(received).hexdigest()
    sha256_match = "true" if received_sha256 == fixture_sha256 else "false"

    duration_ms = (t_end - t_start) / 1_000_000
    ttfb_ms = (t_first_byte - t_start) / 1_000_000
    total_bytes_on_wire = 2 * len(payload)
    throughput_mbps = (
        (total_bytes_on_wire * 8) / (duration_ms * 1000) if duration_ms > 0 else 0
    )

    error_class = "ok" if sha256_match == "true" else "corruption"

    return {
        "bytes_sent": total_sent,
        "bytes_received": len(received),
        "ttfb_ms": ttfb_ms,
        "duration_ms": duration_ms,
        "throughput_mbps": throughput_mbps,
        "sha256_match": sha256_match,
        "error_class": error_class,
    }


async def mode_source(
    reader: object,
    writer: object,
    size: int,
) -> dict:
    """Source mode: send 4-byte LE length N, read N bytes from server.

    Args:
        reader: object with async .readexactly(n) method
        writer: object with .write(data) and async .drain() methods
        size: number of bytes to request from server

    Returns dict with throughput metrics.
    """
    t_start = time.monotonic_ns()
    t_first_byte = None

    length_prefix = struct.pack("<I", size)
    writer.write(length_prefix)
    await writer.drain()

    received = await reader.readexactly(size)
    t_first_byte = time.monotonic_ns()

    t_end = time.monotonic_ns()

    duration_ms = (t_end - t_start) / 1_000_000
    ttfb_ms = (t_first_byte - t_start) / 1_000_000
    throughput_mbps = (size * 8) / (duration_ms * 1000) if duration_ms > 0 else 0

    return {
        "bytes_received": len(received),
        "ttfb_ms": ttfb_ms,
        "duration_ms": duration_ms,
        "throughput_mbps": throughput_mbps,
        "sha256_match": "na",
        "error_class": "ok",
    }


async def run_bench(
    endpoint: dict,
    mode: str,
    fixture_data: Optional[bytes],
    fixture_sha256: Optional[str],
    size_bytes: int,
    run_index: int,
    chunk_size: int,
    ack_mode: str,
    port: int = 443,
    tls: bool = True,
    timeout: float = 30.0,
) -> RunResult:
    """Execute a single bench run against the server."""
    ts_iso = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

    try:
        conn, session = await asyncio.wait_for(
            connect_type3(
                server=endpoint["server"],
                port=port,
                path=endpoint["path"],
                secret=endpoint["secret"],
                command_type=T3_CMD_BENCH,
                tls=tls,
            ),
            timeout=timeout,
        )
    except asyncio.TimeoutError:
        return RunResult(
            ts_iso=ts_iso,
            mode=mode,
            size_bytes=size_bytes,
            run_index=run_index,
            ttfb_ms=0.0,
            duration_ms=0.0,
            throughput_mbps=0.0,
            sha256_match="false",
            error_class="timeout",
        )
    except Exception as e:
        return RunResult(
            ts_iso=ts_iso,
            mode=mode,
            size_bytes=size_bytes,
            run_index=run_index,
            ttfb_ms=0.0,
            duration_ms=0.0,
            throughput_mbps=0.0,
            sha256_match="false",
            error_class="handshake_fail",
        )

    try:
        sub_mode_byte = {
            "sink": BENCH_SUB_MODE_SINK,
            "echo": BENCH_SUB_MODE_ECHO,
            "source": BENCH_SUB_MODE_SOURCE,
        }[mode]

        await conn.send(bytes([sub_mode_byte]))
        adapter = ConnAdapter(conn)

        if mode == "sink":
            result = await asyncio.wait_for(
                mode_sink(writer=adapter, payload=fixture_data, chunk_size=chunk_size),
                timeout=timeout,
            )
        elif mode == "echo":
            result = await asyncio.wait_for(
                mode_echo(
                    reader=adapter,
                    writer=adapter,
                    payload=fixture_data,
                    ack_mode=ack_mode,
                    chunk_size=chunk_size,
                ),
                timeout=timeout,
            )
        elif mode == "source":
            result = await asyncio.wait_for(
                mode_source(reader=adapter, writer=adapter, size=size_bytes),
                timeout=timeout,
            )
        else:
            raise ValueError(f"unknown mode: {mode}")

        return RunResult(
            ts_iso=ts_iso,
            mode=mode,
            size_bytes=size_bytes,
            run_index=run_index,
            ttfb_ms=result["ttfb_ms"],
            duration_ms=result["duration_ms"],
            throughput_mbps=result["throughput_mbps"],
            sha256_match=result.get("sha256_match", "na"),
            error_class=result.get("error_class", "ok"),
        )
    except asyncio.TimeoutError:
        return RunResult(
            ts_iso=ts_iso,
            mode=mode,
            size_bytes=size_bytes,
            run_index=run_index,
            ttfb_ms=0.0,
            duration_ms=0.0,
            throughput_mbps=0.0,
            sha256_match="false",
            error_class="timeout",
        )
    except ConnectionError:
        return RunResult(
            ts_iso=ts_iso,
            mode=mode,
            size_bytes=size_bytes,
            run_index=run_index,
            ttfb_ms=0.0,
            duration_ms=0.0,
            throughput_mbps=0.0,
            sha256_match="false",
            error_class="connection_reset",
        )
    finally:
        await conn.close()


class ConnAdapter:
    """Adapts Type3Connection to the write/drain/readexactly interface used by mode functions."""

    def __init__(self, conn: Type3Connection):
        self._conn = conn
        self._pending_writes = bytearray()

    def write(self, data: bytes):
        self._pending_writes.extend(data)

    async def drain(self):
        if self._pending_writes:
            await self._conn.send(bytes(self._pending_writes))
            self._pending_writes.clear()

    async def readexactly(self, n: int) -> bytes:
        return await self._conn.recv(n)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Type3 bench client — throughput measurement"
    )
    parser.add_argument(
        "--mode",
        choices=["sink", "echo", "source"],
        required=True,
        help="bench sub-mode",
    )
    parser.add_argument("--fixture", help="path to binary fixture file")
    parser.add_argument(
        "--size",
        type=int,
        default=1048576,
        help="byte count for source mode (default 1MB)",
    )
    parser.add_argument(
        "--runs", type=int, default=1, help="number of runs (default 1)"
    )
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="CSV output path")
    parser.add_argument("--server", help="server hostname")
    parser.add_argument("--port", type=int, default=443, help="server port")
    parser.add_argument("--path", help="WebSocket path")
    parser.add_argument("--secret-env", help="env var name for secret hex")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="max bytes per WS frame (default 65536)",
    )
    parser.add_argument(
        "--ack-mode",
        choices=["streaming", "sequential"],
        default="streaming",
        help="echo mode: streaming (default) or sequential chunk-and-wait",
    )
    parser.add_argument(
        "--no-tls", action="store_true", help="disable TLS (for local testing)"
    )
    parser.add_argument(
        "--timeout", type=float, default=30.0, help="per-run timeout in seconds"
    )
    return parser


async def main_async(args: argparse.Namespace) -> int:
    endpoint = resolve_endpoint(
        server=args.server, path=args.path, secret_env=args.secret_env
    )

    fixture_data = None
    fixture_sha256 = None
    size_bytes = args.size

    if args.mode in ("sink", "echo"):
        if not args.fixture:
            print("error: --fixture required for sink/echo mode", file=sys.stderr)
            return 1
        loaded = load_fixture(args.fixture)
        fixture_data = loaded["data"]
        fixture_sha256 = loaded["sha256"]
        size_bytes = loaded["size_bytes"]

    emitter = CsvEmitter(args.output)

    try:
        for run_idx in range(args.runs):
            result = await run_bench(
                endpoint=endpoint,
                mode=args.mode,
                fixture_data=fixture_data,
                fixture_sha256=fixture_sha256,
                size_bytes=size_bytes,
                run_index=run_idx,
                chunk_size=args.chunk_size,
                ack_mode=args.ack_mode,
                port=args.port,
                tls=not args.no_tls,
                timeout=args.timeout,
            )
            emitter.write_row(result)
            status = "OK" if result.error_class == "ok" else result.error_class
            print(
                f"run {run_idx}: {result.mode} {result.size_bytes}B "
                f"{result.throughput_mbps:.2f} Mbps [{status}]"
            )
    finally:
        emitter.close()

    return 0


def main():
    parser = build_parser()
    args = parser.parse_args()
    sys.exit(asyncio.run(main_async(args)))


if __name__ == "__main__":
    main()
