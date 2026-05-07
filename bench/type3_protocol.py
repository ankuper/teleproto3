"""
Type3 protocol handshake: secret parsing, WebSocket upgrade, KDF, obfuscated-2 AES-256-CTR.

Implements wire-format.md §1 (WS upgrade), §3 (Session Header), §4 (obfuscated-2 stream).
Secret parsing per secret-format.md §1.

Exports:
    connect_type3(server, port, path, secret, command_type) → (reader, writer, session)
    build_session_header(command_type, version, flags) → bytes
    parse_secret_hex(hex_string) → dict
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import ssl
import struct
from dataclasses import dataclass, field
from typing import Optional

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

T3_CMD_MTPROTO_PASSTHROUGH = 0x01
T3_CMD_BENCH = 0x04

_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_MAGIC_TAGS = {0xDDDDDDDD, 0xEEEEEEEE, 0xEFEFEFEF}


def parse_secret_hex(hex_string: str) -> dict:
    """Parse a Type3 secret from hex. Returns {marker, key, domain, host, path}."""
    raw = bytes.fromhex(hex_string)
    if len(raw) < 18:
        raise ValueError(f"secret too short: {len(raw)} bytes, need >= 18")
    marker = raw[0]
    if marker != 0xFF:
        raise ValueError(f"bad marker: 0x{marker:02x}, expected 0xff")
    key = raw[1:17]
    domain = raw[17:].decode("utf-8")
    slash_idx = domain.find("/")
    if slash_idx >= 0:
        host = domain[:slash_idx]
        path = domain[slash_idx:]
    else:
        host = domain
        path = ""
    return {"marker": marker, "key": key, "domain": domain, "host": host, "path": path}


def build_session_header(
    command_type: int = T3_CMD_MTPROTO_PASSTHROUGH,
    version: int = 0x01,
    flags: int = 0x0000,
) -> bytes:
    """Build 4-byte Type3 session header: [cmd, version, flags_lo, flags_hi]."""
    return struct.pack("<BBH", command_type, version, flags)


@dataclass
class Type3Session:
    """Holds per-session AES-CTR state for read and write directions."""

    read_encryptor: object = field(repr=False)
    write_encryptor: object = field(repr=False)
    random_header: bytes = field(repr=False)
    command_type: int = T3_CMD_MTPROTO_PASSTHROUGH

    def encrypt(self, plaintext: bytes) -> bytes:
        return self.write_encryptor.update(plaintext)

    def decrypt(self, ciphertext: bytes) -> bytes:
        return self.read_encryptor.update(ciphertext)


def _derive_keys(random_header: bytes, secret_key: bytes) -> tuple:
    """Derive (read_key, read_iv, write_key, write_iv) per wire-format.md §4.2.

    NOTE: These are CLIENT-side names. The client's "write" key encrypts
    data it sends; the server's "read" key is the same key (used to decrypt
    what the client sent). We initialise:
      - write_encryptor with (read_key, read_iv)   — the server's READ stream
        is the client's WRITE stream from its perspective.
      - read_encryptor with (write_key, write_iv)   — vice versa.
    Wait — the spec names are from the SERVER's perspective. Let's be precise:

    Server derives:
      read_key  = SHA256(random_header[8:40]  || secret[0:16])
      read_iv   = random_header[40:56]
      write_key = SHA256(reverse(random_header[24:56]) || secret[0:16])
      write_iv  = reverse(random_header[8:24])

    Client is the mirror: what the server calls "read" is what the client "writes",
    and vice versa.

    Returns (client_encrypt_key, client_encrypt_iv, client_decrypt_key, client_decrypt_iv).
    """
    read_key = hashlib.sha256(random_header[8:40] + secret_key).digest()
    read_iv = random_header[40:56]
    write_key = hashlib.sha256(random_header[24:56][::-1] + secret_key).digest()
    write_iv = random_header[8:24][::-1]
    # Client writes with what the server reads, and reads with what the server writes
    return (read_key, read_iv, write_key, write_iv)


def _make_random_header(secret_key: bytes) -> tuple[bytes, object, object]:
    """Generate 64-byte random_header that passes server-side magic-tag validation.

    The server decrypts random_header with (read_key, read_iv) and checks that
    bytes [56:60) decode as one of the magic tags. We generate random bytes,
    derive keys, then fix bytes [56:60) so the server-side decryption yields a
    valid magic tag.

    Returns (random_header, client_write_encryptor, client_read_encryptor).
    """
    while True:
        random_header = bytearray(os.urandom(64))

        # Derive keys from this candidate header
        read_key, read_iv, write_key, write_iv = _derive_keys(
            bytes(random_header), secret_key
        )

        # The server will decrypt random_header with (read_key, read_iv).
        # We need decrypted[56:60] to be a magic tag.
        # AES-CTR: plaintext = ciphertext XOR keystream
        # We control random_header (the ciphertext from server's POV).
        # Generate keystream by encrypting zeros:
        server_read_cipher = Cipher(
            algorithms.AES(read_key), modes.CTR(read_iv)
        ).encryptor()
        keystream = server_read_cipher.update(b"\x00" * 64)

        # Pick a magic tag and XOR it into the right position
        magic = struct.pack("<I", 0xEFEFEFEF)
        for i in range(4):
            random_header[56 + i] = magic[i] ^ keystream[56 + i]

        # Re-derive keys with the fixed header (keys depend on header bytes,
        # but only on [8:40] and [24:56] — we modified [56:60) which overlaps
        # with neither [8:40] nor [24:56] since [24:56] is [24..55] inclusive
        # i.e. indices 24,25,...,55. Index 56 is outside.)
        read_key, read_iv, write_key, write_iv = _derive_keys(
            bytes(random_header), secret_key
        )

        # Verify our fix worked
        verify_cipher = Cipher(algorithms.AES(read_key), modes.CTR(read_iv)).encryptor()
        decrypted = verify_cipher.update(bytes(random_header))
        tag = struct.unpack("<I", decrypted[56:60])[0]
        if tag in _MAGIC_TAGS:
            # Build the actual encryptors — skip the first 64 bytes of keystream
            # since those were "consumed" by the random_header transmission.
            # The server decrypts random_header (64 bytes) advancing its read counter.
            # For the client, subsequent writes use the SAME CTR stream after 64 bytes.
            client_write_cipher = Cipher(
                algorithms.AES(read_key), modes.CTR(read_iv)
            ).encryptor()
            # Advance past the random_header's 64 bytes in the write stream
            client_write_cipher.update(bytes(random_header))

            # Client read decryptor uses (write_key, write_iv) — the server's write stream
            client_read_cipher = Cipher(
                algorithms.AES(write_key), modes.CTR(write_iv)
            ).encryptor()

            return bytes(random_header), client_write_cipher, client_read_cipher


def _build_ws_upgrade_request(host: str, path: str, ws_key: bytes) -> bytes:
    """Build HTTP/1.1 WebSocket upgrade request per wire-format.md §1."""
    key_b64 = base64.b64encode(ws_key).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Connection: Upgrade\r\n"
        f"Upgrade: websocket\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key_b64}\r\n"
        f"\r\n"
    )
    return request.encode("ascii")


def _parse_ws_upgrade_response(data: bytes, ws_key: bytes) -> int:
    """Parse HTTP/1.1 101 response. Returns index of first byte after headers."""
    header_end = data.find(b"\r\n\r\n")
    if header_end < 0:
        raise ConnectionError("incomplete HTTP response")
    status_line = data[: data.find(b"\r\n")].decode("ascii", errors="replace")
    if "101" not in status_line:
        raise ConnectionError(f"WS upgrade failed: {status_line}")

    expected_accept = base64.b64encode(
        hashlib.sha1(base64.b64encode(ws_key) + _WS_GUID).digest()
    ).decode("ascii")
    headers_text = data[:header_end].decode("ascii", errors="replace")
    if expected_accept not in headers_text:
        raise ConnectionError("Sec-WebSocket-Accept mismatch")

    return header_end + 4


def _build_ws_frame(payload: bytes, opcode: int = 0x02, mask: bool = True) -> bytes:
    """Build a WebSocket frame (client→server: masked)."""
    frame = bytearray()
    frame.append(0x80 | opcode)  # FIN + opcode

    length = len(payload)
    if length < 126:
        frame.append((0x80 if mask else 0x00) | length)
    elif length < 65536:
        frame.append((0x80 if mask else 0x00) | 126)
        frame.extend(struct.pack(">H", length))
    else:
        frame.append((0x80 if mask else 0x00) | 127)
        frame.extend(struct.pack(">Q", length))

    if mask:
        mask_key = os.urandom(4)
        frame.extend(mask_key)
        masked = bytearray(payload)
        for i in range(len(masked)):
            masked[i] ^= mask_key[i % 4]
        frame.extend(masked)
    else:
        frame.extend(payload)

    return bytes(frame)


_MAX_CONSECUTIVE_EMPTY_FRAMES = 8


async def _read_ws_frame(
    reader: asyncio.StreamReader,
    writer: Optional[asyncio.StreamWriter] = None,
) -> bytes:
    """Read and unmask WebSocket frames, skipping control and non-binary frames.

    If `writer` is provided, responds to PING (opcode 0x09) with PONG (opcode 0x0A)
    per RFC 6455 §5.5.2, echoing the payload. PONG and CLOSE frames are discarded.

    Raises ConnectionError if more than _MAX_CONSECUTIVE_EMPTY_FRAMES (8) consecutive
    zero-length data frames arrive — guards against unbounded loops on misbehaving
    servers (P8).
    """
    consecutive_empty = 0
    while True:
        hdr = await reader.readexactly(2)
        opcode = hdr[0] & 0x0F
        is_masked = bool(hdr[1] & 0x80)
        length = hdr[1] & 0x7F

        if length == 126:
            length = struct.unpack(">H", await reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", await reader.readexactly(8))[0]

        if is_masked:
            mask_key = await reader.readexactly(4)
            data = bytearray(await reader.readexactly(length))
            for i in range(len(data)):
                data[i] ^= mask_key[i % 4]
            payload = bytes(data)
        else:
            payload = await reader.readexactly(length)

        if opcode == 0x09:
            # PING: RFC 6455 §5.5.2 mandates an unsolicited PONG echoing the payload.
            if writer is not None:
                pong_frame = _build_ws_frame(payload, opcode=0x0A, mask=True)
                writer.write(pong_frame)
                await writer.drain()
            continue
        if opcode & 0x08:
            # PONG (0x0A) or CLOSE (0x08): discard without advancing AES-CTR.
            continue
        if opcode not in (0x00, 0x02):
            # Text frame or reserved opcode: discard.
            continue

        # P8: bound consecutive zero-length data frames so a misbehaving server
        # cannot drive an unbounded loop here.
        if length == 0:
            consecutive_empty += 1
            if consecutive_empty > _MAX_CONSECUTIVE_EMPTY_FRAMES:
                raise ConnectionError(
                    f"too many consecutive zero-length data frames "
                    f"(>{_MAX_CONSECUTIVE_EMPTY_FRAMES}); server protocol violation"
                )
            continue
        consecutive_empty = 0

        return payload


class Type3Connection:
    """Wraps a raw TCP connection with Type3 WS framing + AES-CTR encryption."""

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        session: Type3Session,
    ):
        self._reader = reader
        self._writer = writer
        self._session = session
        self._read_buf = bytearray()

    async def send(self, plaintext: bytes) -> None:
        """Encrypt and send data as a masked WS binary frame."""
        ciphertext = self._session.encrypt(plaintext)
        frame = _build_ws_frame(ciphertext, opcode=0x02, mask=True)
        self._writer.write(frame)
        await self._writer.drain()

    async def send_raw_ws(self, payload: bytes) -> None:
        """Send raw (unencrypted) bytes as a masked WS binary frame."""
        frame = _build_ws_frame(payload, opcode=0x02, mask=True)
        self._writer.write(frame)
        await self._writer.drain()

    async def recv(self, n: int) -> bytes:
        """Receive and decrypt exactly n bytes from the WS stream.

        Passes self._writer to _read_ws_frame so PINGs get a PONG response per
        RFC 6455 §5.5.2 (P7).
        """
        while len(self._read_buf) < n:
            frame_data = await _read_ws_frame(self._reader, self._writer)
            decrypted = self._session.decrypt(frame_data)
            self._read_buf.extend(decrypted)
        result = bytes(self._read_buf[:n])
        self._read_buf = self._read_buf[n:]
        return result

    async def recv_available(self) -> bytes:
        """Receive whatever is available in the next WS frame."""
        frame_data = await _read_ws_frame(self._reader, self._writer)
        if not frame_data:
            return b""
        return self._session.decrypt(frame_data)

    async def close(self) -> None:
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except Exception:
            pass


async def connect_type3(
    server: str,
    port: int,
    path: str,
    secret: bytes,
    command_type: int = T3_CMD_MTPROTO_PASSTHROUGH,
    tls: bool = True,
) -> tuple[Type3Connection, Type3Session]:
    """Connect to a Type3 server, perform WS upgrade + obfuscated-2 handshake.

    Args:
        server: hostname
        port: TCP port (typically 443 for TLS)
        path: WebSocket path (e.g. "/ws/7f34ba")
        secret: 16-byte secret key (NOT the full hex secret — just the key portion)
        command_type: session header command type (0x01 = passthrough, 0x04 = bench)
        tls: whether to use TLS (default True)

    Returns:
        (connection, session) where connection is a Type3Connection providing
        send/recv, and session holds the crypto state.
    """
    if tls:
        ssl_ctx = ssl.create_default_context()
        reader, writer = await asyncio.open_connection(server, port, ssl=ssl_ctx)
    else:
        reader, writer = await asyncio.open_connection(server, port)

    ws_key = os.urandom(16)
    upgrade_req = _build_ws_upgrade_request(server, path, ws_key)
    writer.write(upgrade_req)
    await writer.drain()

    response = b""
    while b"\r\n\r\n" not in response:
        chunk = await reader.read(4096)
        if not chunk:
            raise ConnectionError("connection closed during WS upgrade")
        response += chunk

    body_start = _parse_ws_upgrade_response(response, ws_key)
    leftover = response[body_start:]

    # Generate random_header and derive AES-CTR keys
    random_header, client_write_enc, client_read_enc = _make_random_header(secret)

    session = Type3Session(
        read_encryptor=client_read_enc,
        write_encryptor=client_write_enc,
        random_header=random_header,
        command_type=command_type,
    )

    # Build session header (plaintext, not encrypted) + random_header (also plaintext on wire)
    session_header = build_session_header(command_type)

    # Send session header + random_header as a single WS frame
    handshake_payload = session_header + random_header
    frame = _build_ws_frame(handshake_payload, opcode=0x02, mask=True)
    writer.write(frame)
    await writer.drain()

    conn = Type3Connection(reader, writer, session)

    # If there was leftover data after HTTP headers, feed it to the read buffer
    if leftover:
        conn._read_buf.extend(leftover)

    return conn, session
