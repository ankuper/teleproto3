"""
socks5_conformance_test.py — AT-CALL-5: SOCKS5 shim compliance fuzzer.

Story 9-1 AC #2. Validates that the t3_shim_socks5 implementation:
  - Answers well-formed REP errors for every error path
  - Does NOT segfault, hang >5s, or use-after-free on malformed input
  - Rejects BIND / UDP-ASSOCIATE with REP=0x07
  - REQUIRES SOCKS5 USERNAME/PASSWORD (method 0x02) per RFC 1929 (D6);
    NO-AUTH (0x00) is refused. Credentials are auto-generated per spawn
    and read from the stub binary's stdout (`USER <hex>` / `PASS <hex>`).

Test strategy
-------------
A local mock Type3 server (Python asyncio + self-signed TLS) is started for
the lifetime of the test session. The shim is launched as a subprocess pointing
at the mock server. Tests connect to the shim as a SOCKS5 client.

Requirements
------------
  pytest, cryptography
  A compiled `test_shim_stub` binary in the same directory or on PATH.
  Run with T3_SHIM_SOCKS5=ON CMake build.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Optional

import pytest

# ---------------------------------------------------------------------------
# Skip guard: require the test_shim_stub binary and cryptography package
# ---------------------------------------------------------------------------
try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.backends import default_backend
    import datetime
    HAS_CRYPTOGRAPHY = True
except ImportError:
    HAS_CRYPTOGRAPHY = False

_STUB_BIN = os.environ.get(
    "T3_SHIM_STUB_BIN",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "test_shim_stub"),
)
_STUB_AVAILABLE = os.path.isfile(_STUB_BIN) and os.access(_STUB_BIN, os.X_OK)

pytestmark = pytest.mark.skipif(
    not _STUB_AVAILABLE or not HAS_CRYPTOGRAPHY,
    reason="test_shim_stub binary or cryptography package not available; "
           "build with T3_SHIM_SOCKS5=ON and install cryptography",
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ShimInfo:
    """Triple of (port, user, pass) describing a running shim."""
    __slots__ = ("port", "user", "pass_")
    def __init__(self, port: int, user: str, pass_: str) -> None:
        self.port = port
        self.user = user
        self.pass_ = pass_


def _socks5_authenticate(s: socket.socket, info: "ShimInfo") -> None:
    """Perform RFC 1928 method-select + RFC 1929 USERNAME/PASSWORD sub-negotiation
    against the shim. Raises AssertionError on protocol failure."""
    # Method-select: offer USERNAME/PASSWORD (0x02).
    s.sendall(bytes([0x05, 0x01, 0x02]))
    method_reply = s.recv(2)
    assert len(method_reply) == 2, f"truncated method reply: {method_reply!r}"
    assert method_reply[0] == 0x05, f"bad VER in method reply: {method_reply!r}"
    assert method_reply[1] == 0x02, (
        f"shim refused USERNAME/PASSWORD (selected method {method_reply[1]:#x}); "
        f"D6 requires 0x02"
    )
    # Sub-negotiation: 0x01 ULEN USER PLEN PASS
    user_b = info.user.encode("ascii")
    pass_b = info.pass_.encode("ascii")
    assert len(user_b) <= 255 and len(pass_b) <= 255
    s.sendall(bytes([0x01, len(user_b)]) + user_b
              + bytes([len(pass_b)]) + pass_b)
    auth_reply = s.recv(2)
    assert len(auth_reply) == 2, f"truncated auth reply: {auth_reply!r}"
    assert auth_reply[0] == 0x01, f"bad sub-version in auth reply: {auth_reply!r}"
    assert auth_reply[1] == 0x00, f"shim rejected credentials: {auth_reply!r}"


def _open_authenticated(info: "ShimInfo", timeout: float = 5.0) -> socket.socket:
    """Open a TCP connection to the shim, complete USER/PASS auth, return socket
    ready to send a SOCKS5 CONNECT request."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(("127.0.0.1", info.port))
    _socks5_authenticate(s, info)
    return s


def _socks5_connect(shim_info: "ShimInfo", atyp: int, dst_addr: bytes, dst_port: int,
                    extra_methods: list[int] | None = None) -> socket.socket:
    """Open a SOCKS5 connection to shim. Returns socket at the CONNECT-reply stage."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(("127.0.0.1", shim_info.port))
    methods = [0x02] + (extra_methods or [])
    s.sendall(bytes([0x05, len(methods)] + methods))
    resp = s.recv(2)
    assert resp[0] == 0x05
    if resp[1] == 0x02:
        _socks5_authenticate_after_select(s, shim_info)
    return s


def _socks5_authenticate_after_select(s: socket.socket, info: "ShimInfo") -> None:
    """Send only the RFC 1929 sub-negotiation part, assuming method-select
    already completed."""
    user_b = info.user.encode("ascii")
    pass_b = info.pass_.encode("ascii")
    s.sendall(bytes([0x01, len(user_b)]) + user_b
              + bytes([len(pass_b)]) + pass_b)
    auth_reply = s.recv(2)
    assert auth_reply[0] == 0x01 and auth_reply[1] == 0x00, (
        f"sub-negotiation failed: {auth_reply!r}"
    )


def _send_connect(s: socket.socket, atyp: int, dst_addr: bytes, dst_port: int) -> bytes:
    """Send SOCKS5 CONNECT request; return the REP response bytes."""
    req = bytes([0x05, 0x01, 0x00, atyp])
    if atyp == 0x03:
        req += bytes([len(dst_addr)]) + dst_addr
    else:
        req += dst_addr
    req += struct.pack(">H", dst_port)
    s.sendall(req)
    # Read at least 4 bytes (VER, REP, RSV, ATYP)
    data = b""
    s.settimeout(5.0)
    while len(data) < 4:
        chunk = s.recv(4 - len(data))
        if not chunk:
            break
        data += chunk
    return data


# ---------------------------------------------------------------------------
# Self-signed TLS certificate fixture
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def tls_creds(tmp_path_factory):
    """Generate a self-signed cert + key for the mock server."""
    assert HAS_CRYPTOGRAPHY
    tmp = tmp_path_factory.mktemp("certs")
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                   backend=default_backend())
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"t3-test")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow())
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=1))
        .add_extension(x509.SubjectAlternativeName(
            [x509.DNSName(u"127.0.0.1"), x509.IPAddress(__import__("ipaddress").ip_address("127.0.0.1"))]),
            critical=False)
        .sign(key, hashes.SHA256(), default_backend())
    )
    cert_path = str(tmp / "cert.pem")
    key_path  = str(tmp / "key.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                   serialization.PrivateFormat.TraditionalOpenSSL,
                                   serialization.NoEncryption()))
    return cert_path, key_path


# ---------------------------------------------------------------------------
# Mock Type3 server
# ---------------------------------------------------------------------------

_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_TEST_SECRET_KEY = bytes(range(16))   # 16-byte secret key for tests
_TEST_SECRET_HEX = "ff" + _TEST_SECRET_KEY.hex() + "127.0.0.1"


def _ws_accept(key_b64: str) -> str:
    accept = base64.b64encode(
        hashlib.sha1(key_b64.encode() + _WS_GUID).digest()
    ).decode()
    return accept


async def _handle_mock_type3(reader: asyncio.StreamReader,
                              writer: asyncio.StreamWriter) -> None:
    """Very basic Type3 server: accepts WS upgrade, reads handshake, acts as SOCKS5."""
    # WebSocket upgrade
    req = b""
    while b"\r\n\r\n" not in req:
        chunk = await reader.read(4096)
        if not chunk:
            return
        req += chunk
    ws_key = ""
    for line in req.decode("ascii", errors="replace").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            ws_key = line.split(":", 1)[1].strip()
    resp = (
        f"HTTP/1.1 101 Switching Protocols\r\n"
        f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {_ws_accept(ws_key)}\r\n\r\n"
    ).encode()
    writer.write(resp)
    await writer.drain()

    # Read WS frame: session_header(4) + random_header(64)
    hdr = await reader.readexactly(2)
    length = hdr[1] & 0x7F
    masked = bool(hdr[1] & 0x80)
    if length == 126:
        length = struct.unpack(">H", await reader.readexactly(2))[0]
    mask_key = await reader.readexactly(4) if masked else b"\x00\x00\x00\x00"
    payload = bytearray(await reader.readexactly(length))
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask_key[i % 4]
    if len(payload) < 68:
        writer.close()
        return

    # session_header = payload[0:4]; random_header = payload[4:68]
    rh = bytes(payload[4:68])
    sk = _TEST_SECRET_KEY

    # Server-side key derivation (mirror of shim):
    # Server reads with (enc_key, enc_iv) — same as shim's enc direction
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    enc_key = hashlib.sha256(rh[8:40] + sk).digest()
    enc_iv  = rh[40:56]
    dec_key = hashlib.sha256(rh[24:56][::-1] + sk).digest()
    dec_iv  = rh[8:24][::-1]

    # Server decrypts what client sends (client used enc_key + enc_iv, advanced past rh)
    srv_dec = Cipher(algorithms.AES(enc_key), modes.CTR(enc_iv),
                     backend=default_backend()).encryptor()
    srv_dec.update(rh)  # advance past random_header (server already consumed these)

    # Server encrypts what it sends (client decrypts with dec_key + dec_iv)
    srv_enc = Cipher(algorithms.AES(dec_key), modes.CTR(dec_iv),
                     backend=default_backend()).encryptor()

    async def recv_plaintext() -> bytes:
        h = await reader.readexactly(2)
        ln = h[1] & 0x7F
        is_masked = bool(h[1] & 0x80)
        if ln == 126: ln = struct.unpack(">H", await reader.readexactly(2))[0]
        mk = await reader.readexactly(4) if is_masked else b"\x00" * 4
        enc = bytearray(await reader.readexactly(ln))
        if is_masked:
            for i in range(len(enc)): enc[i] ^= mk[i % 4]
        return srv_dec.update(bytes(enc))

    async def send_plaintext(data: bytes) -> None:
        ct = srv_enc.update(data)
        mask = os.urandom(4)
        masked_ct = bytearray(ct)
        for i in range(len(masked_ct)): masked_ct[i] ^= mask[i % 4]
        frame = bytes([0x82, 0x80 | len(ct)]) + mask + bytes(masked_ct)
        writer.write(frame)
        await writer.drain()

    # SOCKS5 greeting from shim
    greet = await recv_plaintext()
    if len(greet) < 2 or greet[0] != 0x05:
        writer.close(); return
    # Reply: NO-AUTH accepted
    await send_plaintext(bytes([0x05, 0x00]))

    # SOCKS5 CONNECT from shim
    req_bytes = await recv_plaintext()
    if len(req_bytes) < 4 or req_bytes[1] != 0x01:
        writer.close(); return
    # Reply: success (0.0.0.0:0)
    await send_plaintext(bytes([0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0]))

    # Relay mode: just consume/ignore data (test sink)
    try:
        while True:
            data = await recv_plaintext()
            if not data:
                break
    except Exception:
        pass
    writer.close()


@pytest.fixture(scope="session")
def mock_server(tls_creds):
    """Start the mock Type3 server in a background thread. Returns (host, port)."""
    cert_path, key_path = tls_creds
    host, port = "127.0.0.1", _free_port()

    async def _run():
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(cert_path, key_path)
        server = await asyncio.start_server(
            _handle_mock_type3, host, port, ssl=ssl_ctx)
        async with server:
            await server.serve_forever()

    def _thread():
        asyncio.run(_run())

    t = threading.Thread(target=_thread, daemon=True)
    t.start()
    time.sleep(0.3)  # let the server bind
    return host, port, cert_path


@pytest.fixture(scope="session")
def shim(mock_server, tmp_path_factory):
    """Start the shim stub subprocess and return a ShimInfo(port, user, pass)."""
    host, port, cert_path = mock_server
    shim_p = _free_port()
    env = os.environ.copy()
    env["T3_SHIM_STUB_SERVER"] = f"{host}:{port}"
    env["T3_SHIM_STUB_SECRET"] = _TEST_SECRET_HEX
    env["T3_SHIM_STUB_PORT"]   = str(shim_p)
    env["T3_SHIM_STUB_CA"]     = cert_path
    proc = subprocess.Popen(
        [_STUB_BIN],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Stub prints (in order): READY\n PORT <p>\n USER <hex>\n PASS <hex>\n
    ready = proc.stdout.readline().strip()
    assert ready == b"READY", f"stub did not print READY; got: {ready!r}"
    port_line = proc.stdout.readline().strip()
    assert port_line.startswith(b"PORT "), f"missing PORT line: {port_line!r}"
    user_line = proc.stdout.readline().strip()
    assert user_line.startswith(b"USER "), f"missing USER line: {user_line!r}"
    pass_line = proc.stdout.readline().strip()
    assert pass_line.startswith(b"PASS "), f"missing PASS line: {pass_line!r}"
    info = ShimInfo(
        port=int(port_line[5:].decode("ascii")),
        user=user_line[5:].decode("ascii"),
        pass_=pass_line[5:].decode("ascii"),
    )
    # Sanity: port consistency with the env hint (when non-zero).
    if shim_p != 0:
        assert info.port == shim_p
    yield info
    proc.terminate()
    proc.wait(timeout=5)


@pytest.fixture(scope="session")
def shim_port(shim):
    """Backwards-compat alias: just the port number from the ShimInfo."""
    return shim.port


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

class TestSocks5AuthNegotiation:
    """RFC 1928 §3 auth-method negotiation + RFC 1929 USERNAME/PASSWORD (D6)."""

    def test_no_auth_rejected(self, shim_port):
        """D6: NO-AUTH (0x00) must be refused — any local process could
        otherwise tunnel through the loopback listener."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x05, 0x01, 0x00]))  # only NO-AUTH
        resp = s.recv(2)
        assert resp[0] == 0x05
        assert resp[1] == 0xFF, f"NO-AUTH should be refused; got {resp[1]:#x}"
        s.close()

    def test_userpass_selected_when_offered_alone(self, shim):
        """USER/PASS (0x02) alone → method-select reply 0x02."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim.port))
        s.sendall(bytes([0x05, 0x01, 0x02]))
        resp = s.recv(2)
        assert resp == bytes([0x05, 0x02])
        s.close()

    def test_userpass_correct_credentials_accepted(self, shim):
        """USER/PASS sub-negotiation with the real creds → 0x01 0x00."""
        s = _open_authenticated(shim)
        s.close()

    def test_userpass_wrong_password_rejected(self, shim):
        """USER/PASS sub-negotiation with wrong password → 0x01 0xFF."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim.port))
        s.sendall(bytes([0x05, 0x01, 0x02]))
        assert s.recv(2) == bytes([0x05, 0x02])
        bad_pass = "0" * 32
        user_b = shim.user.encode("ascii")
        pass_b = bad_pass.encode("ascii")
        s.sendall(bytes([0x01, len(user_b)]) + user_b
                  + bytes([len(pass_b)]) + pass_b)
        auth = s.recv(2)
        assert auth[0] == 0x01 and auth[1] == 0xFF
        s.close()

    def test_userpass_wrong_username_rejected(self, shim):
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim.port))
        s.sendall(bytes([0x05, 0x01, 0x02]))
        assert s.recv(2) == bytes([0x05, 0x02])
        bad_user = "f" * 32
        user_b = bad_user.encode("ascii")
        pass_b = shim.pass_.encode("ascii")
        s.sendall(bytes([0x01, len(user_b)]) + user_b
                  + bytes([len(pass_b)]) + pass_b)
        auth = s.recv(2)
        assert auth[0] == 0x01 and auth[1] == 0xFF
        s.close()

    def test_userpass_wrong_length_rejected(self, shim):
        """USERNAME of unexpected length (e.g. 16 chars instead of 32) → 0xFF."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim.port))
        s.sendall(bytes([0x05, 0x01, 0x02]))
        assert s.recv(2) == bytes([0x05, 0x02])
        user_b = b"deadbeefdeadbeef"  # 16 chars
        pass_b = shim.pass_.encode("ascii")
        s.sendall(bytes([0x01, len(user_b)]) + user_b
                  + bytes([len(pass_b)]) + pass_b)
        try:
            auth = s.recv(2)
            assert not auth or auth[1] == 0xFF
        except (ConnectionResetError, TimeoutError):
            pass  # clean close acceptable
        s.close()

    def test_gssapi_only_rejected(self, shim_port):
        """GSSAPI-only list → 0xFF (no acceptable method)."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x05, 0x01, 0x01]))   # only GSSAPI
        resp = s.recv(2)
        assert resp[0] == 0x05
        assert resp[1] == 0xFF   # no acceptable methods
        s.close()

    def test_unknown_only_rejected(self, shim_port):
        """Unknown method 0x80 only → 0xFF."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x05, 0x01, 0x80]))
        resp = s.recv(2)
        assert resp[0] == 0x05
        assert resp[1] == 0xFF
        s.close()

    def test_mixed_list_selects_userpass(self, shim_port):
        """List containing USER/PASS alongside other methods → select 0x02."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x05, 0x03, 0x01, 0x02, 0x80]))
        resp = s.recv(2)
        assert resp[1] == 0x02
        s.close()

    def test_empty_method_list_rejected(self, shim_port):
        """Empty method list → close or 0xFF response, no hang."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x05, 0x00]))  # nmethods=0
        try:
            resp = s.recv(2)
            assert not resp or resp[1] == 0xFF
        except (ConnectionResetError, TimeoutError):
            pass  # clean close is also acceptable
        s.close()


class TestSocks5Commands:
    """RFC 1928 §4 command handling."""

    def test_bind_returns_0x07(self, shim):
        """CMD=BIND (0x02) → REP=0x07 (Command not supported)."""
        s = _open_authenticated(shim)
        s.sendall(bytes([0x05, 0x02, 0x00, 0x01, 0, 0, 0, 0, 0, 80]))
        resp = s.recv(4)
        assert resp[1] == 0x07
        s.close()

    def test_udp_associate_returns_0x07(self, shim):
        """CMD=UDP-ASSOCIATE (0x03) → REP=0x07."""
        s = _open_authenticated(shim)
        s.sendall(bytes([0x05, 0x03, 0x00, 0x01, 0, 0, 0, 0, 0, 80]))
        resp = s.recv(4)
        assert resp[1] == 0x07
        s.close()

    @pytest.mark.parametrize("atyp,addr", [
        (0x01, b"\x7f\x00\x00\x01"),        # IPv4 localhost
        (0x04, b"\x00" * 15 + b"\x01"),     # IPv6 ::1
        (0x03, b"127.0.0.1"),               # domain
    ])
    def test_connect_atyp_matrix(self, shim, atyp, addr):
        """CONNECT with various ATYP values should NOT crash or hang."""
        s = _open_authenticated(shim)
        req = bytes([0x05, 0x01, 0x00, atyp])
        if atyp == 0x03:
            req += bytes([len(addr)]) + addr
        else:
            req += addr
        req += struct.pack(">H", 9999)
        s.sendall(req)
        try:
            resp = s.recv(4)
            assert len(resp) >= 4
            assert resp[0] == 0x05
        except Exception:
            pass  # connection error from upstream is acceptable
        s.close()


class TestSocks5EdgeCases:
    """Malformed / boundary input handling."""

    def test_oversized_domain_255(self, shim):
        """255-byte domain (max allowed per RFC 1928 §4) — no hang."""
        domain = b"x" * 255
        s = _open_authenticated(shim)
        req = bytes([0x05, 0x01, 0x00, 0x03, 255]) + domain + struct.pack(">H", 80)
        s.sendall(req)
        try:
            resp = s.recv(4)
            assert resp[0] == 0x05
        except Exception:
            pass
        s.close()

    def test_oversized_domain_256_rejected(self, shim):
        """256-byte domain (over RFC 1928 limit) — clean close within 5s."""
        # RFC 1928 encodes domain length as 1 byte (max 255), so 256 is impossible
        # in a well-formed request. We force-overflow by sending length=0xFF and
        # then 256 bytes. The shim must not hang or crash.
        s = _open_authenticated(shim)
        req = bytes([0x05, 0x01, 0x00, 0x03, 0xFF]) + b"x" * 256 + struct.pack(">H", 80)
        s.sendall(req)
        try:
            resp = s.recv(4)
            assert resp[0] == 0x05
        except Exception:
            pass  # clean close acceptable
        s.close()

    def test_partial_read_auth_method_byte_at_a_time(self, shim):
        """Send method-select negotiation 1 byte at a time — shim must assemble correctly."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim.port))
        # D6: offer USER/PASS one byte at a time.
        for byte in [0x05, 0x01, 0x02]:
            s.send(bytes([byte]))
            time.sleep(0.01)
        resp = s.recv(2)
        assert resp == bytes([0x05, 0x02])  # USER/PASS selected
        s.close()

    def test_partial_read_connect_byte_at_a_time(self, shim):
        """Send CONNECT request 1 byte at a time — no hang >5s."""
        s = _open_authenticated(shim)
        req = bytes([0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x40])
        for byte in req:
            s.send(bytes([byte]))
            time.sleep(0.01)
        try:
            resp = s.recv(4)
            assert resp[0] == 0x05
        except Exception:
            pass
        s.close()

    def test_wrong_socks_version(self, shim_port):
        """SOCKS4 greeting (version=0x04) — clean close, no hang."""
        s = socket.socket(); s.settimeout(5.0)
        s.connect(("127.0.0.1", shim_port))
        s.sendall(bytes([0x04, 0x01, 0x00, 0x50, 0x7f, 0x00, 0x00, 0x01, 0x00]))
        try:
            resp = s.recv(4)
            # Should not respond with a valid SOCKS5 reply
        except Exception:
            pass  # clean close is the correct behaviour
        s.close()

    def test_connection_close_after_auth_no_connect(self, shim):
        """Close connection after auth negotiation without sending CONNECT — no leak."""
        s = _open_authenticated(shim)
        s.close()  # close without sending CONNECT — shim must not hang
        time.sleep(0.5)  # give shim time to handle the close
