"""
socks5_byteshape_test.py — ATDD RED-phase byte-shape conformance for story 9.2 (AC2 + AC4).

RED → GREEN
-----------
Today the shim (lib/src/shim_socks5.c) performs an RFC 6455 WebSocket upgrade
(ws_do_upgrade @ :375, called @ :573) and wraps every SOCKS5 byte in a masked
WS frame (ws_send @ :188 / ws_recv_frame @ :220). Therefore the client->server
bytes on the wire contain `Upgrade: websocket`, a `Sec-WebSocket-Key`, and WS
frame opcode+mask bytes.

AC2 requires the shim be RE-BASED onto the canonical t3_client_* API so the
SOCKS5 stream is carried as length-delimited HTTP-stream chunks (POST + chunked
transfer / t3 chunk framing) with NO WebSocket upgrade and NO WS frames anywhere
on the wire. When that migration lands, these three tests go GREEN.

Until then all three are @pytest.mark.skip (TDD RED) — they assert the EXPECTED
post-migration wire shape against an HTTP-stream mock (NOT a WS mock), so they
fail today purely because the shim still speaks WebSocket.

Mirrors socks5_conformance_test.py: same _STUB_AVAILABLE / HAS_CRYPTOGRAPHY skip
guard, same self-signed-TLS cert helper, same stub-spawn (READY/PORT/USER/PASS)
and RFC1928+RFC1929 SOCKS5 auth handshake. The ONLY structural difference is the
mock server: this one is an HTTP-stream recorder, not a WS server.

Requirements: pytest, cryptography; a test_shim_stub built with T3_SHIM_SOCKS5=ON.
"""

from __future__ import annotations

import asyncio
import datetime as _datetime
import os
import socket
import ssl
import struct
import subprocess
import threading
import time

import pytest

# ---------------------------------------------------------------------------
# Skip guard (mirrors socks5_conformance_test.py)
# ---------------------------------------------------------------------------
try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.backends import default_backend
    HAS_CRYPTOGRAPHY = True
except ImportError:
    HAS_CRYPTOGRAPHY = False

# The faithful HTTP-stream Type3 echo mock (depends on cryptography). Imported
# only when available so collection still skips cleanly without the package.
if HAS_CRYPTOGRAPHY:
    from _t3_http_stream_mock import type3_http_handler

_STUB_BIN = os.environ.get(
    "T3_SHIM_STUB_BIN",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "test_shim_stub"),
)
_STUB_AVAILABLE = os.path.isfile(_STUB_BIN) and os.access(_STUB_BIN, os.X_OK)

# Story 9.2: the shim is now re-based onto t3_client_* (HTTP-stream, no WS), so
# these byte-shape tests are active (the ATDD RED skip marker has been removed).
pytestmark = pytest.mark.skipif(
    not _STUB_AVAILABLE or not HAS_CRYPTOGRAPHY,
    reason="test_shim_stub binary or cryptography package not available; "
           "build with T3_SHIM_SOCKS5=ON and install cryptography",
)

_TEST_SECRET_KEY = bytes(range(16))
_TEST_SECRET_HEX = "ff" + _TEST_SECRET_KEY.hex() + "127.0.0.1"

# WS sentinels that MUST be absent from the post-migration wire.
_WS_TOKENS = (b"Upgrade: websocket", b"upgrade: websocket",
              b"Sec-WebSocket-Key", b"sec-websocket-key",
              b"Sec-WebSocket-Accept", b"101 Switching Protocols")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ShimInfo:
    __slots__ = ("port", "user", "pass_")
    def __init__(self, port: int, user: str, pass_: str) -> None:
        self.port = port
        self.user = user
        self.pass_ = pass_


def _socks5_authenticate(s: socket.socket, info: "ShimInfo") -> None:
    """RFC 1928 method-select + RFC 1929 USER/PASS (mirrors conformance harness)."""
    s.sendall(bytes([0x05, 0x01, 0x02]))
    method_reply = s.recv(2)
    assert method_reply == bytes([0x05, 0x02]), f"bad method reply: {method_reply!r}"
    user_b = info.user.encode("ascii")
    pass_b = info.pass_.encode("ascii")
    s.sendall(bytes([0x01, len(user_b)]) + user_b + bytes([len(pass_b)]) + pass_b)
    auth_reply = s.recv(2)
    assert auth_reply == bytes([0x01, 0x00]), f"auth rejected: {auth_reply!r}"


def _open_authenticated(info: "ShimInfo", timeout: float = 5.0) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(("127.0.0.1", info.port))
    _socks5_authenticate(s, info)
    return s


# ---------------------------------------------------------------------------
# Self-signed TLS cert (mirrors socks5_conformance_test.py::tls_creds)
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def tls_creds(tmp_path_factory):
    assert HAS_CRYPTOGRAPHY
    import ipaddress
    tmp = tmp_path_factory.mktemp("certs9_2")
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                   backend=default_backend())
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"t3-test")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject).issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(_datetime.datetime.utcnow())
        .not_valid_after(_datetime.datetime.utcnow() + _datetime.timedelta(days=1))
        .add_extension(x509.SubjectAlternativeName(
            [x509.DNSName(u"127.0.0.1"),
             x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
        .sign(key, hashes.SHA256(), default_backend())
    )
    cert_path = str(tmp / "cert.pem")
    key_path = str(tmp / "key.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.TraditionalOpenSSL,
                                  serialization.NoEncryption()))
    return cert_path, key_path


# ---------------------------------------------------------------------------
# HTTP-stream MOCK server (NOT WebSocket) + raw client->server byte recorder.
#
# Post-migration the shim must open an HTTP-stream request (POST ... \r\n,
# Transfer-Encoding: chunked, or the t3 chunk framing) and carry the obfs2 init
# + SOCKS5 stream inside length-delimited chunks. This mock records every byte
# the client sends BEFORE any framing is stripped, so the byte-shape asserts can
# inspect the real wire. It deliberately does NOT speak WS — if the shim still
# tries a WS upgrade, the recorded bytes will carry the WS tokens and the asserts
# fail (the RED today), and the handshake will not complete the HTTP-stream path.
# ---------------------------------------------------------------------------
class _Recorder:
    def __init__(self) -> None:
        self.client_bytes = bytearray()
        self.lock = threading.Lock()

    def add(self, data: bytes) -> None:
        with self.lock:
            self.client_bytes += data

    def snapshot(self) -> bytes:
        with self.lock:
            return bytes(self.client_bytes)


@pytest.fixture(scope="function")
def http_stream_mock(tls_creds):
    """Start an HTTP-stream-recording TLS mock; yield (host, port, cert, recorder).

    Records raw client->server bytes. Echoes a minimal HTTP-stream response
    (status line + chunked framing) and then echoes back any payload chunk so
    the round-trip test can observe length-delimited reassembly. NO WS upgrade.
    """
    cert_path, key_path = tls_creds
    host, port = "127.0.0.1", _free_port()
    rec = _Recorder()
    ready = threading.Event()

    # Faithful Type3 HTTP-stream echo (shared with the conformance mock): decodes
    # the obfs2 init, decrypts the length-delimited tunnel stream, terminates the
    # inner SOCKS5 handshake, and echoes payloads back encrypted. Records ALL raw
    # client->server bytes (head + body) for the wire-shape asserts.
    _handle = type3_http_handler(_TEST_SECRET_KEY, recorder=rec)

    async def _run():
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(cert_path, key_path)
        server = await asyncio.start_server(_handle, host, port, ssl=ssl_ctx)
        ready.set()
        async with server:
            await server.serve_forever()

    t = threading.Thread(target=lambda: asyncio.run(_run()), daemon=True)
    t.start()
    assert ready.wait(timeout=5.0), "mock HTTP-stream server failed to bind"
    time.sleep(0.1)
    yield host, port, cert_path, rec


@pytest.fixture(scope="function")
def shim(http_stream_mock):
    """Spawn test_shim_stub pointed at the HTTP-stream mock; yield ShimInfo.

    Mirrors socks5_conformance_test.py::shim (READY/PORT/USER/PASS protocol).
    NOTE: the stub passes a "/ws/test" path literal to t3_shim_open today — the
    path string is just a URL component; AC2 forbids the WS *upgrade*, not a
    path that happens to contain "ws". The byte-shape asserts target the wire,
    not the path string.
    """
    host, port, cert_path, _rec = http_stream_mock
    shim_p = _free_port()
    env = os.environ.copy()
    env["T3_SHIM_STUB_SERVER"] = f"{host}:{port}"
    env["T3_SHIM_STUB_SECRET"] = _TEST_SECRET_HEX
    env["T3_SHIM_STUB_PORT"] = str(shim_p)
    env["T3_SHIM_STUB_CA"] = cert_path
    proc = subprocess.Popen([_STUB_BIN], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready = proc.stdout.readline().strip()
    assert ready == b"READY", f"stub did not print READY; got: {ready!r}"
    port_line = proc.stdout.readline().strip()
    user_line = proc.stdout.readline().strip()
    pass_line = proc.stdout.readline().strip()
    assert port_line.startswith(b"PORT "), f"missing PORT line: {port_line!r}"
    assert user_line.startswith(b"USER "), f"missing USER line: {user_line!r}"
    assert pass_line.startswith(b"PASS "), f"missing PASS line: {pass_line!r}"
    info = ShimInfo(port=int(port_line[5:].decode()),
                    user=user_line[5:].decode(),
                    pass_=pass_line[5:].decode())
    yield info
    proc.terminate()
    proc.wait(timeout=5)


def _drive_connect(info: "ShimInfo", payload: bytes = b"") -> bytes:
    """Auth + SOCKS5 CONNECT to 127.0.0.1:9999; optionally send a payload and
    return the bytes echoed back. Best-effort — a transport error after CONNECT
    is tolerated; the wire asserts run regardless."""
    s = _open_authenticated(info)
    req = bytes([0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1]) + struct.pack(">H", 9999)
    s.sendall(req)
    echoed = b""
    try:
        # Consume the FULL 10-byte CONNECT reply (VER REP RSV ATYP=1 + 4 BND + 2 PORT)
        # before sending the payload — otherwise the leftover BND/PORT bytes would
        # be mis-read as the echo.
        s.settimeout(5.0)
        reply = b""
        while len(reply) < 10:
            chunk = s.recv(10 - len(reply))
            if not chunk:
                break
            reply += chunk
        if payload:
            s.sendall(payload)
            s.settimeout(3.0)
            while len(echoed) < len(payload):
                chunk = s.recv(len(payload) + 16 - len(echoed))
                if not chunk:
                    break
                echoed += chunk
    except Exception:
        pass
    finally:
        s.close()
    time.sleep(0.2)  # let the recorder flush
    return echoed


# ---------------------------------------------------------------------------
# Test cases (all RED until the shim is re-based onto t3_client_*)
# ---------------------------------------------------------------------------

def test_no_ws_upgrade_on_wire(shim, http_stream_mock):
    """[P0] AC2: the client->server wire must carry NO WebSocket upgrade and the
    HTTP-stream request markers (POST + chunked) instead."""
    _host, _port, _cert, rec = http_stream_mock
    _drive_connect(shim)
    wire = rec.snapshot()
    assert wire, "recorder captured no client bytes"

    for tok in _WS_TOKENS:
        assert tok not in wire, f"WS token leaked onto the wire: {tok!r}"

    # HTTP-stream request markers must be present (POST line + chunked framing).
    assert b"POST " in wire or b"POST/" in wire, "no HTTP-stream POST request on wire"
    assert (b"Transfer-Encoding: chunked" in wire
            or b"transfer-encoding: chunked" in wire), "no chunked transfer framing on wire"

    # The HTTP request head must not negotiate a WebSocket upgrade. We assert on
    # the head, NOT a byte-pattern scan of the body: post-migration the body is
    # AES-CTR ciphertext, so scanning it for a 0x82/MASK "WS frame opener" would
    # false-positive on random ciphertext. The absence of the upgrade handshake
    # (tokens above + the Upgrade/Connection headers here) is the real guarantee.
    head_end = wire.find(b"\r\n\r\n")
    assert head_end >= 0, "no HTTP head terminator on wire"
    head = wire[:head_end].lower()
    assert b"upgrade:" not in head, "WS-style Upgrade header on the wire"
    assert b"connection: upgrade" not in head, "WS-style Connection: Upgrade on the wire"


def _recover_obfs2_fields(init64: bytes, secret_key: bytes) -> bytes:
    """Decode obfs2 init [56:64] exactly as the server does (mirrors
    t3_client_crypto.c). Bytes [0:56] are cleartext; [56:64] are AES-256-CTR
    ciphertext that re-encrypts the tag+sentinel in place. Recover them with:
        write_key = SHA256(init[8:40] || secret_key)   write_iv = init[40:56]
    then AES-256-CTR over the 64-byte init. Returns the 64-byte plaintext."""
    import hashlib
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    write_key = hashlib.sha256(init64[8:40] + secret_key).digest()
    write_iv = init64[40:56]
    dec = Cipher(algorithms.AES(write_key), modes.CTR(write_iv),
                 backend=default_backend()).decryptor()
    return dec.update(init64[:64]) + dec.finalize()


def test_obfs2_sentinel_on_wire(shim, http_stream_mock):
    """[P0] AC2+AC1: the obfs2 init carried inside the HTTP-stream body must,
    once decoded server-side, show the padded-intermediate tag 0xdddddddd at
    [56:60] and the SOCKS5-tunnel sentinel 0x53 0x53 at [60:62] (replacing dc_id).

    Bytes [56:64] are obfs2-ENCRYPTED on the wire (t3_client_crypto.c re-encrypts
    them in place), so we re-derive the write key/iv from the cleartext [8:56]
    region + secret and decrypt before asserting — exactly the server's decode.

    !! ACTIVATION CAVEAT (story 9.2 AC2 wire layout is not built yet) !!
    This test assumes the 64-byte obfs2 init is the first 64 bytes of the first
    length-delimited HTTP chunk body. The EXACT framing (t3_client_* HTTP-stream:
    POST + chunked vs. a session-header prefix, 4-byte wire_length, padding) is
    defined BY AC2. When the shim is re-based, confirm where the init lands in the
    recorded `wire` and adjust the chunk/offset parsing below if needed. The
    decode recipe (_recover_obfs2_fields) is correct regardless of offset."""
    _host, _port, _cert, rec = http_stream_mock
    _drive_connect(shim)
    wire = rec.snapshot()
    assert wire, "recorder captured no client bytes"

    head_end = wire.find(b"\r\n\r\n")
    assert head_end >= 0, "no HTTP head terminator on wire"
    body = wire[head_end + 4:]

    # Decode the first HTTP/1.1 chunk: "<hexlen>\r\n<data>\r\n".
    nl = body.find(b"\r\n")
    assert nl > 0, "no chunk size line in body"
    chunk_len = int(body[:nl], 16)
    first_chunk = body[nl + 2: nl + 2 + chunk_len]
    assert len(first_chunk) >= 64, \
        f"first chunk shorter than a 64-byte obfs2 init: {len(first_chunk)}"

    plain = _recover_obfs2_fields(first_chunk[:64], _TEST_SECRET_KEY)
    # Padded-intermediate transport tag at [56:60] (decoded).
    assert plain[56:60] == b"\xdd\xdd\xdd\xdd", \
        f"tag@56 != 0xdddddddd (decoded): {plain[56:60].hex()}"
    # SOCKS5-tunnel sentinel at [60:62] (decoded — replaces dc_id).
    assert plain[60:62] == b"\x53\x53", \
        f"sentinel@60 != 0x5353 (decoded): {plain[60:62].hex()}"


def test_connect_roundtrip_length_delimited(shim, http_stream_mock):
    """[P1] AC2: a CONNECT payload survives the tunnel intact — carried as a
    length-delimited chunk on send and reassembled on receive (the HTTP-stream
    mock echoes the body back through the same chunk framing)."""
    _host, _port, _cert, rec = http_stream_mock
    probe = b"TYPE3-ROUNDTRIP-9.2-" + bytes(range(32))
    echoed = _drive_connect(shim, payload=probe)
    # The mock echoes the post-SOCKS5 body; the probe bytes must reappear intact.
    assert probe in echoed, \
        f"round-trip payload not reassembled intact (got {len(echoed)} bytes)"
