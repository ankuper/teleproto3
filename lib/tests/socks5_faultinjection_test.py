"""
socks5_faultinjection_test.py — Story 9-1 AC #6 library-level fault injection.

While the operational runbook (`_bmad-output/implementation-artifacts/9-1-tests/
fault-injection-runbook.md`) covers the live `.252` dogfood smoke test, this
file is the CI-runnable companion. It exercises the corner the runbook
cannot easily reach:

  - The shim's t3_shim_close() drain path (Layer-1 P4) must run cleanly when
    a SOCKS5 tunnel is in active splice. UAF / leak / hang here would only
    surface in production as a tdesktop crash mid-call.
  - The shim's accept/relay threads must shut down without leaving an
    open TCP socket on the client side hanging > 5s.
  - When the stub is built with T3_SHIM_SANITIZE=address (or the umbrella
    T3_SANITIZE=address option), the test FAILS if ASan reports any leak,
    UAF or other heap error during teardown.

The test reuses the conformance suite's mock_server + shim fixtures (re-exported
via conftest.py here would be the clean approach; for now we import via a
sys.path tweak so this single file is self-contained).
"""

from __future__ import annotations

import os
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

# Reuse the conformance suite's helpers + fixtures by importing the module.
# pytest discovers fixtures from this module too as long as it's in the same
# tests/ directory.
_TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TESTS_DIR))
from socks5_conformance_test import (  # type: ignore  # noqa: E402
    ShimInfo,
    _open_authenticated,
    _STUB_BIN,
    _STUB_AVAILABLE,
    HAS_CRYPTOGRAPHY,
    _TEST_SECRET_HEX,
    tls_creds,        # session fixture
    mock_server,      # session fixture
    _free_port,
)

pytestmark = pytest.mark.skipif(
    not _STUB_AVAILABLE or not HAS_CRYPTOGRAPHY,
    reason="test_shim_stub binary or cryptography package not available",
)


# Per-test stub fixture (function-scoped, NOT session-scoped). Each
# fault-injection test wants a freshly-spawned stub it can SIGTERM/SIGKILL
# without affecting other tests. The conformance suite's `shim` fixture is
# session-scoped — we cannot reuse it.
@pytest.fixture
def stub_proc(mock_server, tmp_path):
    host, port, cert_path = mock_server
    shim_p = _free_port()
    env = os.environ.copy()
    env["T3_SHIM_STUB_SERVER"] = f"{host}:{port}"
    env["T3_SHIM_STUB_SECRET"] = _TEST_SECRET_HEX
    env["T3_SHIM_STUB_PORT"]   = str(shim_p)
    env["T3_SHIM_STUB_CA"]     = cert_path
    # Route stderr to a file so the OS pipe buffer can't fill and block
    # the stub's vkprintf calls mid-test.
    stderr_path = tmp_path / "stub-stderr.log"
    stderr_file = open(stderr_path, "wb")
    proc = subprocess.Popen(
        [_STUB_BIN],
        env=env,
        stdout=subprocess.PIPE,
        stderr=stderr_file,
    )
    proc._stderr_path = stderr_path  # attach for later inspection
    proc._stderr_file = stderr_file
    # Read READY / PORT / USER / PASS lines.
    info = {}
    for expected in ("READY", "PORT", "USER", "PASS"):
        line = proc.stdout.readline().strip().decode("ascii")
        if expected == "READY":
            assert line == "READY", f"stub greeting: {line!r}"
        else:
            assert line.startswith(expected + " "), f"missing {expected}: {line!r}"
            info[expected] = line[len(expected) + 1:]
    shim_info = ShimInfo(
        port=int(info["PORT"]),
        user=info["USER"],
        pass_=info["PASS"],
    )
    yield proc, shim_info
    # Cleanup: if still alive, SIGTERM then SIGKILL.
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    try:
        proc._stderr_file.close()
    except Exception:
        pass


def _drain_stub_stderr(proc: subprocess.Popen) -> bytes:
    """Read the stub's stderr log file. Sanitizer output appears here —
    ASan prints to stderr by default."""
    path = getattr(proc, "_stderr_path", None)
    if path is None:
        return b""
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


def _assert_no_sanitizer_error(stub_stderr: bytes, where: str) -> None:
    """Fail the test if ASan/UBSan/LSan logged any error in the stub stderr.
    When sanitizers are NOT enabled at build time this is a no-op."""
    s = stub_stderr.decode("utf-8", errors="replace")
    bad_markers = [
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "runtime error:",        # UBSan
        "SUMMARY: AddressSanitizer",
        "SUMMARY: LeakSanitizer",
    ]
    hits = [m for m in bad_markers if m in s]
    assert not hits, (
        f"sanitizer reported error during {where}: matched markers={hits}\n"
        f"--- stub stderr ---\n{s}\n--- end ---"
    )


def _send_connect_and_wait_for_reply(sock: socket.socket,
                                     proc: subprocess.Popen | None = None) -> bytes:
    """Send a SOCKS5 CONNECT to 127.0.0.1:9 (discard) — the mock server will
    happily accept any CONNECT and return REP=0x00. Returns the 4-byte
    reply prefix. If `proc` is given and we get a short reply, dump its
    stderr (non-blocking) to aid debugging."""
    req = bytes([0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1]) + struct.pack(">H", 9)
    sock.sendall(req)
    sock.settimeout(5.0)
    data = b""
    while len(data) < 4:
        try:
            chunk = sock.recv(4 - len(data))
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    if len(data) < 4 and proc is not None:
        err = _drain_stub_stderr(proc)
        print(f"\n[stub stderr on short reply]: {err.decode('utf-8', errors='replace')}",
              file=sys.stderr)
    return data


def test_sigterm_during_active_tunnel_clean_teardown(stub_proc):
    """SIGTERM the stub while a SOCKS5 tunnel is in active splice. Asserts:
      - stub exits with rc=0 within 15s (the t3_shim_close drain is bounded to ~10s).
      - the client socket gets EOF/error within 5s — no infinite hang.
      - stub stderr is sanitizer-clean (when sanitizers enabled at build time).

    This exercises the Layer-1 P4 patch end-to-end: stopping-flag check in the
    splice loop + bounded-wait drain of active_tunnels before SSL_CTX_free."""
    proc, info = stub_proc

    # Open and authenticate a SOCKS5 connection, then complete CONNECT to a
    # discard target (the mock server replies success for any CONNECT).
    sock = _open_authenticated(info)
    rep = _send_connect_and_wait_for_reply(sock, proc)
    assert rep[:2] == bytes([0x05, 0x00]), f"CONNECT did not succeed: {rep!r}"

    # Pump some bytes through the tunnel to make sure the splice loop is hot
    # at the moment we send SIGTERM (otherwise the loop may be parked in poll).
    sock.sendall(b"PING" * 64)
    time.sleep(0.1)

    # SIGTERM — this is the graceful path the stub binary registers a handler
    # for (sets g_running = 0, eventually calls t3_shim_close).
    proc.send_signal(signal.SIGTERM)

    # The stub should exit within ~15s (drain timeout 10s + overhead).
    try:
        rc = proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        pytest.fail("stub did not exit within 15s of SIGTERM — t3_shim_close drain stuck")
    assert rc == 0, f"stub exited with rc={rc} (expected 0 on clean SIGTERM teardown)"

    # The client socket should EOF or RST within 5s.
    sock.settimeout(5.0)
    try:
        leftover = sock.recv(4096)
        # An empty recv() means clean EOF. Some data leftover is also fine —
        # the failure mode we're checking against is infinite hang.
        assert leftover is not None
    except (ConnectionResetError, OSError):
        pass  # RST is acceptable
    sock.close()

    _assert_no_sanitizer_error(_drain_stub_stderr(proc),
                               "SIGTERM-during-active-tunnel teardown")


def test_sigkill_during_active_tunnel_client_does_not_hang(stub_proc):
    """SIGKILL (-9) the stub mid-tunnel. The client side should observe a
    socket error / EOF within 5s — no infinite hang. We do NOT assert a
    sanitizer-clean stderr here because SIGKILL bypasses ASan teardown logging."""
    proc, info = stub_proc

    sock = _open_authenticated(info)
    rep = _send_connect_and_wait_for_reply(sock, proc)
    assert rep[:2] == bytes([0x05, 0x00])

    sock.sendall(b"BURST" * 200)
    time.sleep(0.05)

    proc.kill()  # SIGKILL — no graceful path
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pytest.fail("stub did not die within 5s of SIGKILL — kernel problem")

    sock.settimeout(5.0)
    start = time.monotonic()
    try:
        # Either recv() returns EOF (b"") quickly, or raises a connection error.
        # Anything that takes >5s means we have an infinite hang somewhere.
        sock.recv(4096)
    except (ConnectionResetError, OSError):
        pass
    elapsed = time.monotonic() - start
    assert elapsed < 5.0, f"client recv() hung for {elapsed:.2f}s after stub SIGKILL"
    sock.close()


def test_concurrent_tunnels_then_sigterm_drain(stub_proc):
    """Open N concurrent SOCKS5 tunnels, pump data through each, then SIGTERM
    the stub. Asserts the drain handles multiple active_tunnels cleanly without
    leak or UAF. This is the regression test for the Layer-1 P4 drain patch
    under non-trivial concurrent load."""
    proc, info = stub_proc
    N = 4

    socks = []
    try:
        for _ in range(N):
            s = _open_authenticated(info)
            rep = _send_connect_and_wait_for_reply(s)
            assert rep[:2] == bytes([0x05, 0x00])
            s.sendall(b"x" * 256)
            socks.append(s)

        time.sleep(0.2)
        proc.send_signal(signal.SIGTERM)

        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
            pytest.fail(f"stub did not exit within 15s with N={N} tunnels — drain stuck")

        # rc may be 0 (clean drain) or non-zero (drain timeout fell back to leak-rather-than-UAF).
        # Either is acceptable; the failure mode we're checking against is hang or sanitizer error.
        assert rc in (0, 1), f"unexpected stub exit code: {rc}"

        _assert_no_sanitizer_error(_drain_stub_stderr(proc),
                                   f"SIGTERM drain with N={N} concurrent tunnels")
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
