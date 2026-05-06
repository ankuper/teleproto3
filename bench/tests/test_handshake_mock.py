"""
Tests for Story 1a-3 AC#9(a): handshake mock.

Verifies that the client emits command_type=0x04 in the session header
during Type3 handshake, and that KDF + AES-CTR round-trip works correctly.
"""

from __future__ import annotations

import hashlib
import struct

import pytest

from teleproto3.bench.type3_protocol import (
    T3_CMD_BENCH,
    T3_CMD_MTPROTO_PASSTHROUGH,
    _derive_keys,
    _make_random_header,
    _MAGIC_TAGS,
    build_session_header,
)

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def test_session_header_bench():
    """Client emits command_type=0x04 for bench sessions."""
    header = build_session_header(command_type=T3_CMD_BENCH)
    assert header == b"\x04\x01\x00\x00"


def test_session_header_passthrough():
    """Client emits command_type=0x01 for normal sessions."""
    header = build_session_header(command_type=T3_CMD_MTPROTO_PASSTHROUGH)
    assert header == b"\x01\x01\x00\x00"


def test_kdf_deterministic():
    """Same random_header + secret_key always produces same keys."""
    secret = bytes.fromhex("78ef151a20066770db00a2f905c103e9")
    rh = bytes(range(64))

    k1 = _derive_keys(rh, secret)
    k2 = _derive_keys(rh, secret)
    assert k1 == k2


def test_kdf_key_lengths():
    """KDF produces 32-byte keys and 16-byte IVs."""
    secret = bytes(16)
    rh = bytes(64)

    read_key, read_iv, write_key, write_iv = _derive_keys(rh, secret)
    assert len(read_key) == 32
    assert len(read_iv) == 16
    assert len(write_key) == 32
    assert len(write_iv) == 16


def test_kdf_read_key_formula():
    """read_key = SHA256(random_header[8:40] || secret[0:16]) per §4.2."""
    secret = bytes.fromhex("aabbccdd" * 4)
    rh = bytes(range(64))

    read_key, _, _, _ = _derive_keys(rh, secret)
    expected = hashlib.sha256(rh[8:40] + secret).digest()
    assert read_key == expected


def test_kdf_write_key_formula():
    """write_key = SHA256(reverse(random_header[24:56]) || secret[0:16]) per §4.2."""
    secret = bytes.fromhex("aabbccdd" * 4)
    rh = bytes(range(64))

    _, _, write_key, _ = _derive_keys(rh, secret)
    expected = hashlib.sha256(rh[24:56][::-1] + secret).digest()
    assert write_key == expected


def test_random_header_magic_tag():
    """Generated random_header decrypts to a valid magic tag at [56:60)."""
    secret = bytes.fromhex("78ef151a20066770db00a2f905c103e9")
    rh, _, _ = _make_random_header(secret)

    read_key, read_iv, _, _ = _derive_keys(rh, secret)
    cipher = Cipher(algorithms.AES(read_key), modes.CTR(read_iv)).encryptor()
    decrypted = cipher.update(rh)

    tag = struct.unpack("<I", decrypted[56:60])[0]
    assert tag in _MAGIC_TAGS


def test_encryption_round_trip():
    """Data encrypted by client can be decrypted by server (simulated)."""
    secret = bytes.fromhex("78ef151a20066770db00a2f905c103e9")
    rh, client_write, client_read = _make_random_header(secret)

    read_key, read_iv, write_key, write_iv = _derive_keys(rh, secret)

    # Simulate server read: advance past random_header, then decrypt client data
    server_read = Cipher(algorithms.AES(read_key), modes.CTR(read_iv)).encryptor()
    server_read.update(rh)

    plaintext = b"bench test payload for echo mode"
    ciphertext = client_write.update(plaintext)
    decrypted = server_read.update(ciphertext)

    assert decrypted == plaintext


def test_encryption_both_directions():
    """Bidirectional encryption: client→server and server→client."""
    secret = bytes.fromhex("78ef151a20066770db00a2f905c103e9")
    rh, client_write, client_read = _make_random_header(secret)

    read_key, read_iv, write_key, write_iv = _derive_keys(rh, secret)

    # Server write stream
    server_write = Cipher(algorithms.AES(write_key), modes.CTR(write_iv)).encryptor()

    server_msg = b"server response data"
    server_encrypted = server_write.update(server_msg)
    client_decrypted = client_read.update(server_encrypted)

    assert client_decrypted == server_msg


def test_random_header_uniqueness():
    """Two random_header generations produce different headers."""
    secret = bytes(16)
    rh1, _, _ = _make_random_header(secret)
    rh2, _, _ = _make_random_header(secret)
    assert rh1 != rh2
