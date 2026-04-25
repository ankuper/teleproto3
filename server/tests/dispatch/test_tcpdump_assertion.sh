#!/usr/bin/env bash
# test_tcpdump_assertion.sh — AC-PROTO-002.
# Asserts that when the server silently drops a malformed Type3 header,
# tcpdump shows a bare TCP FIN/RST and zero protocol-level error bytes
# on the wire. Runs in server CI against a locally spawned mtproto-proxy.
#
# TODO(server-v0.1.0): implement.
#   1. spawn mtproto-proxy on a random high port
#   2. run tcpdump -w capture.pcap on loopback for that port
#   3. send a malformed Session Header via a helper client
#   4. assert capture contains only TCP control flags + zero payload from server
#   5. teardown cleanly
set -euo pipefail
echo "TODO(AC-PROTO-002): implement tcpdump assertion"
exit 0
