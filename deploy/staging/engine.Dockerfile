# syntax=docker/dockerfile:1.7
# vyuha_engine (release). Build context: repo root (make staging-build).
# Toolchain matches CI/Linux dev: GCC 14, vcpkg at engine/vcpkg.json's builtin-baseline, static ports,
# release-only triplets (engine.yml's ENGINE_CMAKE_ARGS): half the cold-build time, same dependency builds.
# Runs with network_mode: host so sessions can bind each account's static IP (ARCHITECTURE §5).

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS build
ENV DEBIAN_FRONTEND=noninteractive CC=gcc-14 CXX=g++-14 VCPKG_ROOT=/opt/vcpkg VCPKG_DISABLE_METRICS=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-14 make cmake ninja-build pkg-config git curl ca-certificates zip unzip tar python3 \
      perl autoconf automake autoconf-archive libtool bison flex linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY engine/vcpkg.json engine/vcpkg-configuration.json engine/
COPY engine/vcpkg-overlays engine/vcpkg-overlays
RUN baseline="$(python3 -c 'import json; print(json.load(open("engine/vcpkg.json"))["builtin-baseline"])')" \
    && git init -q "$VCPKG_ROOT" && git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git \
    && git -C "$VCPKG_ROOT" fetch -q --depth 1 origin "$baseline" && git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
# Dependencies in their own layer (the long part: gRPC/abseil/protobuf), reused until the manifest changes.
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    "$VCPKG_ROOT/vcpkg" install --x-manifest-root=engine --x-install-root=/opt/vcpkg_installed \
      --triplet x64-linux-release --host-triplet x64-linux-release --clean-after-build
COPY proto proto
COPY engine engine
RUN cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
      -DVCPKG_TARGET_TRIPLET=x64-linux-release -DVCPKG_HOST_TRIPLET=x64-linux-release \
      -DVYUHA_BUILD_TESTS=OFF -DVYUHA_BUILD_PYTHON=OFF \
    && cmake --build build --target vyuha_engine

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/* && useradd --system --uid 10001 --no-create-home vyuha
COPY --from=build /src/build/vyuha_engine /usr/local/bin/vyuha_engine
USER vyuha
ENTRYPOINT ["/usr/local/bin/vyuha_engine"]
