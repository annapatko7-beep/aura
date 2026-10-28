# C++-сервер Selfvoy (CMake + libpq + OpenSSL для SMTP)
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build libpq-dev libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
# vendored libargon2 — без него не собирается argon2 (см. server/CMakeLists.txt)
COPY third_party ./third_party

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSELFVOY_BUILD_TESTS=OFF \
    && cmake --build build

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libpq5 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/selfvoy-server /usr/local/bin/selfvoy-server

# Непривилегированный пользователь (порт 9000 > 1024, root не нужен).
RUN useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin selfvoy
USER 10001:10001

EXPOSE 9000
ENTRYPOINT ["/usr/local/bin/selfvoy-server"]
