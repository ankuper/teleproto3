/*
    This file is part of Teleproxy.

    Teleproxy is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Teleproxy is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Teleproxy.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "net/net-crypto-aes.h"

/* Transport tags */
#define OBFS2_TAG_PAD       0xdddddddd
#define OBFS2_TAG_MEDIUM    0xeeeeeeee
#define OBFS2_TAG_COMPACT   0xefefefef

struct obfs2_parse_result {
  unsigned tag;              /* matched transport tag */
  int dc;                    /* target DC (signed short from offset 60) */
  int secret_id;             /* matched secret index (0-based) */
  struct aes_key_data keys;  /* derived AES-256-CTR key material */
};

/* Parse and validate a 64-byte obfuscated2 handshake header.
   Tries each secret in turn: derives AES keys, decrypts header,
   checks the transport tag at offset 56.

   header[] is decrypted in-place on success, restored on failure.

   secrets:        array of 16-byte secrets (NULL when secret_cnt == 0)
   secret_cnt:     number of secrets (0 = no-secret mode)
   rand_pad_only:  if set, only accept OBFS2_TAG_PAD (0xdddddddd)

   Returns 0 on success (result populated), -1 if no valid tag found. */
int obfs2_parse_header (unsigned char header[64],
                        const unsigned char (*secrets)[16],
                        int secret_cnt,
                        int rand_pad_only,
                        struct obfs2_parse_result *result);

/* Frame length parsing result */
struct obfs2_frame_result {
  int packet_len;        /* parsed packet length in bytes (always > 0) */
  int header_bytes;      /* number of header bytes consumed (1 or 4) */
  int quickack;          /* 1 if QUICKACK flag was set */
};

/* Parse a frame length from the first 4 bytes of a decrypted obfs2 stream.
   raw4:   first 4 bytes read from the stream (little-endian int)
   flags:  RPC_F_MEDIUM / RPC_F_PAD bits from the handshake tag
   max_packet_len: upper bound (0 = no limit)

   Returns 0 on success (result populated), -1 on validation error. */
int obfs2_parse_frame_length (int raw4, int flags, int max_packet_len,
                              struct obfs2_frame_result *result);
