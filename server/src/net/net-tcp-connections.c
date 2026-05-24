/*
    This file is part of Mtproto-proxy Library.

    Mtproto-proxy Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Mtproto-proxy Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Mtproto-proxy Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2009-2013 Vkontakte Ltd
              2008-2013 Nikolai Durov
              2008-2013 Andrey Lopatin
                   2013 Vitaliy Valtman
    
    Copyright 2014-2016 Telegram Messenger Inc                 
              2015-2016 Vitaly Valtman     
*/

#include <errno.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "net/net-connections.h"
#include "net/net-msg.h"
#include "net/net-msg-buffers.h"
#include "crypto/aesni256.h"
#include "net/net-crypto-aes.h"
#include "net/net-websocket.h"
#include "kprintf.h"


/* Forward declaration: defined after the chunk-header helper (below). */
static void http_stream_write_terminal (connection_job_t C);

int cpu_tcp_free_connection_buffers (connection_job_t C) /* {{{ */ {
  struct connection_info *c = CONN_INFO (C);
  assert_net_cpu_thread ();
  rwm_free (&c->in);
  rwm_free (&c->in_u);
  rwm_free (&c->out);
  rwm_free (&c->out_p);
  return 0;
}
/* }}} */


int cpu_tcp_server_writer (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();

  struct connection_info *c = CONN_INFO (C);
  
  int stop = 0;
  if (c->status == conn_write_close) {
    stop = 1;
    /* For HTTP stream connections, emit the terminal chunk before the final
     * flush so the client receives a clean end-of-stream signal (spec §5). */
    if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM && c->crypto) {
      http_stream_write_terminal (C);
    }
  }
  
  while (1) {
    struct raw_message *raw = mpq_pop_nw (c->out_queue, 4);
    if (!raw) { break; }
    //rwm_union (out, raw);
    c->type->write_packet (C, raw);
    free (raw);
  }
  
  c->type->flush (C);

  struct raw_message *raw = malloc (sizeof (*raw));

  if (c->type->crypto_encrypt_output && c->crypto) {
    if (c->type->crypto_encrypt_output (C) < 0) {
      free (raw);
      return -1;
    }
    *raw = c->out_p;
    rwm_init (&c->out_p, 0);
  } else {
    *raw = c->out;
    rwm_init (&c->out, 0);
  }
 
  if (raw->total_bytes && c->io_conn) {        
    mpq_push_w (SOCKET_CONN_INFO(c->io_conn)->out_packet_queue, raw, 0);
    if (stop) {
      __sync_fetch_and_or (&SOCKET_CONN_INFO(c->io_conn)->flags, C_STOPWRITE);
    }
    job_signal (JOB_REF_CREATE_PASS (c->io_conn), JS_RUN);
  } else {
    rwm_free (raw);
    free (raw);
  }

  return 0;
}
/* }}} */

/*
 * http_stream_chunk_header_parse — shared chunk-header parser for HTTP stream mode.
 *
 * Peeks at `rmsg` (without consuming) to find the chunk-size CRLF line.
 * On success: consumes the header (hex + CRLF) from `rmsg`, fills *out_size,
 *             returns 0.  chunk_size 0 means terminal chunk.
 * Returns  1 if more data is needed (not yet MALFORMED).
 * Returns -1 if the header is MALFORMED (caller must fail_connection).
 *
 * Limits enforced (spec/wire-format.md §2.2):
 *   - max chunk-size: 65535 bytes
 *   - chunk extensions: recognised at ';', skipped (not validated further)
 *   - header line (hex + optional extension): must fit in 16 bytes
 */
static int http_stream_chunk_header_parse (struct raw_message *rmsg, unsigned int *out_size) {
  int avail = rmsg->total_bytes;
  if (avail == 0) {
    return 1; /* need more data */
  }
  int peek_len = avail < 18 ? avail : 18; /* 16 hex + CRLF */
  unsigned char peek_buf[18];
  assert (rwm_fetch_lookup (rmsg, peek_buf, peek_len) == peek_len);

  /* Find first CRLF in the peek window */
  int header_line_len = -1;
  for (int i = 0; i < peek_len - 1; i++) {
    if (peek_buf[i] == '\r' && peek_buf[i + 1] == '\n') {
      header_line_len = i;
      break;
    }
  }
  if (header_line_len < 0) {
    if (avail >= 18) {
      return -1; /* MALFORMED: chunk header too long */
    }
    return 1; /* need more data */
  }
  if (header_line_len == 0) {
    return -1; /* MALFORMED: empty chunk-size field */
  }

  /* Find optional ';' chunk-extension separator */
  int hex_len = header_line_len;
  for (int i = 0; i < header_line_len; i++) {
    if (peek_buf[i] == ';') {
      hex_len = i;
      break;
    }
  }
  if (hex_len == 0) {
    return -1; /* MALFORMED: no hex digits before extension */
  }

  /* Parse hex digits into unsigned int with overflow guard */
  unsigned int size = 0;
  for (int i = 0; i < hex_len; i++) {
    unsigned char ch = peek_buf[i];
    unsigned int digit;
    if      (ch >= '0' && ch <= '9') digit = (unsigned int)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') digit = (unsigned int)(ch - 'a') + 10u;
    else if (ch >= 'A' && ch <= 'F') digit = (unsigned int)(ch - 'A') + 10u;
    else {
      return -1; /* MALFORMED: non-hex character */
    }
    /* Overflow check: size * 16 + digit <= 65535 */
    if (size > (65535u - digit) / 16u) {
      return -1; /* MALFORMED: chunk-size exceeds 65535 (spec §2.2) */
    }
    size = size * 16u + digit;
  }

  /* Consume the header line (hex [;ext] CRLF) from the raw_message */
  int consume = header_line_len + 2;
  assert (rwm_skip_data (rmsg, consume) == consume);

  *out_size = size;
  return 0;
}

/*
 * http_stream_write_terminal — emit the HTTP chunked terminal chunk (0\r\n\r\n)
 * to c->out_p for this connection, signaling end-of-stream to the client.
 * Called before fail_connection() for graceful close in HTTP stream mode.
 */
static void http_stream_write_terminal (connection_job_t C) {
  struct connection_info *c = CONN_INFO (C);
  if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM) {
    rwm_push_data (&c->out_p, "0\r\n\r\n", 5);
    __sync_fetch_and_or (&c->flags, C_WANTWR);
    job_signal (JOB_REF_CREATE_PASS (C), JS_RUN);
  }
}

static int http_stream_unwrap_precrypto (connection_job_t C) {
  struct connection_info *c = CONN_INFO (C);

  int needed = T3_PRECRYPTO_BYTES - c->in.total_bytes; /* 4 Session Header + 64 random_header */
  if (needed <= 0) {
    return 0;
  }

  while (needed > 0 && c->in_u.total_bytes > 0) {
    if (c->http_chunk_remaining == 0) {
      if (c->http_chunk_header_state == 2) {
        if (c->in_u.total_bytes < 2) {
          break;
        }
        unsigned char crlf[2];
        assert (rwm_fetch_lookup (&c->in_u, crlf, 2) == 2);
        if (crlf[0] != '\r' || crlf[1] != '\n') {
          vkprintf (1, "HTTP stream pre-crypto: expected trailing CRLF, got 0x%02x 0x%02x\n", crlf[0], crlf[1]);
          fail_connection (C, -1);
          return -1;
        }
        assert (rwm_skip_data (&c->in_u, 2) == 2);
        c->http_chunk_header_state = 0;
      }

      if (c->http_chunk_header_state == 0) {
        unsigned int chunk_size = 0;
        int r = http_stream_chunk_header_parse (&c->in_u, &chunk_size);
        if (r == 1) {
          break; /* need more data */
        }
        if (r < 0) {
          vkprintf (1, "HTTP stream pre-crypto: MALFORMED chunk header\n");
          fail_connection (C, -1);
          return -1;
        }
        if (chunk_size == 0) {
          vkprintf (1, "HTTP stream pre-crypto: terminal chunk, closing gracefully\n");
          fail_connection (C, 0);
          return -1;
        }
        c->http_chunk_remaining = (int)chunk_size;
        c->http_chunk_header_state = 1;
      }
    }

    int avail = c->in_u.total_bytes;
    int to_read = avail < c->http_chunk_remaining ? avail : c->http_chunk_remaining;
    if (to_read > needed) {
      to_read = needed;
    }
    if (to_read <= 0) break;

    struct raw_message payload;
    rwm_init (&payload, 0);
    assert (rwm_split_head (&payload, &c->in_u, to_read) == 0);
    rwm_union (&c->in, &payload);

    c->http_chunk_remaining -= to_read;
    needed -= to_read;
    if (c->http_chunk_remaining == 0) {
      c->http_chunk_header_state = 2;
    }
  }

  return 0;
}


int cpu_tcp_server_reader (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();
  struct connection_info *c = CONN_INFO(C);

  while (1) {
    struct raw_message *raw = mpq_pop_nw (c->in_queue, 4);
    if (!raw) { break; }

    if (c->crypto || (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM && !c->crypto)) {
      rwm_union (&c->in_u, raw);
    } else {
      rwm_union (&c->in, raw);
    }
    free (raw);
  }
        
  if (c->crypto) {
    if (c->type->crypto_decrypt_input (C) < 0) {
      return -1;
    }
  } else if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM) {
    if (http_stream_unwrap_precrypto (C) < 0) {
      return -1;
    }
  }

  int r = c->in.total_bytes;
        
  int s = c->skip_bytes;

  if (c->type->data_received) {
    c->type->data_received (C, r);
  }

  if (c->flags & (C_FAILED | C_ERROR | C_NET_FAILED)) {
    return -1;
  }
  if (c->flags & C_STOPREAD) {
    return 0;
  }

  int r1 = r;

  if (s < 0) {
    // have to skip s more bytes
    if (r1 > -s) {
      r1 = -s;
    }
    rwm_skip_data (&c->in, r1);
    c->skip_bytes = s += r1;

    vkprintf (2, "skipped %d bytes, %d more to skip\n", r1, -s);
      
    if (s) {
      return 0;
    }
  }

  if (s > 0) {
    // need to read s more bytes before invoking parse_execute()
    if (r1 >= s) {
      c->skip_bytes = s = 0;
    }

    vkprintf (1, "fetched %d bytes, %d available bytes, %d more to load\n", r, r1, s ? s - r1 : 0);
    if (s) {
      return 0;
    }
  }


  while (!c->skip_bytes && !(c->flags & (C_ERROR | C_FAILED | C_NET_FAILED | C_STOPREAD)) && c->status != conn_error) {
    int bytes = c->in.total_bytes;
    if (!bytes) {
      break;
    }

    int res = c->type->parse_execute (C);
    
    // 0 - ok/done, >0 - need that much bytes, <0 - skip bytes, or NEED_MORE_BYTES
    if (!res) {
    } else if (res != NEED_MORE_BYTES) {
      bytes = (c->crypto ? c->in.total_bytes : c->in_u.total_bytes);
      // have to load or skip abs(res) bytes before invoking parse_execute
      if (res < 0) {
        res -= bytes;
      } else {
        res += bytes;
      }
      c->skip_bytes = res;
      break;
    } else {
      break;
    }
  }

  return 0;
}
/* }}} */

int cpu_tcp_aes_crypto_encrypt_output (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();
  struct connection_info *c = CONN_INFO (C);

  struct aes_crypto *T = c->crypto;
  assert (c->crypto);
  struct raw_message *out = &c->out;

  int l = out->total_bytes;
  l &= ~15;
  if (l) {
    if (rwm_encrypt_decrypt_to (&c->out, &c->out_p, l, T->write_aeskey, 16) != l) {
      fail_connection (C, -1);
      return -1;
    }
  }

  return (-out->total_bytes) & 15;
}
/* }}} */

int cpu_tcp_aes_crypto_decrypt_input (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();
  struct connection_info *c = CONN_INFO (C);
  struct aes_crypto *T = c->crypto;
  assert (c->crypto);
  struct raw_message *in = &c->in_u;

  int l = in->total_bytes;
  l &= ~15;
  if (l) {
    if (rwm_encrypt_decrypt_to (&c->in_u, &c->in, l, T->read_aeskey, 16) != l) {
      fail_connection (C, -1);
      return -1;
    }
  }

  return (-in->total_bytes) & 15;
}
/* }}} */

int cpu_tcp_aes_crypto_needed_output_bytes (connection_job_t C) /* {{{ */ {
  struct connection_info *c = CONN_INFO (C);
  assert (c->crypto);
  return -c->out.total_bytes & 15;
}
/* }}} */

static int http_chunk_write_header (struct raw_message *out, int len) {
  char hex_buf[32];
  int hex_len = snprintf (hex_buf, sizeof (hex_buf), "%x\r\n", len);
  return rwm_push_data (out, hex_buf, hex_len);
}

int cpu_tcp_aes_crypto_ctr128_encrypt_output (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();
  struct connection_info *c = CONN_INFO (C);

  struct aes_crypto *T = c->crypto;
  assert (c->crypto);

  while (c->out.total_bytes) {
    int len = c->out.total_bytes;
    if (c->ws_state == WS_STATE_ACTIVE) {
      // Type3 WS transport: wrap outgoing (server→client) payloads in
      // unmasked RFC 6455 binary frames. Cap at 16 KiB for parity with
      // typical middlebox-friendly frame sizes.
      const int WS_MAX_FRAME = 16384;
      if (len > WS_MAX_FRAME) {
        len = WS_MAX_FRAME;
      }
      ws_write_frame_header (&c->out_p, len);
      vkprintf (2, "WS_OUTPUT: send binary frame len=%d\n", len);
    } else if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM) {
      const int HTTP_MAX_CHUNK = 16384;
      if (len > HTTP_MAX_CHUNK) {
        len = HTTP_MAX_CHUNK;
      }
      http_chunk_write_header (&c->out_p, len);
      vkprintf (2, "HTTP_STREAM: send chunk len=%d\n", len);
    } else if (c->flags & C_IS_TLS) {
      assert (c->left_tls_packet_length >= 0);
      const int MAX_PACKET_LENGTH = 1425;
      if (MAX_PACKET_LENGTH < len) {
        len = MAX_PACKET_LENGTH;
      }

      unsigned char header[5] = {0x17, 0x03, 0x03, len >> 8, len & 255};
      rwm_push_data (&c->out_p, header, 5);
      vkprintf (2, "Send TLS-packet of length %d\n", len);
    }

    if (rwm_encrypt_decrypt_to (&c->out, &c->out_p, len, T->write_aeskey, 1) != len) {
      fail_connection (C, -1);
      return -1;
    }

    if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM) {
      rwm_push_data (&c->out_p, "\r\n", 2);
    }
  }

  return 0;
}
/* }}} */

int cpu_tcp_aes_crypto_ctr128_decrypt_input (connection_job_t C) /* {{{ */ {
  assert_net_cpu_thread ();
  struct connection_info *c = CONN_INFO (C);
  struct aes_crypto *T = c->crypto;
  assert (c->crypto);

  while (c->in_u.total_bytes) {
    int len = c->in_u.total_bytes;
    if (c->ws_state == WS_STATE_ACTIVE) {
      // Type3 WS transport: unwrap incoming (client→server, masked) frames.
      // Each frame is: [2..14 byte header] [masked payload]. Parse header
      // once per frame, then unmask and pass payload through the AES-CTR
      // decryptor in fixed-size chunks of up to 16 KiB (stack buffer).
      if (c->ws_frame_remaining == 0) {
        int payload_len = ws_parse_frame_header (c, &c->in_u);
        if (payload_len < 0) {
          vkprintf (1, "WS frame parse error\n");
          fail_connection (C, -1);
          return 0;
        }
        if (payload_len == 0) {
          return 0; // need more data to parse the next frame header
        }
        len = c->in_u.total_bytes;
      }

      if (c->ws_frame_remaining < len) {
        len = c->ws_frame_remaining;
      }
      c->ws_frame_remaining -= len;

      if (len > 0) {
        unsigned char stack_buf[16384];
        unsigned char *tmp = (len <= (int)sizeof(stack_buf)) ? stack_buf : malloc (len);
        assert (rwm_fetch_data (&c->in_u, tmp, len) == len);
        ws_unmask_data (c, tmp, len);
        struct raw_message tmp_msg;
        rwm_create (&tmp_msg, tmp, len);
        int r = rwm_encrypt_decrypt_to (&tmp_msg, &c->in, len, T->read_aeskey, 1);
        rwm_free (&tmp_msg);
        if (tmp != stack_buf) free (tmp);
        if (r != len) {
          fail_connection (C, -1);
          return -1;
        }
        vkprintf (2, "WS_INPUT: decrypted %d bytes, %d remaining in frame\n", len, c->ws_frame_remaining);
      }
      continue;
    } else if (c->transport_mode == TRANSPORT_MODE_HTTP_STREAM) {
      if (c->http_chunk_remaining == 0) {
        if (c->http_chunk_header_state == 2) {
          if (c->in_u.total_bytes < 2) {
            return 0; /* need more data */
          }
          unsigned char crlf[2];
          assert (rwm_fetch_lookup (&c->in_u, crlf, 2) == 2);
          if (crlf[0] != '\r' || crlf[1] != '\n') {
            vkprintf (1, "HTTP stream: expected trailing CRLF, got 0x%02x 0x%02x\n", crlf[0], crlf[1]);
            fail_connection (C, -1);
            return 0;
          }
          assert (rwm_skip_data (&c->in_u, 2) == 2);
          c->http_chunk_header_state = 0;
        }

        if (c->http_chunk_header_state == 0) {
          unsigned int chunk_size = 0;
          int r = http_stream_chunk_header_parse (&c->in_u, &chunk_size);
          if (r == 1) {
            return 0; /* need more data */
          }
          if (r < 0) {
            vkprintf (1, "HTTP stream: MALFORMED chunk header\n");
            fail_connection (C, -1);
            return 0;
          }
          if (chunk_size == 0) {
            vkprintf (1, "HTTP stream: terminal chunk received, closing gracefully\n");
            fail_connection (C, 0);
            return 0;
          }
          c->http_chunk_remaining = (int)chunk_size;
          c->http_chunk_header_state = 1;
        }
      }

      int to_read = c->in_u.total_bytes;
      if (c->http_chunk_remaining < to_read) {
        to_read = c->http_chunk_remaining;
      }
      c->http_chunk_remaining -= to_read;
      if (c->http_chunk_remaining == 0) {
        c->http_chunk_header_state = 2;
      }

      if (to_read > 0) {
        if (rwm_encrypt_decrypt_to (&c->in_u, &c->in, to_read, T->read_aeskey, 1) != to_read) {
          fail_connection (C, -1);
          return -1;
        }
        vkprintf (2, "HTTP stream: decrypted %d bytes, %d remaining in chunk\n", to_read, c->http_chunk_remaining);
      }
      continue;
    } else if (c->flags & C_IS_TLS) {
      assert (c->left_tls_packet_length >= 0);
      if (c->left_tls_packet_length == 0) {
        if (len < 5) {
          vkprintf (2, "Need %d more bytes to parse TLS header\n", 5 - len);
          return 5 - len;
        }

        unsigned char header[5];
        assert (rwm_fetch_lookup (&c->in_u, header, 5) == 5);
        if (memcmp (header, "\x17\x03\x03", 3) != 0) {
          vkprintf (1, "error while parsing packet: expect TLS header\n");
          fail_connection (C, -1);
          return 0;
        }
        c->left_tls_packet_length = 256 * header[3] + header[4];
        vkprintf (2, "Receive TLS-packet of length %d\n", c->left_tls_packet_length);
        assert (rwm_skip_data (&c->in_u, 5) == 5);
        len -= 5;
      }

      if (c->left_tls_packet_length < len) {
        len = c->left_tls_packet_length;
      }
      c->left_tls_packet_length -= len;
    }
    vkprintf (2, "Read %d bytes out of %d available\n", len, c->in_u.total_bytes);
    if (rwm_encrypt_decrypt_to (&c->in_u, &c->in, len, T->read_aeskey, 1) != len) {
      fail_connection (C, -1);
      return -1;
    }
  }

  return 0;
}
/* }}} */

int cpu_tcp_aes_crypto_ctr128_needed_output_bytes (connection_job_t C) /* {{{ */ {
  struct connection_info *c = CONN_INFO (C);
  assert (c->crypto);
  return 0;
}
/* }}} */
