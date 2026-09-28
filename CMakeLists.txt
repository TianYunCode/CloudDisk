# 设置CMake的最小版本
cmake_minimum_required(VERSION 3.15)

# 项目名称
#   项目可以包含多个目标(target)
#   target: 可执行程序, 静态库, 动态库
project(CloudDisk)

# 指定C++的版本: -std=c++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 可执行程序输出到 bin/ (与源码目录 server/ 分离; 运行时仍从仓库根目录启动)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 分层源码目录的头文件搜索路径 (使裸 #include "X.h" 跨层解析; 根目录用于 third_party/*)
set(CLOUDDISK_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/server/gateway
    ${CMAKE_SOURCE_DIR}/server/common
    ${CMAKE_SOURCE_DIR}/server/config
    ${CMAKE_SOURCE_DIR}/server/util
    ${CMAKE_SOURCE_DIR}/server/infra
    ${CMAKE_SOURCE_DIR}/server/rpc
)

# =============================== server (HTTP 网关) ==========================
add_executable(server
    server/gateway/CloudiskServer.cpp
    server/gateway/GatewaySupport.cpp
    server/infra/BlobStore.cpp
    server/gateway/handlers/AuthHandlers.cpp
    server/gateway/handlers/FileHandlers.cpp
    server/gateway/handlers/TrashHandlers.cpp
    server/gateway/handlers/ShareHandlers.cpp
    server/gateway/handlers/AdminHandlers.cpp
    server/gateway/handlers/WebdavHandlers.cpp
    server/gateway/handlers/ExtraHandlers.cpp
    server/gateway/main.cpp
    server/util/CryptoUtil.cpp
    server/util/Thumbnailer.cpp
    server/util/Totp.cpp
    server/util/FileType.cpp
    server/util/RateLimiter.cpp
    server/util/JsonUtil.cpp
    server/rpc/UserService.pb.cpp
)
target_include_directories(server PRIVATE ${CLOUDDISK_INCLUDE_DIRS})
target_compile_definitions(server PRIVATE DEBUG)
target_compile_options(server PRIVATE -g)
target_link_libraries(server PRIVATE
    wfrest
    workflow
    SimpleAmqpClient
    srpc
    lz4
    snappy
    protobuf
    jwt
    ssl
    crypto
)

# =============================== backup (OSS 异步备份消费者) ==================
add_executable(backup
    server/infra/OssManager.cpp
    server/backup-service/backup.cpp
)
target_include_directories(backup PRIVATE ${CLOUDDISK_INCLUDE_DIRS})
target_compile_definitions(backup PRIVATE DEBUG)
target_compile_options(backup PRIVATE -g -fno-rtti)
target_link_libraries(backup PRIVATE
    SimpleAmqpClient
    alibabacloud-oss-cpp-sdk
    curl
    crypto
    pthread
)

# =============================== UserService (srpc 微服务) ===================
add_executable(UserService
    server/user-service/UserService.cpp
    server/rpc/UserService.pb.cpp
    server/util/CryptoUtil.cpp
)
target_include_directories(UserService PRIVATE ${CLOUDDISK_INCLUDE_DIRS})
target_compile_definitions(UserService PRIVATE DEBUG)
target_compile_options(UserService PRIVATE -g)
target_link_libraries(UserService PRIVATE
    ppconsul
    srpc
    workflow
    lz4
    snappy
    protobuf
    jwt
    ssl
    crypto
)

# =============================== unit_tests (纯逻辑单元测试) ==================
# 覆盖已抽出的纯函数/值对象层 (SqlUtil / Totp / LocalBlobStore / CryptoUtil),
# 无需运行服务或数据库。构建后执行 ./bin/unit_tests 即可。
add_executable(unit_tests
    tests/unit/unit_tests.cpp
    server/util/Totp.cpp
    server/util/FileType.cpp
    server/util/RateLimiter.cpp
    server/util/JsonUtil.cpp
    server/util/CryptoUtil.cpp
    server/infra/BlobStore.cpp
)
target_include_directories(unit_tests PRIVATE ${CLOUDDISK_INCLUDE_DIRS})
target_compile_options(unit_tests PRIVATE -g)
target_link_libraries(unit_tests PRIVATE
    SimpleAmqpClient
    jwt
    ssl
    crypto
)
