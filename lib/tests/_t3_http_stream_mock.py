"""
_t3_http_stream_mock.py — shared HTTP-stream Type3 server mock for the SOCKS5
shim tests (story 9.2).

It mirrors the obfs2/AES-CTR crypto of the legacy WebSocket mock in
socks5_conformance_test.py, but speaks the t3_client_* HTTP-stream wire instead
of WebSocket:

  client -> server:
    POST <path> HTTP/1.1 ... Transfer-Encoding: chunked  (request head)
    chunk #1            = the 64-byte obfs2 init (cleartext [0:56], AES-CTR [56:64])
    chunk #2, #3, ...   = AES-CTR ciphertext of one t3 frame each:
                            [wire_length:4 LE][inner][padding 0-15]
                          where inner = [inner_len:2 LE][socks5 bytes]   (story 9.2)
  server -> client:
    HTTP/1.1 200 OK ... Transfer-Encoding: chunked       (response head)
    chunks of AES-CTR ciphertext of [wire_length:4 LE][inner] (no padding)

Key derivation (canonical, identical to the WS mock and t3_client_crypto.c):
    enc_key = SHA256(rh[8:40]  || secret)   enc_iv = rh[40:56]   (client write / server read)
    dec_key = SHA256(rev(rh[24:56]) || secret) dec_iv = rev(rh[8:24]) (server write / client read)
The server advances its read keystream by the 64-byte header (the client encrypts
the header first), then decrypts the data chunks in order.

The handler terminates the inner SOCKS5 handshake (NO-AUTH greeting + CONNECT)
and then echoes the byte stream, so a round-trip test can observe length-delimited
reassembly. Requires the `cryptography` package.
"""

from __future__ import annotations

import hashlib
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend


class Recorder:
    """Thread-safe-ish accumulator of raw client->server bytes for wire asserts."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def add(self, data: bytes) -> None:
        self._buf += data

    def snapshot(self) -> bytes:
        return bytes(self._buf)


def _aes_ctr(key: bytes, iv: bytes):
    return Cipher(algorithms.AES(key), modes.CTR(iv), backend=default_backend()).encryptor()


class _ChunkReader:
    """Incremental HTTP/1.1 chunked-body reader over an asyncio StreamReader.

    Records every *newly read* raw byte into `recorder` (the leftover handed in
    via `initial` was already recorded by read_http_head, so it is not re-added).
    """

    def __init__(self, reader, initial: bytes = b"", recorder: "Recorder | None" = None) -> None:
        self._reader = reader
        self._buf = bytearray(initial)
        self._recorder = recorder

    async def _fill(self) -> bool:
        data = await self._reader.read(4096)
        if not data:
            return False
        if self._recorder is not None:
            self._recorder.add(data)
        self._buf += data
        return True

    async def read_chunk(self) -> "bytes | None":
        """Return the next chunk's data bytes, or None at EOF / terminal chunk."""
        while b"\r\n" not in self._buf:
            if not await self._fill():
                return None
        nl = self._buf.index(b"\r\n")
        try:
            size = int(bytes(self._buf[:nl]), 16)
        except ValueError:
            return None
        need = nl + 2 + size + 2
        while len(self._buf) < need:
            if not await self._fill():
                return None
        data = bytes(self._buf[nl + 2: nl + 2 + size])
        del self._buf[:need]
        if size == 0:
            return None  # terminal chunk
        return data


async def read_http_head(reader, recorder: "Recorder | None" = None):
    """Read the HTTP request head; return (head_bytes, leftover_body_bytes).

    Records the full first read(s) (head + any leftover) into `recorder`.
    Returns (None, b"") on early EOF.
    """
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = await reader.read(4096)
        if not chunk:
            return None, b""
        head += chunk
    if recorder is not None:
        recorder.add(head)
    idx = head.index(b"\r\n\r\n") + 4
    return head[:idx], head[idx:]


def type3_http_handler(secret_key: bytes, *, recorder: "Recorder | None" = None):
    """Build an asyncio start_server handler that speaks the Type3 HTTP-stream
    tunnel protocol, terminates the inner SOCKS5 handshake, and echoes data."""

    async def handler(reader, writer) -> None:
        try:
            head, leftover = await read_http_head(reader, recorder)
            if head is None:
                writer.close()
                return
            writer.write(
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Transfer-Encoding: chunked\r\n\r\n"
            )
            await writer.drain()

            cr = _ChunkReader(reader, leftover, recorder)

            # chunk #1 = 64-byte obfs2 init
            init = await cr.read_chunk()
            if not init or len(init) < 64:
                writer.close()
                return
            rh = init[:64]
            sk = secret_key
            enc_key = hashlib.sha256(rh[8:40] + sk).digest()
            enc_iv = rh[40:56]
            dec_key = hashlib.sha256(rh[24:56][::-1] + sk).digest()
            dec_iv = rh[8:24][::-1]

            srv_dec = _aes_ctr(enc_key, enc_iv)
            srv_dec.update(rh)  # advance past the 64-byte header the client encrypted first
            srv_enc = _aes_ctr(dec_key, dec_iv)

            pending = bytearray()  # decrypted [wire_length][inner] stream

            async def recv_msg():
                """Strip t3 wire_length + 2-byte inner length + padding -> socks5 bytes."""
                while True:
                    while len(pending) < 4:
                        ch = await cr.read_chunk()
                        if ch is None:
                            return None
                        pending.extend(srv_dec.update(ch))
                    wire_length = struct.unpack("<I", bytes(pending[:4]))[0] & 0x7FFFFFFF
                    while len(pending) < 4 + wire_length:
                        ch = await cr.read_chunk()
                        if ch is None:
                            return None
                        pending.extend(srv_dec.update(ch))
                    frame = bytes(pending[4:4 + wire_length])
                    del pending[:4 + wire_length]
                    if len(frame) < 2:
                        continue  # malformed inner frame; skip
                    inner_len = frame[0] | (frame[1] << 8)
                    return frame[2:2 + inner_len]

            async def send_msg(data: bytes) -> None:
                inner = bytes([len(data) & 0xFF, (len(data) >> 8) & 0xFF]) + data
                frame = struct.pack("<I", len(inner)) + inner  # no padding
                ct = srv_enc.update(frame)
                writer.write(b"%x\r\n" % len(ct) + ct + b"\r\n")
                await writer.drain()

            # inner SOCKS5: NO-AUTH greeting (05 01 00)
            greet = await recv_msg()
            if greet is None or len(greet) < 2 or greet[0] != 0x05:
                writer.close()
                return
            await send_msg(bytes([0x05, 0x00]))

            # inner SOCKS5: CONNECT request
            req = await recv_msg()
            if req is None or len(req) < 4 or req[1] != 0x01:
                writer.close()
                return
            await send_msg(bytes([0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0]))

            # echo the post-handshake byte stream
            while True:
                data = await recv_msg()
                if data is None:
                    break
                if data:
                    await send_msg(data)
        except (ConnectionError, BrokenPipeError, OSError):
            pass
        finally:
            try:
                writer.close()
            except Exception:
                pass

    return handler
