/*
    DC latency probes — periodic TCP handshake measurement to Telegram DCs.

    Teleproxy is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "common/common-stats.h"

/* Initialize the DC probe subsystem and start the timer.
   interval_seconds > 0 enables probes; 0 disables.
   Call once from master process after the engine is set up. */
void dc_probes_init (int interval_seconds);

/* Append Prometheus histogram / counter / gauge lines to sb. */
void dc_probes_write_prometheus (stats_buffer_t *sb);

/* Append human-readable text stats to sb. */
void dc_probes_write_text_stats (stats_buffer_t *sb);
