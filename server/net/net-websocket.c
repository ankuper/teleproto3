/*
    WebSocket support for MTProxy nginx tunnel mode.
    Implements RFC 6455 WebSocket protocol.
    
    Copyright 2026 ToxeH
*/

#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "common/kprintf.h"
#include "net/net-connections.h"
#include "net/net-msg.h"
#include "net/net-websocket.h"

// RFC 6455 magic GUID for Sec-WebSocket-Accept computation
static const char WS_MAGIC_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ---- Base64 encode helper ---- */
static int base64_encode (const unsigned char *in, int in_len, char *out, int out_len) {
  BIO *b64 = BIO_new (BIO_f_base64());
  BIO *mem = BIO_new (BIO_s_mem());
  b64 = BIO_push (b64, mem);
  BIO_set_flags (b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write (b64, in, in_len);
  BIO_flush (b64);
  BUF_MEM *bptr;
  BIO_get_mem_ptr (b64, &bptr);
  if (bptr->length >= out_len) {
    BIO_free_all (b64);
    return -1;
  }
  memcpy (out, bptr->data, bptr->length);
  out[bptr->length] = 0;
  int ret = (int)bptr->length;
  BIO_free_all (b64);
  return ret;
}

/* ---- Sec-WebSocket-Accept computation (RFC 6455 Section 4.2.2) ---- */
int ws_compute_accept_key (const char *key, char *out, int out_len) {
  // Concatenate key + magic GUID
  char concat[256];
  int key_len = strlen (key);
  if (key_len + sizeof(WS_MAGIC_GUID) > sizeof(concat)) {
    return -1;
  }
  memcpy (concat, key, key_len);
  memcpy (concat + key_len, WS_MAGIC_GUID, sizeof(WS_MAGIC_GUID) - 1);
  int concat_len = key_len + sizeof(WS_MAGIC_GUID) - 1;
  
  // SHA1 hash
  unsigned char sha1_result[SHA_DIGEST_LENGTH];
  SHA1 ((unsigned char *)concat, concat_len, sha1_result);
  
  // Base64 encode
  return base64_encode (sha1_result, SHA_DIGEST_LENGTH, out, out_len);
}

/* ---- Build HTTP 101 Switching Protocols response ---- */
int ws_build_upgrade_response (const char *accept_key, char *buf, int buf_len) {
  return snprintf (buf, buf_len,
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n"
    "\r\n", accept_key);
}

/* ---- Parse HTTP Upgrade request ---- */
// Looks for: GET <path> HTTP/1.1 + Upgrade: websocket + Sec-WebSocket-Key
// Returns: 1 = valid WS upgrade, 0 = need more data, -1 = not a WS upgrade
int ws_parse_upgrade_request (struct raw_message *in, char *ws_key, int ws_key_len, char *path, int path_len) {
  int total = in->total_bytes;
  if (total < 4) {
    return 0; // need more data
  }
  
  // We need to find \r\n\r\n (end of HTTP headers)
  // Read up to 4096 bytes for header parsing
  int read_len = total < 4096 ? total : 4096;
  unsigned char buf[read_len + 1];
  assert (rwm_fetch_lookup (in, buf, read_len) == read_len);
  buf[read_len] = 0;
  
  char *header_end = strstr ((char *)buf, "\r\n\r\n");
  if (!header_end) {
    vkprintf (0, "WS_PARSE: no double CRLF found\n");
    if (read_len >= 4096) {
      return -1; // header too large, not a valid request
    }
    return 0; // need more data
  }
  
  // Check it starts with GET
  if (memcmp (buf, "GET ", 4) != 0) {
    return -1;
  }
  
  // Extract path (between "GET " and " HTTP/")
  char *path_start = (char *)buf + 4;
  char *path_end = strstr (path_start, " HTTP/");
  if (!path_end) {
    vkprintf (0, "WS_PARSE: no HTTP/ found\n");
    return -1;
  }
  int plen = path_end - path_start;
  if (plen >= path_len) plen = path_len - 1;
  memcpy (path, path_start, plen);
  path[plen] = 0;
  
  // Check for Upgrade: websocket (case-insensitive)
  if (!strcasestr ((char *)buf, "Upgrade: websocket")) {
    vkprintf (0, "WS_PARSE: no Upgrade: websocket found\n");
    return -1;
  }
  
  // Extract Sec-WebSocket-Key
  char *key_header = strcasestr ((char *)buf, "Sec-WebSocket-Key:");
  if (!key_header) {
    vkprintf (0, "WS_PARSE: no Sec-WebSocket-Key found\n");
    return -1;
  }
  key_header += strlen ("Sec-WebSocket-Key:");
  while (*key_header == ' ') key_header++;
  char *key_end = strstr (key_header, "\r\n");
  if (!key_end) {
    return -1;
  }
  int klen = key_end - key_header;
  if (klen >= ws_key_len) klen = ws_key_len - 1;
  memcpy (ws_key, key_header, klen);
  ws_key[klen] = 0;
  
  vkprintf (0, "WS_PARSE: WebSocket upgrade success: path=%s, key=%s\n", path, ws_key);
  return 1;
}

/* ---- Parse WebSocket frame header ---- */
// RFC 6455 Section 5.2
// Client→Server frames MUST be masked
// Returns payload bytes available, 0 = need more, -1 = error
int ws_parse_frame_header (struct connection_info *c, struct raw_message *in) {
  int total = in->total_bytes;
  if (total < 2) {
    return 0;
  }
  
  unsigned char header[14]; // max header size: 2 + 8 + 4
  int peek_len = total < 14 ? total : 14;
  assert (rwm_fetch_lookup (in, header, peek_len) == peek_len);
  
  int pos = 0;
  // Byte 0: FIN + opcode
  // int fin = (header[0] >> 7) & 1;
  int opcode = header[0] & 0x0F;
  pos++;
  
  // Byte 1: MASK + payload length
  int masked = (header[1] >> 7) & 1;
  int payload_len = header[1] & 0x7F;
  pos++;
  
  if (payload_len == 126) {
    if (peek_len < pos + 2) return 0;
    payload_len = (header[pos] << 8) | header[pos + 1];
    pos += 2;
  } else if (payload_len == 127) {
    if (peek_len < pos + 8) return 0;
    // Reject frames > 2GB (upper 4 bytes must be zero)
    if (header[pos] | header[pos + 1] | header[pos + 2] | header[pos + 3]) {
      vkprintf (1, "WebSocket frame too large (>4GB)\n");
      return -1;
    }
    payload_len = (header[pos + 4] << 24) | (header[pos + 5] << 16) |
                  (header[pos + 6] << 8) | header[pos + 7];
    if (payload_len < 0) {
      vkprintf (1, "WebSocket frame too large (>2GB)\n");
      return -1;
    }
    pos += 8;
  }
  
  if (masked) {
    if (peek_len < pos + 4) return 0;
    memcpy (c->ws_mask, header + pos, 4);
    c->ws_mask_offset = 0;
    pos += 4;
  }
  
  int header_len = pos;

  // Handle control frames
  if (opcode == WS_OPCODE_CLOSE) {
    vkprintf (1, "WebSocket close frame received\n");
    return -1;
  }
  if (opcode == WS_OPCODE_PING) {
    // Need full header + payload to skip
    if (total < header_len + payload_len) return 0;
    vkprintf (2, "WebSocket ping received, length %d\n", payload_len);
    rwm_skip_data (in, header_len + payload_len);
    return 0; // signal to re-parse
  }
  if (opcode == WS_OPCODE_PONG) {
    if (total < header_len + payload_len) return 0;
    vkprintf (2, "WebSocket pong received, length %d\n", payload_len);
    rwm_skip_data (in, header_len + payload_len);
    return 0;
  }

  // Data frames (TEXT, BINARY, CONTINUATION)
  // Skip header, set remaining payload for caller to consume
  if (total < header_len) return 0;

  rwm_skip_data (in, header_len);
  c->ws_frame_remaining = payload_len;

  vkprintf (2, "WebSocket frame: opcode=%d, masked=%d, payload_len=%d\n", opcode, masked, payload_len);
  return payload_len;
}

/* ---- Write WebSocket binary frame header ---- */
// Server→Client: NO masking
int ws_write_frame_header (struct raw_message *out, int payload_len) {
  unsigned char header[10];
  int pos = 0;
  
  // Byte 0: FIN=1, opcode=0x02 (binary)
  header[pos++] = 0x82;
  
  // Byte 1+: payload length, no mask
  if (payload_len < 126) {
    header[pos++] = (unsigned char)payload_len;
  } else if (payload_len < 65536) {
    header[pos++] = 126;
    header[pos++] = (payload_len >> 8) & 0xFF;
    header[pos++] = payload_len & 0xFF;
  } else {
    header[pos++] = 127;
    // 8-byte length, upper 4 bytes are 0
    header[pos++] = 0;
    header[pos++] = 0;
    header[pos++] = 0;
    header[pos++] = 0;
    header[pos++] = (payload_len >> 24) & 0xFF;
    header[pos++] = (payload_len >> 16) & 0xFF;
    header[pos++] = (payload_len >> 8) & 0xFF;
    header[pos++] = payload_len & 0xFF;
  }
  
  rwm_push_data (out, header, pos);
  return pos;
}

/* ---- Unmask data in-place ---- */
void ws_unmask_data (struct connection_info *c, unsigned char *data, int len) {
  int i;
  for (i = 0; i < len; i++) {
    data[i] ^= c->ws_mask[(c->ws_mask_offset + i) & 3];
  }
  c->ws_mask_offset = (c->ws_mask_offset + len) & 3;
}
