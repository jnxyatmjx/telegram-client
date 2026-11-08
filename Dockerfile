# Multi-stage Dockerfile for tg-client (skeleton).
# NOTE (chunk 1): documents the target image layout; the TDLib build stage is
# exercised by a later chunk. TDLib is memory-hungry: build with -j1/-j2.
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git gperf php-cli ccache pkg-config \
      libssl-dev zlib1g-dev \
      libgrpc++-dev protobuf-compiler protobuf-compiler-grpc \
      libspdlog-dev libsqlite3-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
# third_party/td is cloned outside the image (kept out of the build context).
RUN git clone --depth 1 https://github.com/tdlib/td /src/third_party/td

# TDLib first Release build takes 20-60 min on this class of host.
RUN cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /src/build --parallel 2

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgrpc++1 libprotobuf32 libspdlog1.12 libsqlite3-0 libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/tg-client /usr/local/bin/tg-client
VOLUME /data
ENTRYPOINT ["/usr/local/bin/tg-client"]
