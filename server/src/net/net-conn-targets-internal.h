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

#include "jobs/jobs.h"
#include "net/net-connections.h"
#include "common/common-stats.h"

#ifndef MODULE
#define MODULE connections
#endif

MODULE_STAT_TYPE {
  int active_connections, active_dh_connections;
  int outbound_connections, active_outbound_connections, ready_outbound_connections, listening_connections;
  int allocated_outbound_connections, allocated_inbound_connections;
  int inbound_connections, active_inbound_connections;

  long long outbound_connections_created, inbound_connections_accepted;
  int ready_targets;

  long long netw_queries, netw_update_queries, total_failed_connections, total_connect_failures, unused_connections_closed;

  int allocated_targets, active_targets, inactive_targets, free_targets;
  int allocated_connections, allocated_socket_connections;
  long long accept_calls_failed, accept_nonblock_set_failed, accept_connection_limit_failed,
            accept_rate_limit_failed, accept_init_accepted_failed, accept_ip_acl_rejected;

  long long tcp_readv_calls, tcp_writev_calls, tcp_readv_intr, tcp_writev_intr;
  long long tcp_readv_bytes, tcp_writev_bytes;

  int free_later_size;
  long long free_later_total;
};

#define MAX_RECONNECT_INTERVAL 20

static inline int connection_is_active (int flags) {
  return (flags & C_CONNECTED) && !(flags & C_READY_PENDING);
}
