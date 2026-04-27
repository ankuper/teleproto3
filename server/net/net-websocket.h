/*
    WebSocket support for MTProxy nginx tunnel mode.
    
    Implements RFC 6455 WebSocket protocol:
    - Handshake (HTTP Upgrade → 101 Switching Protocols)
    - Binary frame parsing (client→server, masked)
    - Binary frame serialization (server→client, unmasked)
    
    Copyright 2026 ToxeH
*/

#pragma once

#include "net/net-connections.h"

#define WS_STATE_NONE       0
#define WS_STATE_HANDSHAKE  1
#define WS_STATE_ACTIVE     2

// WebSocket opcodes
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT         0x1
#define WS_OPCODE_BINARY       0x2
#define WS_OPCODE_CLOSE        0x8
#define WS_OPCODE_PING         0x9
#define WS_OPCODE_PONG         0xA

// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key
// key: null-terminated base64 string from client
// out: 29-byte buffer for base64-encoded SHA1 result (null-terminated)
// Returns 0 on success, -1 on error
int ws_compute_accept_key (const char *key, char *out, int out_len);

// Build HTTP 101 response for WebSocket upgrade
// Returns length of response written to buf
int ws_build_upgrade_response (const char *accept_key, char *buf, int buf_len);

// Parse incoming WebSocket frame header from raw_message
// Returns: number of payload bytes available to read, 0 if need more data, -1 on error
// On success, sets c->ws_frame_remaining, c->ws_mask, c->ws_mask_offset
int ws_parse_frame_header (struct connection_info *c, struct raw_message *in);

// Write a WebSocket binary frame header for outgoing data
// Server→client frames are NOT masked (per RFC 6455)
// payload_len: length of the payload that follows
// Returns: number of header bytes written to out
int ws_write_frame_header (struct raw_message *out, int payload_len);

// Unmask data in-place using the connection's current mask state
void ws_unmask_data (struct connection_info *c, unsigned char *data, int len);

// Try to parse HTTP Upgrade request from raw_message
// Returns: 1 if valid WS upgrade found (ws_key filled), 0 if need more data, -1 if not WS
int ws_parse_upgrade_request (struct raw_message *in, char *ws_key, int ws_key_len, char *path, int path_len);
