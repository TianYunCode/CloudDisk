# =============================================================================
# CloudVault 企业云盘 — 多阶段构建镜像
#   builder 阶段: 从源码编译 Sogou C++ Workflow / srpc / wfrest / 依赖 + 本项目
#   runtime 阶段: 仅拷贝三个可执行文件 + 运行期动态库 + 静态资源
#
# 说明: 本 Dockerfile 在无 Docker 的开发机上按惯例编写, 未在本机实测构建。
#       各上游依赖使用其默认分支; 如遇 API 漂移可锁定到具体 tag。
# =============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config \
        libssl-dev zlib1g-dev libgtest-dev \
        protobuf-compiler libprotobuf-dev \
        liblz4-dev libsnappy-dev \
        librabbitmq-dev libboost-all-dev \
        libjansson-dev libcurl4-openssl-dev libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /deps

# ---- Sogou C++ Workflow ----
RUN git clone --depth 1 https://github.com/sogou/workflow.git && \
    cmake -S workflow -B workflow/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build workflow/build -j"$(nproc)" && \
    cmake --install workflow/build && ldconfig

# ---- srpc ----
RUN git clone --depth 1 https://github.com/sogou/srpc.git && \
    cmake -S srpc -B srpc/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build srpc/build -j"$(nproc)" && \
    cmake --install srpc/build && ldconfig

# ---- wfrest ----
RUN git clone --depth 1 https://github.com/wfrest/wfrest.git && \
    cmake -S wfrest -B wfrest/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build wfrest/build -j"$(nproc)" && \
    cmake --install wfrest/build && ldconfig

# ---- nlohmann/json (header-only) ----
RUN git clone --depth 1 https://github.com/nlohmann/json.git && \
    cp -r json/single_include/nlohmann /usr/local/include/

# ---- SimpleAmqpClient (依赖 rabbitmq-c) ----
RUN git clone --depth 1 https://github.com/alanxz/SimpleAmqpClient.git && \
    cmake -S SimpleAmqpClient -B SimpleAmqpClient/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build SimpleAmqpClient/build -j"$(nproc)" && \
    cmake --install SimpleAmqpClient/build && ldconfig

# ---- libjwt: 使用发行版软件包 (2.x 系列, 含 CryptoUtil 依赖的经典 API) ----
# (通过上面的 apt libjwt-dev 提供, 无需从源码构建)

# ---- ppconsul (Consul C++ 客户端) ----
RUN git clone --depth 1 --recursive https://github.com/oliora/ppconsul.git && \
    cmake -S ppconsul -B ppconsul/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build ppconsul/build -j"$(nproc)" && \
    cmake --install ppconsul/build && ldconfig

# ---- 阿里云 OSS C++ SDK (backup 目标使用) ----
# 注: 该 SDK 默认以 -Werror 编译, 在新版 curl(8.x) 上会因 deprecated 声明而失败;
#     去掉 -Werror 并显式关闭 deprecated 警告为错误。
RUN git clone --depth 1 https://github.com/aliyun/aliyun-oss-cpp-sdk.git && \
    grep -rlZ -- '-Werror' aliyun-oss-cpp-sdk --include=CMakeLists.txt 2>/dev/null | xargs -0 -r sed -i 's/-Werror//g' && \
    cmake -S aliyun-oss-cpp-sdk -B aliyun-oss-cpp-sdk/build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_FLAGS="-Wno-error=deprecated-declarations -Wno-deprecated-declarations" \
          -DCMAKE_CXX_FLAGS="-Wno-error=deprecated-declarations -Wno-deprecated-declarations" && \
    cmake --build aliyun-oss-cpp-sdk/build -j"$(nproc)" && \
    cmake --install aliyun-oss-cpp-sdk/build && ldconfig

# ---- 构建本项目 ----
WORKDIR /app
# libjwt 使用发行版软件包 (含 CryptoUtil 依赖的经典 API); 单独一层以复用上面的依赖缓存
RUN apt-get update && apt-get install -y --no-install-recommends libjwt-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /app
ENV LIBRARY_PATH=/usr/local/lib \
    LD_LIBRARY_PATH=/usr/local/lib \
    CPLUS_INCLUDE_PATH=/usr/local/include
RUN rm -rf build && mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j"$(nproc)" server UserService backup

# =============================================================================
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 zlib1g libprotobuf32t64 liblz4-1 libsnappy1v5 \
        librabbitmq4 libjansson4 libcurl4 libmysqlclient21 libjwt2 \
        libboost-chrono1.83.0 libboost-system1.83.0 \
        ca-certificates netcat-openbsd default-mysql-client \
    && rm -rf /var/lib/apt/lists/*

# 运行期需要 /usr/local/lib 下的自建动态库
COPY --from=builder /usr/local/lib /usr/local/lib
RUN ldconfig

WORKDIR /app
# 三个可执行文件生成在源码根 (CMAKE_RUNTIME_OUTPUT_DIRECTORY=../)
COPY --from=builder /app/server /app/UserService /app/backup /app/
COPY --from=builder /app/static /app/static
COPY --from=builder /app/scripts /app/scripts
COPY --from=builder /app/config.json /app/config.json
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

ENV LD_LIBRARY_PATH=/usr/local/lib
EXPOSE 8888
ENTRYPOINT ["/app/entrypoint.sh"]
