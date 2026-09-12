# ---- 阶段 1：builder ----
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y \
    build-essential cmake git wget \
    libssl-dev zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 依赖：header-only 库
RUN wget -q -O /src/include/json.hpp \
    https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp || \
    (mkdir -p /src/include && wget -q -O /src/include/json.hpp \
    https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp)

# 依赖：uWebSockets
RUN git clone --recurse-submodules --depth 1 \
    https://github.com/uNetworking/uWebSockets.git /root/uWebSockets && \
    cd /root/uWebSockets && make && make install && \
    cd uSockets && ar rcs libuSockets.a *.o && \
    cp libuSockets.a /usr/lib/ && ldconfig

# 拷贝项目并编译
COPY . /src
RUN mkdir -p /src/include && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ---- 阶段 2：runtime ----
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt update && apt install -y \
    libssl3 zlib1g can-utils iproute2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/dashboard       /app/dashboard
COPY --from=builder /src/build/can_producer    /app/can_producer
COPY --from=builder /src/www                   /app/www
COPY --from=builder /src/scripts               /app/scripts

EXPOSE 8080
CMD ["/app/dashboard", "--can", "vcan0"]
