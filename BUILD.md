# CloudDisk 本地编译与运行指南

一个基于 C++ 的云盘（网盘）系统。架构分三部分：

- **server** — HTTP 网关（wfrest，`:8888`），面向浏览器，转发到后端微服务
- **UserService** — 用户微服务（srpc，`:1414`），注册到 Consul，读写 MySQL
- **backup** — RabbitMQ 消费者，把上传的文件异步备份到阿里云 OSS

依赖中间件：**MySQL**、**Consul**、**RabbitMQ**。

> 本文档基于 Ubuntu 24.04 (x86_64, gcc 13, cmake 3.28) 实测通过。

---

## 1. 安装系统依赖（apt）

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential pkg-config cmake git wget curl unzip ca-certificates \
  libssl-dev zlib1g-dev \
  libprotobuf-dev protobuf-compiler \
  libboost-all-dev \
  librabbitmq-dev libcurl4-openssl-dev libjansson-dev \
  libsnappy-dev liblz4-dev libgtest-dev libjwt-dev \
  mysql-server rabbitmq-server
```

## 2. 从源码编译并安装第三方 C++ 库（装到 /usr/local）

这些库 apt 里没有，需要源码编译。存在依赖顺序：**workflow → srpc / wfrest**。

```bash
mkdir -p ~/deps && cd ~/deps

# nlohmann/json（单头文件）
sudo mkdir -p /usr/local/include/nlohmann
curl -sL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp \
  | sudo tee /usr/local/include/nlohmann/json.hpp >/dev/null

# Sogou workflow（基础库，其它都依赖它）
git clone --depth 1 https://github.com/sogou/workflow.git
cd workflow && make -j"$(nproc)" && sudo make install && cd ..

# Sogou srpc（含 srpc_generator 代码生成器）
git clone --depth 1 https://github.com/sogou/srpc.git
cd srpc && cmake -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j"$(nproc)" \
  && sudo make -C build install && cd ..

# wfrest（HTTP 框架）
git clone --depth 1 https://github.com/wfrest/wfrest.git
cd wfrest && cmake -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j"$(nproc)" \
  && sudo make -C build install && cd ..

# ppconsul（Consul 客户端，UserService 用）
git clone --depth 1 https://github.com/oliora/ppconsul.git
cd ppconsul && cmake -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j"$(nproc)" \
  && sudo make -C build install && cd ..

# SimpleAmqpClient（RabbitMQ C++ 封装）
git clone --depth 1 https://github.com/alanxz/SimpleAmqpClient.git
cd SimpleAmqpClient && cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  && make -C build -j"$(nproc)" && sudo make -C build install && cd ..

# 阿里云 OSS C++ SDK（backup 用；gcc13 需去掉 -Werror）
git clone --depth 1 https://github.com/aliyun/aliyun-oss-cpp-sdk.git
cd aliyun-oss-cpp-sdk
sed -i 's/"-Wall" "-Werror" "-pedantic" "-Wextra"/"-Wall"/g' CMakeLists.txt
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTS=OFF -DBUILD_SAMPLE=OFF
make -C build -j"$(nproc)" && sudo make -C build install && cd ..

sudo ldconfig
```

## 3. 重新生成 protobuf / srpc 代码（重要）

仓库里自带的 `UserService.pb.*` 是用旧版 protoc 生成的，和新版 protobuf 不兼容
（报错 `regenerate this file with a newer version of protoc`）。需重新生成：

```bash
cd /home/terminal/project/CloudDisk
protoc --cpp_out=. UserService.proto           # 生成 UserService.pb.h / .pb.cc
srpc_generator protobuf UserService.proto .    # 生成 UserService.srpc.h
mv -f UserService.pb.cc UserService.pb.cpp     # CMakeLists 里用的是 .cpp 扩展名
rm -f server.pb_skeleton.cc client.pb_skeleton.cc
```

## 4. 编译本项目

```bash
cd /home/terminal/project/CloudDisk
export LIBRARY_PATH=/usr/local/lib LD_LIBRARY_PATH=/usr/local/lib CPLUS_INCLUDE_PATH=/usr/local/include
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j"$(nproc)"
# 产物: ./server  ./UserService  ./backup （输出到项目根目录）
```

## 5. 初始化 MySQL

代码里连接串写死为 `mysql://root:1234@localhost/test`，因此需要 root 密码为 `1234`
且使用 `mysql_native_password`（workflow 的 MySQL 客户端对 `caching_sha2_password` 支持不佳）：

```bash
sudo mysql < scripts/init_db.sql
```

## 6. 启动与验证

一键启动（会拉起 MySQL/RabbitMQ、声明队列、启动 Consul/UserService/server）：

```bash
bash scripts/start.sh
```

手动逐个启动等价于：

```bash
export LD_LIBRARY_PATH=/usr/local/lib
consul agent -dev -client=127.0.0.1 &     # :8500
./UserService &                           # :1414, 注册到 Consul
./server &                                # :8888
```

浏览器打开 <http://127.0.0.1:8888/> 注册 → 登录 → 拖拽上传 / 下载。

命令行冒烟测试（新版 `/api` 接口，统一 JSON 响应，Bearer 鉴权）：

```bash
BASE=http://127.0.0.1:8888
curl -s -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
     -d '{"username":"alice","password":"secret123"}'
RESP=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
     -d '{"username":"alice","password":"secret123"}')
TOKEN=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"]["token"])')
echo hello > /tmp/hello.txt
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload" -F "f=@/tmp/hello.txt"
curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?limit=15"
```

完整回归测试：`bash tests/integration.sh`（29 项 API/边界/安全用例）。接口详情见 `docs/API.md`。

停止应用进程：`bash scripts/stop.sh`

---

## 已修复的问题

- **端口不一致**：`UserService.cpp` 中 SRPC 监听 `1414`，却向 Consul 注册 `1314`，网关连不上真实服务。已改为注册配置端口（默认 `1414`）。
- **SQL 注入**：注册/登录/上传/列表/删除/重命名/下载全部改为**转义 + 白名单**（见 `SqlUtil.h`），并有注入用例回归。
- **硬编码密钥**：阿里云 AK/SK、JWT 密钥、MySQL/RabbitMQ 口令全部移出源码，改由 `config.json` / 环境变量注入（见 `Config.h`、`scripts/oss.env.example`）。
- **明文密码日志**：移除 `#ifdef DEBUG` 下打印密码的逻辑。
- **前端全部重写**：去除失效的外部 CDN 依赖（百度 jQuery / jsdelivr），改为自研、无外链、支持明暗主题的现代界面；补齐网页上传（原项目上传页未接线）。
- **存储模型**：改为**内容寻址 Blob + 元数据表**，实现秒传去重与安全的引用计数删除。

## 说明

- **`backup` / 阿里云 OSS**：原 `OssManager.cpp` 中硬编码且已泄露的 AccessKey 已删除，改为从环境变量读取。配置 `scripts/oss.env` 后 `start.sh` 会自动拉起 `backup`；未配置则不启动。上传文件时消息已正常投递到 RabbitMQ `oss.queue`。
- **HTTP 明文**：当前为 HTTP，生产环境请置于 TLS 反向代理之后。
