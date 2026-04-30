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
*/

#pragma once

#include "net/net-connections.h"

extern conn_type_t ct_direct_dc;
extern conn_type_t ct_direct_client;
extern conn_type_t ct_direct_client_drs;

int direct_connect_to_dc (connection_job_t C, int target_dc);
void direct_retry_dc_connection (connection_job_t C);

/* Cross-module helpers: defined in net-tcp-rpc-ext-server.c,
   called from net-tcp-direct-dc.c */
int secret_over_quota (int secret_id);
void ip_track_disconnect_impl (int secret_id, unsigned ip, const unsigned char *ipv6);
void rate_limit_after_relay (connection_job_t C, int secret_id,
                             long long bytes, unsigned ip,
                             const unsigned char *ipv6);
int rate_limit_resume (connection_job_t C);
int tcp_rpcs_ext_drs_alarm (connection_job_t c);
int tcp_proxy_pass_write_packet (connection_job_t c, struct raw_message *raw);
