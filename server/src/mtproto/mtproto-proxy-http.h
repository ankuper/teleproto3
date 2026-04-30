/*
    This file is part of Teleproxy.

    Teleproxy is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is released under the GPL with the additional exemption
    that compiling, linking, and/or using OpenSSL is allowed.
    You are free to remove this exemption from derived works.
*/

#pragma once

#include "net/net-http-server.h"
#include "net/net-tcp-rpc-server.h"

extern struct http_server_functions http_methods;
extern struct http_server_functions http_methods_stats;
extern struct tcp_rpc_server_functions ext_rpc_methods;

int client_send_message (JOB_REF_ARG(C), long long in_conn_id, struct tl_in_state *tlio_in, int flags);
int hts_execute (connection_job_t C, struct raw_message *msg, int op);
int hts_stats_execute (connection_job_t C, struct raw_message *msg, int op);
int mtproto_http_alarm (connection_job_t C);
int mtproto_http_close (connection_job_t C, int who);

unsigned parse_text_ipv4 (char *str);
int parse_text_ipv6 (unsigned char ip[16], const char *str);
