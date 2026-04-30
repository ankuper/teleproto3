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

#include "net/net-obfs2-parse.h"

#include <string.h>
#include <openssl/evp.h>

#include "common/sha256.h"
#include "crypto/aesni256.h"
#include "net/net-tcp-rpc-common.h"

int obfs2_parse_header (unsigned char header[64],
                        const unsigned char (*secrets)[16],
                        int secret_cnt,
                        int rand_pad_only,
                        struct obfs2_parse_result *result) {
  unsigned char saved[64];
  memcpy (saved, header, 64);

  struct aes_key_data kd;
  int iterations = (secret_cnt > 0) ? secret_cnt : 1;

  for (int sid = 0; sid < iterations; sid++) {
    /* Derive read key: SHA256(header[8:40] + secret) or raw header[8:40] */
    if (secret_cnt > 0) {
      unsigned char k[48];
      memcpy (k, header + 8, 32);
      memcpy (k + 32, secrets[sid], 16);
      sha256 (k, 48, kd.read_key);
    } else {
      memcpy (kd.read_key, header + 8, 32);
    }
    memcpy (kd.read_iv, header + 40, 16);

    /* Derive write key: reversed bytes from header */
    for (int i = 0; i < 32; i++) {
      kd.write_key[i] = header[55 - i];
    }
    for (int i = 0; i < 16; i++) {
      kd.write_iv[i] = header[23 - i];
    }

    if (secret_cnt > 0) {
      unsigned char k[48];
      memcpy (k, kd.write_key, 32);
      memcpy (k + 32, secrets[sid], 16);
      sha256 (k, 48, kd.write_key);
    }

    /* AES-256-CTR decrypt the header to verify the tag */
    EVP_CIPHER_CTX *ctx = evp_cipher_ctx_init (EVP_aes_256_ctr (), kd.read_key, kd.read_iv, 1);
    evp_crypt (ctx, header, header, 64);
    EVP_CIPHER_CTX_free (ctx);

    unsigned tag = *(unsigned *)(header + 56);

    if (tag == OBFS2_TAG_PAD ||
        ((tag == OBFS2_TAG_MEDIUM || tag == OBFS2_TAG_COMPACT) && !rand_pad_only)) {
      result->tag = tag;
      result->dc = *(short *)(header + 60);
      result->secret_id = sid;
      result->keys = kd;
      return 0;
    }

    /* No match — restore header and try next secret */
    memcpy (header, saved, 64);
  }

  return -1;
}

int obfs2_parse_frame_length (int raw4, int flags, int max_packet_len,
                              struct obfs2_frame_result *result) {
  int packet_len = raw4;
  int packet_len_bytes = 4;
  int quickack = 0;

  if (flags & RPC_F_MEDIUM) {
    /* Transport error codes: negative small int */
    if (packet_len < 0 && packet_len > -1000) {
      return -1;
    }
    quickack = !!(packet_len & RPC_F_QUICKACK);
    packet_len &= ~RPC_F_QUICKACK;
  } else {
    /* Compact mode */
    if (packet_len & 0x80) {
      quickack = 1;
      packet_len &= ~0x80;
    }
    if ((packet_len & 0xff) == 0x7f) {
      packet_len = ((unsigned) packet_len >> 8);
      if (packet_len < 0x7f) {
        return -1;  /* overlong encoding */
      }
    } else {
      packet_len &= 0x7f;
      packet_len_bytes = 1;
    }
    packet_len <<= 2;
  }

  if (packet_len <= 0 || (packet_len & 0xc0000000)) {
    return -1;
  }
  if (!(flags & RPC_F_PAD) && (packet_len & 3)) {
    return -1;
  }
  if (max_packet_len > 0 && packet_len > max_packet_len) {
    return -1;
  }

  result->packet_len = packet_len;
  result->header_bytes = packet_len_bytes;
  result->quickack = quickack;
  return 0;
}
