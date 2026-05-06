"""
RED-phase tests for Story 1a-3: Python bench client.

Covers: type3_protocol importability, connect_type3() API, command_type=0x04
session header, three sub-modes (sink/echo/source), fixture SHA-256 recording,
endpoint sourcing from .credentials env vars, chunk-size and ack-mode tunables.

All tests are skip-decorated until the implementation modules exist.
"""

from __future__ import annotations

import hashlib
import struct
import time
from unittest.mock import AsyncMock, MagicMock, patch

import pytest


# ---------------------------------------------------------------------------
# AC#1: type3_protocol.py exports connect_type3(), importable
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: type3_protocol module not yet implemented")
def test_type3_protocol_importable():
    """AC#1: type3_protocol.py is importable and exposes connect_type3."""
    # Arrange / Act
    from teleproto3.bench.type3_protocol import connect_type3

    # Assert — import itself is the assertion; additionally check callable
    assert callable(connect_type3)


@pytest.mark.skip(reason="RED PHASE: type3_protocol module not yet implemented")
@pytest.mark.asyncio
async def test_connect_type3_returns_triple():
    """AC#1: connect_type3() returns (reader, writer, session) triple."""
    from teleproto3.bench.type3_protocol import connect_type3

    # Arrange — mock the network layer so no real connection is needed
    with patch("teleproto3.bench.type3_protocol.asyncio.open_connection") as mock_conn:
        mock_reader = AsyncMock()
        mock_writer = MagicMock()
        mock_conn.return_value = (mock_reader, mock_writer)

        # Act
        result = await connect_type3(
            server="127.0.0.1",
            port=3129,
            path="/ws/test",
            secret=bytes.fromhex("78ef151a20066770db00a2f905c103e9"),
            command_type=0x04,
        )

    # Assert
    assert isinstance(result, tuple)
    assert len(result) == 3
    reader, writer, session = result
    assert reader is not None
    assert writer is not None
    assert session is not None


# ---------------------------------------------------------------------------
# AC#2: Client sends 0x04 0x01 0x00 0x00 header (command_type=BENCH)
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
def test_command_type_bench_header():
    """AC#2: BENCH session header is exactly 0x04 0x01 0x00 0x00."""
    from teleproto3.bench.type3_protocol import build_session_header

    # Arrange
    T3_CMD_BENCH = 0x04
    VERSION = 0x01
    FLAGS = 0x0000

    # Act
    header = build_session_header(command_type=T3_CMD_BENCH)

    # Assert — 4-byte header: cmd, version, flags_lo, flags_hi
    assert header == bytes([0x04, 0x01, 0x00, 0x00])
    assert len(header) == 4
    # Verify struct interpretation matches
    cmd, ver, flags = struct.unpack("<BBH", header)
    assert cmd == T3_CMD_BENCH
    assert ver == VERSION
    assert flags == FLAGS


# ---------------------------------------------------------------------------
# AC#3: Three sub-modes — sink, echo, source
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
@pytest.mark.asyncio
async def test_mode_sink_throughput():
    """AC#3 (sink): sink mode sends N bytes and computes positive throughput."""
    from teleproto3.bench.bench_client import mode_sink

    # Arrange — 64 KB fixture payload
    payload = b"\xab" * 65536
    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    # Act
    result = await mode_sink(
        writer=mock_writer,
        payload=payload,
        chunk_size=65536,
    )

    # Assert — result contains throughput_mbps and it's positive
    assert "throughput_mbps" in result
    assert result["throughput_mbps"] > 0.0
    assert result["bytes_sent"] == 65536
    assert result["duration_ms"] > 0.0
    # sink mode has no return data — sha256_match is n/a
    assert result["sha256_match"] == "na"


@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
@pytest.mark.asyncio
async def test_mode_echo_sha256_match():
    """AC#3 (echo): echo mode verifies SHA-256 of returned data matches fixture."""
    from teleproto3.bench.bench_client import mode_echo

    # Arrange — 1 KB fixture
    payload = bytes(range(256)) * 4  # 1024 bytes, deterministic
    expected_sha256 = hashlib.sha256(payload).hexdigest()

    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    # Server echoes the exact same bytes back
    mock_reader.readexactly = AsyncMock(return_value=payload)

    # Act
    result = await mode_echo(
        reader=mock_reader,
        writer=mock_writer,
        payload=payload,
        ack_mode="streaming",
        chunk_size=65536,
    )

    # Assert — SHA-256 of received data matches fixture
    assert result["sha256_match"] == "true"
    assert result["throughput_mbps"] > 0.0
    assert result["bytes_sent"] == 1024
    assert result["error_class"] == "ok"


@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
@pytest.mark.asyncio
async def test_mode_source_reads_n_bytes():
    """AC#3 (source): source mode sends 4-byte LE length N, reads exactly N bytes."""
    from teleproto3.bench.bench_client import mode_source

    # Arrange — request 2048 bytes from server
    n = 2048
    server_payload = b"\xcd" * n

    mock_writer = MagicMock()
    mock_writer.write = MagicMock()
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    mock_reader.readexactly = AsyncMock(return_value=server_payload)

    # Act
    result = await mode_source(
        reader=mock_reader,
        writer=mock_writer,
        size=n,
    )

    # Assert — client received exactly N bytes
    assert result["bytes_received"] == n
    assert result["throughput_mbps"] > 0.0
    assert result["error_class"] == "ok"
    # Verify the 4-byte LE length was written
    expected_length_prefix = struct.pack("<I", n)
    mock_writer.write.assert_any_call(expected_length_prefix)


# ---------------------------------------------------------------------------
# AC#4: --fixture <path> reads binary, SHA-256 recorded
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
def test_fixture_sha256_recorded(tmp_path):
    """AC#4: fixture SHA-256 appears in the run result metadata."""
    from teleproto3.bench.bench_client import load_fixture

    # Arrange — write a known binary fixture
    fixture_path = tmp_path / "fixture-test.bin"
    fixture_data = b"\x00\x01\x02\x03" * 256  # 1024 bytes
    fixture_path.write_bytes(fixture_data)
    expected_sha256 = hashlib.sha256(fixture_data).hexdigest()

    # Act
    loaded = load_fixture(str(fixture_path))

    # Assert — SHA-256 is recorded and matches
    assert loaded["sha256"] == expected_sha256
    assert loaded["data"] == fixture_data
    assert loaded["size_bytes"] == 1024


# ---------------------------------------------------------------------------
# AC#6: Endpoint configurable via .credentials env vars
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
def test_endpoint_from_credentials():
    """AC#6: endpoint defaults sourced from BENCH_* env vars, falling back to WS_*."""
    from teleproto3.bench.bench_client import resolve_endpoint

    # Arrange — set BENCH_* env vars (primary) and WS_* (fallback)
    env = {
        "BENCH_DOMAIN": "bench.example.com",
        "BENCH_PATH": "/ws/bench",
        "BENCH_SECRET": "aabbccdd00112233aabbccdd00112233",
        "WS_DOMAIN": "prod.example.com",
        "WS_PATH": "/ws/prod",
        "WS_SECRET": "11223344556677881122334455667788",
    }

    with patch.dict("os.environ", env, clear=False):
        # Act
        endpoint = resolve_endpoint(server=None, path=None, secret_env=None)

    # Assert — BENCH_* takes precedence over WS_*
    assert endpoint["server"] == "bench.example.com"
    assert endpoint["path"] == "/ws/bench"
    assert endpoint["secret"] == bytes.fromhex("aabbccdd00112233aabbccdd00112233")

    # Arrange — only WS_* available (fallback)
    env_fallback = {
        "WS_DOMAIN": "prod.example.com",
        "WS_PATH": "/ws/prod",
        "WS_SECRET": "11223344556677881122334455667788",
    }

    with patch.dict("os.environ", env_fallback, clear=True):
        # Act
        endpoint_fb = resolve_endpoint(server=None, path=None, secret_env=None)

    # Assert — falls back to WS_*
    assert endpoint_fb["server"] == "prod.example.com"
    assert endpoint_fb["path"] == "/ws/prod"


# ---------------------------------------------------------------------------
# AC#7: --chunk-size and --ack-mode tunables
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
@pytest.mark.asyncio
async def test_chunk_size_respected():
    """AC#7: chunk_size controls the maximum bytes per write call in send loop."""
    from teleproto3.bench.bench_client import mode_sink

    # Arrange — 4 KB payload with 1 KB chunk size -> expect 4 write calls
    payload = b"\xef" * 4096
    chunk_size = 1024

    mock_writer = MagicMock()
    writes_captured = []
    mock_writer.write = MagicMock(side_effect=lambda data: writes_captured.append(data))
    mock_writer.drain = AsyncMock()

    # Act
    await mode_sink(
        writer=mock_writer,
        payload=payload,
        chunk_size=chunk_size,
    )

    # Assert — each write call sends at most chunk_size bytes
    assert len(writes_captured) >= 4
    for chunk in writes_captured:
        assert len(chunk) <= chunk_size


@pytest.mark.skip(reason="RED PHASE: bench_client module not yet implemented")
@pytest.mark.asyncio
async def test_ack_mode_sequential():
    """AC#7: sequential ack_mode awaits server ack between each chunk (echo mode)."""
    from teleproto3.bench.bench_client import mode_echo

    # Arrange — 2 KB payload, 1 KB chunks, sequential mode
    payload = b"\xaa" * 2048
    chunk_size = 1024

    call_order = []

    mock_writer = MagicMock()
    mock_writer.write = MagicMock(side_effect=lambda data: call_order.append(("write", len(data))))
    mock_writer.drain = AsyncMock()

    mock_reader = AsyncMock()
    # Each readexactly returns a chunk-sized ack
    async def fake_read(n):
        call_order.append(("read", n))
        return b"\xaa" * n

    mock_reader.readexactly = AsyncMock(side_effect=fake_read)

    # Act
    await mode_echo(
        reader=mock_reader,
        writer=mock_writer,
        payload=payload,
        ack_mode="sequential",
        chunk_size=chunk_size,
    )

    # Assert — sequential mode interleaves write/read: write, read, write, read
    write_indices = [i for i, (op, _) in enumerate(call_order) if op == "write"]
    read_indices = [i for i, (op, _) in enumerate(call_order) if op == "read"]

    assert len(write_indices) >= 2
    assert len(read_indices) >= 2
    # Each read must follow its corresponding write
    for w_idx, r_idx in zip(write_indices, read_indices):
        assert r_idx > w_idx, (
            f"In sequential mode, read (index {r_idx}) must follow "
            f"its write (index {w_idx})"
        )
