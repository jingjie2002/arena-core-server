FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    set -eux; \
    rm -f /etc/apt/apt.conf.d/docker-clean; \
    for attempt in 1 2 3 4 5 6 7 8 9 10; do \
      rm -rf /var/lib/apt/lists/*; \
      if apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 update; then \
        dpkg --configure -a || true; \
        apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 -f install -y || true; \
        if apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 --fix-missing install -y --no-install-recommends \
          ca-certificates \
          cmake \
          g++ \
          libboost-system-dev \
          libboost-thread-dev \
          libgtest-dev \
          libhiredis-dev \
          liblua5.4-dev \
          libmysqlcppconn-dev \
          libprotobuf-dev \
          libspdlog-dev \
          ninja-build \
          nlohmann-json3-dev \
          pkg-config \
          protobuf-compiler; then \
          exit 0; \
        fi; \
      fi; \
      sleep 5; \
    done; \
    exit 1

WORKDIR /src
COPY . .
RUN cmake --preset linux-gcc \
    && cmake --build --preset linux-gcc \
    && ctest --preset linux-gcc \
    && cmake --install out/build/linux-gcc --prefix /opt/arena-core

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /opt/arena-core
COPY --from=build /opt/arena-core/ /opt/arena-core/
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    set -eux; \
    rm -f /etc/apt/apt.conf.d/docker-clean; \
    for attempt in 1 2 3 4 5 6 7 8 9 10; do \
      rm -rf /var/lib/apt/lists/*; \
      if apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 update; then \
        dpkg --configure -a || true; \
        apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 -f install -y || true; \
        if apt-get -o Acquire::Retries=10 -o Acquire::http::Pipeline-Depth=0 --fix-missing install -y --no-install-recommends \
          ca-certificates \
          libfmt9 \
          libhiredis1.1.0 \
          liblua5.4-0 \
          libmysqlclient21 \
          libmysqlcppconn7t64 \
          libprotobuf32t64 \
          libspdlog1.12 \
          libssl3t64 \
          libzstd1 \
          zlib1g; then \
          useradd --system --uid 10001 --create-home --home-dir /home/arena arena; \
          exit 0; \
        fi; \
      fi; \
      sleep 5; \
    done; \
    exit 1

RUN mkdir -p /opt/arena-core/.runtime \
    && chown -R arena:arena /opt/arena-core/.runtime

USER arena
EXPOSE 9100 8080 8081 9200
ENTRYPOINT ["/opt/arena-core/bin/arena-core-server"]
CMD ["--config", "/opt/arena-core/.runtime/arena-core.json"]
