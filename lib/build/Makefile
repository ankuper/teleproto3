# DEPRECATED 2026-05-03 — see ../CMakeLists.txt; retained one release for downstream consumers mid-migration.
# libteleproto3 — POSIX Make build.
#
# This is the reference implementation of the Type3 protocol.
# Normative behaviour is defined in spec/. Where they differ, spec/ wins.
#
# Targets: all (default), test, baseline, clean
# Source: story 1.7 Task 9.

CC          ?= cc
AR          ?= ar
CFLAGS      ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror -fPIC -fvisibility=hidden
INCLUDES     = -I../include -I../include/internal

SRC_DIR      = ../src
OBJ_DIR      = .
TEST_DIR     = ../tests/unit

SRCS         = \
    $(SRC_DIR)/t3_secret.c \
    $(SRC_DIR)/t3_obfuscated2.c \
    $(SRC_DIR)/t3_session_header.c \
    $(SRC_DIR)/t3_keyderiv.c \
    $(SRC_DIR)/t3_url_parser.c \
    $(SRC_DIR)/t3_strerror.c \
    $(SRC_DIR)/t3_abi_version.c \
    $(SRC_DIR)/t3_timing.c \
    $(SRC_DIR)/t3_retry.c

OBJS         = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
LIB          = libteleproto3.a

TEST_CFLAGS  = -std=gnu11 -O1 -g -Wall -Wextra \
               -fsanitize=address,undefined -fno-omit-frame-pointer \
               -fvisibility=hidden
TEST_BINS    = $(OBJ_DIR)/test_secret \
               $(OBJ_DIR)/test_session_header \
               $(OBJ_DIR)/test_retry \
               $(OBJ_DIR)/test_url_parser \
               $(OBJ_DIR)/test_obfuscated2 \
               $(OBJ_DIR)/test_abi_surface

BASELINE_BIN = $(OBJ_DIR)/test_timing_baseline
BASELINE_OUT ?= timing-baseline.yaml

VECTORS      = ../../conformance/vectors/unit.json

.PHONY: all clean test baseline

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/test_secret: $(TEST_DIR)/test_secret.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/test_session_header: $(TEST_DIR)/test_session_header.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/test_retry: $(TEST_DIR)/test_retry.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/test_url_parser: $(TEST_DIR)/test_url_parser.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/test_obfuscated2: $(TEST_DIR)/test_obfuscated2.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(OBJ_DIR)/test_abi_surface: $(TEST_DIR)/test_abi_surface.c $(LIB)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $< $(LIB) -o $@

$(BASELINE_BIN): $(TEST_DIR)/test_timing.c $(LIB)
	$(CC) -std=gnu11 -O2 -DNDEBUG -Wall -Wextra \
	    -fvisibility=hidden $(INCLUDES) $< $(LIB) -lm -o $@

test: $(TEST_BINS) $(LIB)
	@echo "--- test_secret ---"
	$(OBJ_DIR)/test_secret $(VECTORS)
	@echo "--- test_session_header ---"
	$(OBJ_DIR)/test_session_header $(VECTORS)
	@echo "--- test_retry ---"
	$(OBJ_DIR)/test_retry
	@echo "--- test_url_parser ---"
	$(OBJ_DIR)/test_url_parser $(VECTORS)
	@echo "--- test_obfuscated2 ---"
	$(OBJ_DIR)/test_obfuscated2
	@echo "--- test_abi_surface ---"
	$(OBJ_DIR)/test_abi_surface $(LIB)
	@echo "All unit tests passed."

baseline: $(BASELINE_BIN)
	$(BASELINE_BIN) --emit-baseline $(BASELINE_OUT)
	@echo "Baseline written to $(BASELINE_OUT)"

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BINS) $(BASELINE_BIN) $(BASELINE_OUT)
	rm -f $(OBJ_DIR)/header_fuzz_crash $(OBJ_DIR)/header_fuzz_timing \
	      $(OBJ_DIR)/secret_fuzz_crash $(OBJ_DIR)/secret_fuzz_timing \
	      $(OBJ_DIR)/side_channel_crash.o $(OBJ_DIR)/side_channel_timing.o

# ── Fuzz harnesses (story 1.10) ─────────────────────────────────────────────
# Archive name: libteleproto3.a — never liblibteleproto3.a (§14 anti-pattern #11)
FUZZ_DIR     = ../fuzz
FUZZ_CORPUS  = ../fuzz/corpus/header_byteorder

# Compiler flags for crash mode: full sanitizer stack
FUZZ_CRASH_CFLAGS = -std=gnu11 -O1 -g -fvisibility=hidden \
                    -fsanitize=fuzzer,address,undefined \
                    -fno-omit-frame-pointer

# Compiler flags for timing mode: no address/UB sanitizers (perturb timing)
FUZZ_TIMING_CFLAGS = -std=gnu11 -O2 -DNDEBUG -fvisibility=hidden \
                     -fno-sanitize=address,undefined \
                     -DT3_FUZZ_TIMING

FUZZ_LINK_FLAGS = -fsanitize=fuzzer

FUZZ_SRCS_CRASH  = $(FUZZ_DIR)/side_channel.c
FUZZ_SRCS_TIMING = $(FUZZ_DIR)/side_channel.c

.PHONY: fuzz-crash fuzz-timing fuzz-lint

# side_channel.o for crash mode (has sanitizers)
$(OBJ_DIR)/side_channel_crash.o: $(FUZZ_DIR)/side_channel.c $(FUZZ_DIR)/side_channel.h
	$(CC) $(FUZZ_CRASH_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) -c $< -o $@

# side_channel.o for timing mode (no sanitizers)
$(OBJ_DIR)/side_channel_timing.o: $(FUZZ_DIR)/side_channel.c $(FUZZ_DIR)/side_channel.h
	$(CC) $(FUZZ_TIMING_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) -c $< -o $@

$(OBJ_DIR)/header_fuzz_crash: $(FUZZ_DIR)/header_fuzz.c $(OBJ_DIR)/side_channel_crash.o $(LIB)
	$(CC) $(FUZZ_CRASH_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) \
	    $< $(OBJ_DIR)/side_channel_crash.o $(LIB) \
	    $(FUZZ_LINK_FLAGS) -o $@

$(OBJ_DIR)/header_fuzz_timing: $(FUZZ_DIR)/header_fuzz.c $(OBJ_DIR)/side_channel_timing.o $(LIB)
	$(CC) $(FUZZ_TIMING_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) \
	    $< $(OBJ_DIR)/side_channel_timing.o $(LIB) \
	    $(FUZZ_LINK_FLAGS) -o $@

$(OBJ_DIR)/secret_fuzz_crash: $(FUZZ_DIR)/secret_fuzz.c $(OBJ_DIR)/side_channel_crash.o $(LIB)
	$(CC) $(FUZZ_CRASH_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) \
	    $< $(OBJ_DIR)/side_channel_crash.o $(LIB) \
	    $(FUZZ_LINK_FLAGS) -o $@

$(OBJ_DIR)/secret_fuzz_timing: $(FUZZ_DIR)/secret_fuzz.c $(OBJ_DIR)/side_channel_timing.o $(LIB)
	$(CC) $(FUZZ_TIMING_CFLAGS) $(INCLUDES) -I$(FUZZ_DIR) \
	    $< $(OBJ_DIR)/side_channel_timing.o $(LIB) \
	    $(FUZZ_LINK_FLAGS) -o $@

# fuzz-crash: build all crash-mode binaries + run no-rdtsc lint
fuzz-crash: $(OBJ_DIR)/header_fuzz_crash $(OBJ_DIR)/secret_fuzz_crash fuzz-lint
	@echo "Fuzz crash-mode binaries built."
	@echo "Run manually: $(OBJ_DIR)/header_fuzz_crash $(FUZZ_CORPUS) -fork=1 -max_total_time=300"

# fuzz-timing: build all timing-mode binaries + run no-rdtsc lint
fuzz-timing: $(OBJ_DIR)/header_fuzz_timing $(OBJ_DIR)/secret_fuzz_timing fuzz-lint
	@echo "Fuzz timing-mode binaries built."
	@echo "Run manually on Linux: setarch \$$(uname -m) -R taskset -c 2 nice -10"
	@echo "  $(OBJ_DIR)/header_fuzz_timing $(FUZZ_CORPUS) -fork=1 -max_total_time=300"

# fuzz-lint: assert no banned clock tokens in lib/fuzz/
fuzz-lint:
	@bash $(FUZZ_DIR)/check-no-rdtsc.sh
