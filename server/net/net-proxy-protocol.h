/*
    PROXY protocol v1/v2 parser for teleproxy.

    Parses HAProxy PROXY protocol headers (RFC-style, haproxy.org/download/2.9/doc/proxy-protocol.txt)
    to extract the real client IP when running behind a load balancer.

    Teleproxy is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <sys/socket.h>
#include "net/net-msg.h"

struct proxy_protocol_result {
  int family;                  /* AF_INET, AF_INET6, or 0 (UNKNOWN/LOCAL) */
  unsigned src_ip;             /* IPv4 source, host byte order (0 if IPv6 or unknown) */
  unsigned char src_ipv6[16];  /* IPv6 source (zeroed if IPv4 or unknown) */
  unsigned short src_port;     /* source port, host byte order */
};

/* Parse a PROXY protocol v1 or v2 header from a raw message buffer.
   Returns:
     >0  bytes consumed (header fully parsed and removed from buffer)
      0  need more data (caller should return NEED_MORE_BYTES)
     -1  parse error (caller should close connection) */
int proxy_protocol_parse (struct raw_message *in, struct proxy_protocol_result *out);

/* Global configuration flag: when set, inbound client connections
   expect a PROXY protocol header before any protocol data. */
extern int proxy_protocol_enabled;

/* Counters for stats/prometheus. Updated from the parse_execute thread. */
extern long long proxy_protocol_connections_total;
extern long long proxy_protocol_errors_total;
