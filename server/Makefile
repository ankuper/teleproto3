# server/Makefile — wraps the upstream MTProxy Makefile and links
# fork-local Type3 dispatch against ../lib/build/libteleproto3.a.
#
# Task 2.4: wire net-type3-dispatch.o, net-type3-stats.o into upstream build.
# Task 3.6: pass --stats-port default (8889) into binary via CFLAGS.

.PHONY: all lib server server-baseline clean

UPSTREAM_DIR := .
FORK_SRCS    := net/net-type3-dispatch.c net/net-type3-stats.c
FORK_OBJS    := $(FORK_SRCS:.c=.o)
LIB_DIR      := ../lib/build
LIB_INC      := ../lib/include

CC      := cc
CFLAGS  := -O2 -std=gnu11 -Wall -Wextra -Wpedantic \
            -I$(LIB_INC) -I. \
            -DTELEPROTO3_DISPATCH_HOOK=1 \
            -DT3_STATS_DEFAULT_PORT=8889

LDFLAGS := -L$(LIB_DIR) -lteleproto3

# Baseline build: TELEPROTO3_DISPATCH_HOOK=0 → upstream-identical binary
CFLAGS_BASELINE := $(filter-out -DTELEPROTO3_DISPATCH_HOOK=%,$(CFLAGS)) \
                   -DTELEPROTO3_DISPATCH_HOOK=0

all: lib server

lib:
	$(MAKE) -C $(LIB_DIR)

# Fork-local translation units
net/net-type3-dispatch.o: net/net-type3-dispatch.c net/net-type3-dispatch.h net/net-type3-stats.h
	$(CC) $(CFLAGS) -c $< -o $@

net/net-type3-stats.o: net/net-type3-stats.c net/net-type3-stats.h
	$(CC) $(CFLAGS) -c $< -o $@

# Upstream build: delegate to upstream Makefile adding fork-local objects and lib.
# When subtree pull brings the real Makefile, replace the echo block below with
# the real $(MAKE) invocation and EXTRA_OBJS / EXTRA_LDFLAGS variables.
server: lib $(FORK_OBJS)
	@echo "INFO: linking fork-local objects with upstream MTProxy"
	@echo "      FORK_OBJS=$(FORK_OBJS)"
	@echo "      LDFLAGS=$(LDFLAGS)"
	@echo "TODO(server-v0.1.0): invoke upstream MTProxy Makefile here once"
	@echo "  the subtree pull lands the full upstream Makefile (see UPSTREAM.md)."

# Upstream-identical (baseline) build for additivity CI (AR-S15)
server-baseline: lib
	$(CC) $(CFLAGS_BASELINE) -c net/net-type3-dispatch.c -o net/net-type3-dispatch-baseline.o
	@echo "Baseline object built (TELEPROTO3_DISPATCH_HOOK=0); diff binary vs upstream."

clean:
	$(MAKE) -C $(LIB_DIR) clean
	rm -f $(FORK_OBJS) net/net-type3-dispatch-baseline.o
