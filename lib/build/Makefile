# libteleproto3 — POSIX Make build.
# Output: libteleproto3.a + consumers link against ../include/t3.h.
# TODO(lib-v0.1.0): flesh out; this is a placeholder that declares the
# final artefact names so CI + server/Makefile can already reference them.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic -fPIC
INCLUDES = -I../include

SRC_DIR = ../src
OBJ_DIR = .
SRCS    = \
    $(SRC_DIR)/t3_secret.c \
    $(SRC_DIR)/t3_obfuscated2.c \
    $(SRC_DIR)/t3_session_header.c \
    $(SRC_DIR)/t3_keyderiv.c \
    $(SRC_DIR)/t3_url_parser.c
OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
LIB     = libteleproto3.a

.PHONY: all clean test
all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

test:
	@echo "TODO(lib-v0.1.0): build + run lib/tests/unit/*.c and lib/tests/integration/*.c"
