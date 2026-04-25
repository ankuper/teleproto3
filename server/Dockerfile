# Dockerfile — teleproto3 server image, published to GHCR as
# ghcr.io/ankuper/teleproto3-server:<server-vX.Y.Z>.
# Two-stage build: compile libteleproto3 + mtproto-proxy, then copy
# into a minimal runtime image.
# TODO(server-v0.1.0): wire into CI (.github/workflows/build-server.yml)
# with provenance attestation.

FROM debian:stable-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY ../lib /src/lib
COPY .     /src/server
RUN make -C /src/lib/build \
 && make -C /src/server

FROM debian:stable-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/server/objs/bin/mtproto-proxy /usr/local/bin/mtproto-proxy

EXPOSE 3129
ENTRYPOINT ["/usr/local/bin/mtproto-proxy"]
