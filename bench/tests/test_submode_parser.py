"""
Tests for Story 1a-3 AC#9(c): sub-mode byte mapping.

Table-driven tests verifying the mode-string → sub-mode-byte mapping
used by bench_client when sending the first encrypted byte to the server.
"""

from __future__ import annotations

import pytest


def test_sub_mode_bytes():
    """Sub-mode bytes match wire-format.md §3.1."""
    from teleproto3.bench.bench_client import (
        BENCH_SUB_MODE_ECHO,
        BENCH_SUB_MODE_SINK,
        BENCH_SUB_MODE_SOURCE,
    )

    assert BENCH_SUB_MODE_SINK == 0x01
    assert BENCH_SUB_MODE_ECHO == 0x02
    assert BENCH_SUB_MODE_SOURCE == 0x03


@pytest.mark.parametrize(
    "mode_str,expected_byte",
    [
        ("sink", 0x01),
        ("echo", 0x02),
        ("source", 0x03),
    ],
)
def test_mode_string_to_byte_mapping(mode_str, expected_byte):
    """CLI --mode string maps to correct sub-mode byte per spec."""
    from teleproto3.bench.bench_client import (
        BENCH_SUB_MODE_ECHO,
        BENCH_SUB_MODE_SINK,
        BENCH_SUB_MODE_SOURCE,
    )

    mapping = {
        "sink": BENCH_SUB_MODE_SINK,
        "echo": BENCH_SUB_MODE_ECHO,
        "source": BENCH_SUB_MODE_SOURCE,
    }
    assert mapping[mode_str] == expected_byte


def test_build_parser_mode_choices():
    """Argparse restricts --mode to exactly {sink, echo, source}."""
    from teleproto3.bench.bench_client import build_parser

    parser = build_parser()

    for action in parser._actions:
        if hasattr(action, "dest") and action.dest == "mode":
            assert set(action.choices) == {"sink", "echo", "source"}
            return
    pytest.fail("--mode argument not found in parser")


def test_build_parser_ack_mode_choices():
    """Argparse restricts --ack-mode to exactly {streaming, sequential}."""
    from teleproto3.bench.bench_client import build_parser

    parser = build_parser()

    for action in parser._actions:
        if hasattr(action, "dest") and action.dest == "ack_mode":
            assert set(action.choices) == {"streaming", "sequential"}
            return
    pytest.fail("--ack-mode argument not found in parser")


def test_build_parser_defaults():
    """Argparse defaults match AC spec values."""
    from teleproto3.bench.bench_client import build_parser

    parser = build_parser()
    defaults = parser.parse_args(["--mode", "echo"])

    assert defaults.chunk_size == 65536
    assert defaults.ack_mode == "streaming"
    assert defaults.runs == 1
    assert defaults.port == 443


def test_secret_parsing():
    """parse_secret_hex correctly splits host and path."""
    from teleproto3.bench.type3_protocol import parse_secret_hex

    hex_str = "ff78ef151a20066770db00a2f905c103e96172637469632d627265657a652e6d792e69642f77732f376633346261"
    parsed = parse_secret_hex(hex_str)

    assert parsed["marker"] == 0xFF
    assert parsed["key"] == bytes.fromhex("78ef151a20066770db00a2f905c103e9")
    assert parsed["host"] == "arctic-breeze.my.id"
    assert parsed["path"] == "/ws/7f34ba"
    assert parsed["domain"] == "arctic-breeze.my.id/ws/7f34ba"


def test_secret_parsing_no_path():
    """parse_secret_hex handles domain without path."""
    from teleproto3.bench.type3_protocol import parse_secret_hex

    hex_str = "ff" + "00" * 16 + "6578616d706c652e636f6d"
    parsed = parse_secret_hex(hex_str)

    assert parsed["host"] == "example.com"
    assert parsed["path"] == "/"
