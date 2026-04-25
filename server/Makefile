# server/Makefile — wraps the upstream MTProxy Makefile and links
# fork-local Type3 dispatch against ../lib/build/libteleproto3.a.
#
# TODO(server-v0.1.0): fold into the upstream Makefile post-subtree-pull.
# For now this is a thin placeholder so CI + docs can reference a
# canonical entry point.

.PHONY: all lib server clean

all: lib server

lib:
	$(MAKE) -C ../lib/build

server:
	@echo "TODO(server-v0.1.0): invoke upstream MTProxy Makefile here,"
	@echo "adding -I../lib/include and -L../lib/build -lteleproto3."
	@echo "Upstream Makefile lands with the first subtree pull (see UPSTREAM.md)."

clean:
	$(MAKE) -C ../lib/build clean
