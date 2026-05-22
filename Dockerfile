# syntax=docker/dockerfile:1

ARG UBUNTU_VERSION=24.04

FROM ubuntu:${UBUNTU_VERSION} AS build-deps

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang-20 \
      cmake \
      libboost-filesystem-dev \
      libboost-json-dev \
      libboost-log-dev \
      libboost-program-options-dev \
      libboost-system-dev \
      libboost-test-dev \
      libboost-thread-dev \
      libssl-dev \
    && rm -rf /var/lib/apt/lists/*

FROM build-deps AS ci

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      libclang-rt-20-dev \
      llvm-20 \
      openssl \
      python3 \
      python3-pip \
      valgrind \
    && rm -rf /var/lib/apt/lists/*

ARG FPS_COMPILER=gcc

ENV FPS_COMPILER=${FPS_COMPILER}
ENV FPS_JOBS=2

WORKDIR /workspaces

COPY . ./

RUN python3 -m pip install --break-system-packages --no-cache-dir \
      -r requirements-runtime.txt

CMD ["bash", "docker/ci-local.sh"]

FROM build-deps AS builder

ARG FPS_COMPILER=gcc

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tools/fps_linux_route.sh ./tools/fps_linux_route.sh
COPY tools/fps_carrier.py ./tools/fps_carrier.py

RUN set -eux; \
    case "$FPS_COMPILER" in \
      gcc) cxx=g++ ;; \
      clang) cxx=clang++-20 ;; \
      *) echo "FPS_COMPILER must be gcc or clang" >&2; exit 2 ;; \
    esac; \
    cmake -S . -B /build \
      -DCMAKE_BUILD_TYPE=Release \
      -DFPS_BUILD_TESTS=OFF \
      -DCMAKE_CXX_COMPILER="$cxx" \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /build -j "$(nproc)" \
    && cmake --install /build --prefix /opt/fps/usr/local

FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV FPS_ROLE=server
ENV FPS_CONFIG=/etc/fps/server.json

COPY requirements-runtime.txt /tmp/fps-requirements-runtime.txt

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates \
      iperf3 \
      iproute2 \
      libboost-atomic1.83.0 \
      libboost-chrono1.83.0t64 \
      libboost-container1.83.0 \
      libboost-filesystem1.83.0 \
      libboost-json1.83.0 \
      libboost-log1.83.0 \
      libboost-program-options1.83.0 \
      libboost-regex1.83.0 \
      libboost-system1.83.0 \
      libboost-thread1.83.0 \
      libssl3t64 \
      openssl \
      python3 \
      python3-pip \
      tini \
    && python3 -m pip install --break-system-packages --no-cache-dir \
      -r /tmp/fps-requirements-runtime.txt \
    && apt-get purge -y --auto-remove python3-pip \
    && rm -rf /var/lib/apt/lists/* \
      /tmp/fps-requirements-runtime.txt \
    && mkdir -p /etc/fps /var/lib/fps /var/log/fps /run/fps

COPY --from=builder /opt/fps/usr/local/ /usr/local/
COPY docker/fps-entrypoint.sh /usr/local/bin/fps-entrypoint.sh

RUN chmod 0755 /usr/local/bin/fps-entrypoint.sh

EXPOSE 8443/tcp

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/fps-entrypoint.sh"]
CMD ["run"]
