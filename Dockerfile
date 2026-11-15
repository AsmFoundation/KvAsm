# syntax=docker/dockerfile:1.7
# =============================================================================
# KvNode container build.
#
# Stages:
#   toolchain  Debian trixie + NASM/GCC + nghttp2 (pinned, verified, built as a
#              static PIC library) + h2spec. nghttp2 is for the test and
#              benchmark CLIENTS only (itest_grpc, test_hpack's oracle,
#              kvbench): the server has its own HTTP/2 engine (ADR 0008).
#              Also used as the development and test container:
#              `docker build --target toolchain -t kvnode-toolchain .`
#   build      Assembles/links the server, runs the native unit tests and the
#              end-to-end suite (real server over loopback: gRPC protocol,
#              concurrency, shutdown, snapshots, h2spec baseline, fuzzing);
#              any failure fails the image build.
#   runtime    FROM scratch: the single static-PIE server binary (glibc
#              linked in, ~1 MB) and the license texts it must carry
#              (/usr/share/doc/kvnode, THIRD_PARTY_NOTICES.md), nothing else
#              — no shell, no package manager, no libraries to patch or
#              exploit. Runs as the unprivileged uid/gid 10001. To debug, use
#              the build stage (`docker build --target build`), which has the
#              same binary plus gdb/strace/nghttp/h2load.
#
# Every third-party download is pinned by version AND SHA-256, so a changed or
# tampered artifact breaks the build instead of silently entering the image.
# =============================================================================

ARG DEBIAN_RELEASE=trixie

FROM debian:${DEBIAN_RELEASE} AS toolchain
ARG NGHTTP2_VERSION=1.70.0
ARG NGHTTP2_SHA256=e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
ARG H2SPEC_VERSION=2.6.0
ARG H2SPEC_SHA256=157ee0de702e01ad40e752dbf074b366027e550c8e7504f9450da2809e279318
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      nasm gcc libc6-dev make cmake binutils ca-certificates curl xz-utils \
      nghttp2-client procps strace gdb \
 && rm -rf /var/lib/apt/lists/*
# nghttp2 is built library-only (C99, no C++ toolchain needed) and position
# independent (the test clients and kvbench are PIEs). It never reaches the
# server binary or the runtime image.
RUN curl -fsSL -o /tmp/nghttp2.tar.xz \
      "https://github.com/nghttp2/nghttp2/releases/download/v${NGHTTP2_VERSION}/nghttp2-${NGHTTP2_VERSION}.tar.xz" \
 && echo "${NGHTTP2_SHA256}  /tmp/nghttp2.tar.xz" | sha256sum -c - \
 && tar -xf /tmp/nghttp2.tar.xz -C /tmp \
 && cmake -S "/tmp/nghttp2-${NGHTTP2_VERSION}" -B /tmp/nghttp2-build \
      -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
      -DENABLE_DOC=OFF -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/nghttp2 \
 && cmake --build /tmp/nghttp2-build -j"$(nproc)" \
 && cmake --install /tmp/nghttp2-build \
 && rm -rf /tmp/nghttp2*
RUN curl -fsSL -o /tmp/h2spec.tgz \
      "https://github.com/summerwind/h2spec/releases/download/v${H2SPEC_VERSION}/h2spec_linux_amd64.tar.gz" \
 && echo "${H2SPEC_SHA256}  /tmp/h2spec.tgz" | sha256sum -c - \
 && tar -xzf /tmp/h2spec.tgz -C /usr/local/bin h2spec \
 && rm /tmp/h2spec.tgz

FROM toolchain AS build
WORKDIR /src
COPY server/ server/
# The shipped binary is the tested one: stripped only after every suite
# passed. --strip-debug keeps the symbol table, so perf/gdb backtraces from
# production still name functions; DWARF stays in the build stage.
RUN make -C server NGHTTP2_PREFIX=/opt/nghttp2 all test itest \
 && strip --strip-debug -o /kvnode server/build/kvnode
# The license texts the runtime image must carry for the third-party code
# linked INTO the binary (THIRD_PARTY_NOTICES.md): glibc (LGPL-2.1+) and
# libgcc (GPL-3.0+ with the GCC Runtime Library Exception). VERSIONS records
# the exact Debian packages linked, because apt installs whatever the archive
# holds at build time; it is what a source request under the LGPL names.
# Package names are derived, not hard-coded, so a new Debian release (another
# GCC major) still finds them; a missing file fails the build instead of
# shipping an image without its notices. A separate layer after the tested
# build, so editing this step never re-runs the suites.
RUN set -eu; \
    mkdir -p /licenses; \
    libgcc_pkg="$(dpkg -S "$(gcc -print-libgcc-file-name)" | cut -d: -f1)"; \
    gcc_major="$(gcc -dumpversion | cut -d. -f1)"; \
    dpkg-query -W -f='${binary:Package} ${source:Package} ${source:Version}\n' \
      libc6-dev "$libgcc_pkg" > /licenses/VERSIONS; \
    cp /usr/share/doc/libc6/copyright /licenses/glibc.copyright; \
    cp "/usr/share/doc/gcc-${gcc_major}-base/copyright" /licenses/gcc.copyright; \
    cp /usr/share/common-licenses/LGPL-2.1 /usr/share/common-licenses/GPL-3 /licenses/

# ---- benchmark client (benchmarks/): not part of the server image ----------
# kvbench shares the toolchain (and therefore the pinned nghttp2, its HTTP/2
# client library) with the server's tests, is unit-tested here, and ships as
# a static binary in its own
# scratch image. These stages come before `runtime` so that a plain
# `docker build .` still produces the server.
FROM toolchain AS kvbench-build
WORKDIR /src
COPY benchmarks/kvbench/ benchmarks/kvbench/
RUN make -C benchmarks/kvbench NGHTTP2_PREFIX=/opt/nghttp2 all test

FROM scratch AS kvbench
COPY --from=kvbench-build /src/benchmarks/kvbench/build/kvbench /usr/local/bin/kvbench
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/kvbench"]

FROM scratch AS runtime
# image.source is what links a GHCR package to its repository; the release
# workflow (.github/workflows/release.yml) adds version, revision and created
# from the tag being released, so a local build carries no stale version.
LABEL org.opencontainers.image.title="KvNode" \
      org.opencontainers.image.description="gRPC key-value server written in x86-64 assembly" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.source="https://github.com/Nucs/KvNode" \
      org.opencontainers.image.documentation="https://github.com/Nucs/KvNode#readme"
COPY --from=build /kvnode /usr/local/bin/kvnode
# Licenses travel with the binary (Apache-2.0 §4, LGPL-2.1 §6): KvNode's own
# LICENSE/NOTICE/THIRD_PARTY_NOTICES.md from the build context, and the
# glibc/libgcc texts plus the exact versions linked from the build stage.
# A few hundred KB of text the server never reads.
COPY LICENSE NOTICE THIRD_PARTY_NOTICES.md /usr/share/doc/kvnode/
COPY --from=build /licenses/ /usr/share/doc/kvnode/third-party/
# Numeric: scratch has no /etc/passwd, and the server needs no user database.
USER 10001:10001
EXPOSE 50051
# Snapshots are read-only inputs; mount them, e.g.
#   docker run -v ./data:/data:ro -e KVNODE_SNAPSHOT=/data/seed.kvs ...
# (deliberately no VOLUME instruction: the server never writes to disk, and a
# declared volume would leave an anonymous volume behind on every run)
# No HEALTHCHECK: the image has no gRPC client to run one. Orchestrators probe
# the standard grpc.health.v1 service directly (Kubernetes: grpc probe on
# port 50051); see docs/OPERATIONS.md.
ENTRYPOINT ["/usr/local/bin/kvnode"]
