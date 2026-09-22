# C++-сервер Aura (CMake + libpq)
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-go libpq-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
# vendored libargon2 — без него не собирается argon2 (см. server/CMakeLists.txt)
COPY third_party ./third_party

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DAURA_BUILD_TESTS=OFF \
    && cmake --build build

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libpq5 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/aura-server /usr/local/bin/aura-server

EXPOSE 9000
ENTRYPOINT ["/usr/local/bin/aura-server"]
