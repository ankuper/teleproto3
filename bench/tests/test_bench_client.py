"""
Tests for Story 1a-3: Python bench client.

Covers: type3_protocol importability, connect_type3() API, command_type=0x04
session header, three sub-modes (sink/echo/source), fixture SHA-256 recording,
endpoint sourcing from .credentials env vars, chunk-size and ack-mode tunables.
"""

from __future__ import annotations

import hashlib
import struct
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

# ---------------------------------------------------------------------------
# AC#1: type3_protocol.py exports connect_type3(), importable
# ---------------------------------------------------------------------------


def test_type3_protocol_importable():
    """AC#1: type3_protocol.py is importable and exposes connect_type3."""
    from teleproto3.bench.type3_protocol import connect_type3

    assert callable(connect_type3)


def test_build_session_header_importable():
    """AC#1: build_session_header is importable."""
    from teleproto3.bench.type3_protocol import build_session_header

    assert callable(build_session_header)


def test_parse_secret_hex_importable():
    """AC#1: parse_secret_hex is importable."""
    from teleproto3.bench.type3_protocol import parse_secret_hex

    assert callable(parse_secret_hex)


# ---------------------------------------------------------------------------
# AC#2: Client sends 0x04 0x01 0x00 0x00 header (command_type=BENCH)
# ---------------------------------------------------------------------------


def test_command_type_bench_header():
    """AC#2: BENCH session header is exactly 0x04 0x01 0x00 0x00."""
    from teleproto3.bench.type3_protocol import build_session_header

    T3_CMD_BENCH = 0x04
    VERSION = 0x01
    FLAGS = 0x0000

    header = build_session_header(command_type=T3_CMD_BENCH)

    assert header == bytes([0x04, 0x01, 0x00, 0x00])
    assert len(header) == 4
    cmd, ver, flags = struct.unpack("<BBH", header)
    assert cmd == T3_CMD_BENCH
    assert ver == VERSION
    assert flags == FLAGS


def test_command_type_passthrough_header():
    """Session header for MTPROTO_PASSTHROUGH is 0x01 0x01 0x00 0x00."""
    from teleproto3.bench.type3_protocol import build_session_header

    header = build_session_header(command_type=0x01)
    assert header == bytes([0x01, 0x01, 0x00, 0x00])


# ---------------------------------------------------------------------------
# AC#3: Three sub-modes — sink, echo, source
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_mode_sink_throughput():
    """AC#3 (sink): sink mode sends N bytes and computes positive throughput."""
    from teleproto3.bench.bench_client import mode_sink

    payload = b"\xab" * 65536
    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    result = await mode_sink(
        writer=mock_writer,
        payload=payload,
        chunk_size=65536,
    )

    assert "throughput_mbps" in result
    assert result["throughput_mbps"] > 0.0
    assert result["bytes_sent"] == 65536
    assert result["duration_ms"] > 0.0
    assert result["sha256_match"] == "na"


@pytest.mark.asyncio
async def test_mode_echo_sha256_match():
    """AC#3 (echo): echo mode verifies SHA-256 of returned data matches fixture."""
    from teleproto3.bench.bench_client import mode_echo

    payload = bytes(range(256)) * 4  # 1024 bytes
    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    mock_reader.readexactly = AsyncMock(return_value=payload)

    result = await mode_echo(
        reader=mock_reader,
        writer=mock_writer,
        payload=payload,
        ack_mode="streaming",
        chunk_size=65536,
    )

    assert result["sha256_match"] == "true"
    assert result["throughput_mbps"] > 0.0
    assert result["bytes_sent"] == 1024
    assert result["error_class"] == "ok"


@pytest.mark.asyncio
async def test_mode_echo_sha256_mismatch():
    """AC#3 (echo): SHA-256 mismatch sets error_class=corruption."""
    from teleproto3.bench.bench_client import mode_echo

    payload = b"\x01" * 1024
    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    mock_reader.readexactly = AsyncMock(return_value=b"\x02" * 1024)

    result = await mode_echo(
        reader=mock_reader,
        writer=mock_writer,
        payload=payload,
        ack_mode="streaming",
        chunk_size=65536,
    )

    assert result["sha256_match"] == "false"
    assert result["error_class"] == "corruption"


@pytest.mark.asyncio
async def test_mode_source_reads_n_bytes():
    """AC#3 (source): source mode sends 4-byte LE length N, reads exactly N bytes."""
    from teleproto3.bench.bench_client import mode_source

    n = 2048
    server_payload = b"\xcd" * n

    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    mock_reader.readexactly = AsyncMock(return_value=server_payload)

    result = await mode_source(
        reader=mock_reader,
        writer=mock_writer,
        size=n,
    )

    assert result["bytes_received"] == n
    assert result["throughput_mbps"] > 0.0
    assert result["error_class"] == "ok"
    expected_length_prefix = struct.pack("<I", n)
    mock_writer.write.assert_any_call(expected_length_prefix)


# ---------------------------------------------------------------------------
# AC#4: --fixture <path> reads binary, SHA-256 recorded
# ---------------------------------------------------------------------------


def test_fixture_sha256_recorded(tmp_path):
    """AC#4: fixture SHA-256 appears in the run result metadata."""
    from teleproto3.bench.bench_client import load_fixture

    fixture_path = tmp_path / "fixture-test.bin"
    fixture_data = b"\x00\x01\x02\x03" * 256  # 1024 bytes
    fixture_path.write_bytes(fixture_data)
    expected_sha256 = hashlib.sha256(fixture_data).hexdigest()

    loaded = load_fixture(str(fixture_path))

    assert loaded["sha256"] == expected_sha256
    assert loaded["data"] == fixture_data
    assert loaded["size_bytes"] == 1024


# ---------------------------------------------------------------------------
# AC#6: Endpoint configurable via .credentials env vars
# ---------------------------------------------------------------------------


def test_endpoint_from_bench_env():
    """AC#6: BENCH_* env vars take precedence over WS_*."""
    from teleproto3.bench.bench_client import resolve_endpoint

    env = {
        "BENCH_DOMAIN": "bench.example.com",
        "BENCH_PATH": "/ws/bench",
        "BENCH_SECRET": "aabbccdd00112233aabbccdd00112233",
        "WS_DOMAIN": "prod.example.com",
        "WS_PATH": "/ws/prod",
        "WS_SECRET": "11223344556677881122334455667788",
    }

    with patch.dict("os.environ", env, clear=False):
        endpoint = resolve_endpoint(server=None, path=None, secret_env=None)

    assert endpoint["server"] == "bench.example.com"
    assert endpoint["path"] == "/ws/bench"
    assert endpoint["secret"] == bytes.fromhex("aabbccdd00112233aabbccdd00112233")


def test_endpoint_fallback_to_ws_env():
    """AC#6: WS_* env vars are used as fallback."""
    from teleproto3.bench.bench_client import resolve_endpoint

    env_fallback = {
        "WS_DOMAIN": "prod.example.com",
        "WS_PATH": "/ws/prod",
        "WS_SECRET": "11223344556677881122334455667788",
    }

    with patch.dict("os.environ", env_fallback, clear=True):
        endpoint = resolve_endpoint(server=None, path=None, secret_env=None)

    assert endpoint["server"] == "prod.example.com"
    assert endpoint["path"] == "/ws/prod"


# ---------------------------------------------------------------------------
# AC#7: --chunk-size and --ack-mode tunables
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chunk_size_respected():
    """AC#7: chunk_size controls the maximum bytes per write call in send loop."""
    from teleproto3.bench.bench_client import mode_sink

    payload = b"\xef" * 4096
    chunk_size = 1024

    mock_writer = MagicMock()
    writes_captured = []
    mock_writer.write = MagicMock(side_effect=lambda data: writes_captured.append(data))
    mock_writer.drain = AsyncMock()

    await mode_sink(
        writer=mock_writer,
        payload=payload,
        chunk_size=chunk_size,
    )

    assert len(writes_captured) >= 4
    for chunk in writes_captured:
        assert len(chunk) <= chunk_size


@pytest.mark.asyncio
async def test_ack_mode_sequential():
    """AC#7: sequential ack_mode interleaves write/read per chunk (echo mode)."""
    from teleproto3.bench.bench_client import mode_echo

    payload = b"\xaa" * 2048
    chunk_size = 1024

    call_order = []

    mock_writer = MagicMock()
    mock_writer.write = MagicMock(
        side_effect=lambda data: call_order.append(("write", len(data)))
    )
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()

    async def fake_read(n):
        call_order.append(("read", n))
        return b"\xaa" * n

    mock_reader.readexactly = AsyncMock(side_effect=fake_read)

    await mode_echo(
        reader=mock_reader,
        writer=mock_writer,
        payload=payload,
        ack_mode="sequential",
        chunk_size=chunk_size,
    )

    write_indices = [i for i, (op, _) in enumerate(call_order) if op == "write"]
    read_indices = [i for i, (op, _) in enumerate(call_order) if op == "read"]

    assert len(write_indices) >= 2
    assert len(read_indices) >= 2
    for w_idx, r_idx in zip(write_indices, read_indices):
        assert r_idx > w_idx, (
            f"In sequential mode, read (index {r_idx}) must follow "
            f"its write (index {w_idx})"
        )
