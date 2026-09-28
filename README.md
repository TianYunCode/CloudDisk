# CloudVault ☁️ — 企业级云端存储

一个基于 C++ 微服务架构的现代云盘系统：HTTP 网关 + SRPC 用户服务 + 服务发现 + 消息队列异步备份到对象存储，配套一套自研、响应式、支持明暗主题的现代 Web 界面。

> 本项目在一个开源学习项目的基础上重构而来，进行了**安全加固、架构完善、功能扩展与全新前端重写**。详见文末「致谢与来源」。

---

## ✨ 功能特性

**账户与安全**
- 注册 / 登录，密码 **加盐 SHA-256** 存储，**JWT** 无状态鉴权（Bearer Token）
- 全链路**参数化转义**，杜绝 SQL 注入；用户名 / 密码 / 哈希白名单校验
- 密钥与口令**全部外置**到 `config.json` / 环境变量，源码零硬编码

**文件管理**
- **多级文件夹**：新建 / 重命名 / 进入，物化路径（materialized path）驱动，面包屑导航
- **收藏夹**：文件/文件夹一键加星，独立「收藏」视图集中查看与跳转，多选批量收藏
- **全局搜索**：搜索框支持「当前目录 / 全部文件」两种范围，全局模式跨所有目录按名称检索（支持中文），结果可直接打开定位或下载
- **文件版本历史**：每个文件保留最近 50 个历史版本，支持上传新版本、查看/下载任意历史版本、一键回滚（回滚也可再次撤销）；内容去重寻址，历史 blob 随文件彻底删除自动回收
- **存储统计分析**：可视化仪表盘展示用量/文件数/去重节省等 KPI、文件类型分布条形图、最大文件榜、近 14 天上传趋势柱状图（纯前端渲染，无第三方图表库）
- **文件标签**：用户自定义彩色标签，多对多关联文件；文件行内直接展示标签芯片，支持按标签筛选查看，标签随文件彻底删除自动解除关联
- **活动日志**：面向用户的操作审计视图，记录登录、上传、删除、恢复、重命名、新建文件夹、创建分享、账户与安全变更等事件，支持按动作类型筛选，数据按用户隔离
- 多文件**拖拽上传**到当前目录，实时**进度条**
- **内容寻址秒传**：相同内容的文件（SHA-256 命中）瞬间完成，不重复传输/存储
- 文件**列表**：目录内关键字搜索、按名称/大小/时间排序、分页
- **多选批量**操作：下载 / 移动 / 删除；**移动**支持文件与文件夹（连同子树）
- **下载 / 重命名 / 删除**（下载与操作均按 id）
- **回收站**：软删除后可恢复，支持递归删除文件夹子树、彻底删除与清空；彻底删除时按内容引用计数回收 blob（无引用才删）
- 个人**存储用量**统计与配额限制

**账户与管理**
- **个人资料**：昵称、邮箱、头像（自动裁剪为 256px）
- **修改密码**、**两步验证 (2FA/TOTP)**：兼容 Google Authenticator 等，登录时校验动态码
- **登录安全**：失败限速（5 次/5 分钟触发 429）、账户禁用检测、全程审计日志
- **管理后台**（管理员）：数据概览、用户管理、按用户设置配额、角色/禁用、审计日志查看

**上传增强**
- **分片上传 + 断点续传**：大文件自动切片（4MB/片），刷新或断网后按已传分片续传，合并时校验整文件 sha256
- **秒传**：基于内容 sha256 命中已有数据块即刻完成
- **文件夹上传**：保留目录结构（服务端递归建目录）
- **URL 离线下载**：粘贴 http/https 链接，服务器后台抓取入库

**在线预览与播放**
- 列表内**图片缩略图**（服务端 stb 生成 + 落盘缓存，长边可配）
- 全屏**预览浮层**：图片**画廊**（键盘 ←/→ 切换）、PDF 内嵌浏览
- **视频 / 音频在线播放**，基于 HTTP Range 支持拖动定位（`206 Partial Content`）
- **文本 / 代码 / Markdown** 在线阅读（内置轻量 Markdown 渲染，XSS 安全）

**分享与协作**
- **公开分享链接**：文件或文件夹（含子树）一键生成链接
- 可选**提取码**（随机 4 位或自定义）、**有效期**（1/7/30 天或永久）、**下载次数上限**
- 内置**二维码**（纯前端生成，无 CDN），扫码即达
- 访客可在线**浏览目录 / 下载**；登录用户可**一键转存**到自己的网盘（内容去重）
- **我的分享**管理：浏览量 / 下载量统计、随时取消

**云端备份**
- 上传后经 **RabbitMQ** 异步投递，由 `backup` 消费者上传到**阿里云 OSS**

**生态与运维**
- **个人访问令牌 (API Token)**：`cvt_` 前缀，供脚本/CLI/第三方集成；服务端仅存哈希，明文只显示一次；进程内注册表统一在所有 `/api/*` 上鉴权
- **WebDAV** (`/webdav/`)：OPTIONS/PROPFIND/GET/HEAD/PUT/DELETE/MKCOL/MOVE，Basic 认证（账户密码或令牌），可用 rclone / Finder / 文件资源管理器挂载
- **Prometheus 指标** (`/metrics`)：登录、上传/下载、分享、令牌、WebDAV 等计数器 + 用户/文件/存储仪表
- **Docker Compose 一键部署**：MySQL + Consul + RabbitMQ + 应用；**GitHub Actions CI** 自动编译依赖并跑集成测试

**前端体验**
- 自研设计系统，**无任何外部 CDN 依赖**（全部本地资源，离线可用）
- 明 / 暗**主题切换**、Toast 通知、模态框、骨架屏、空状态、全窗口拖拽提示
- 响应式布局，移动端可用

---

## 🏗️ 架构

```
                          ┌─────────────────────────────┐
     浏览器 (Web UI) ──────▶│  server  (wfrest HTTP :8888) │  ← JSON API / 静态资源
                          └───────┬─────────────┬───────┘
                                  │ SRPC        │ MySQL (workflow 异步)
                                  ▼             ▼
                    ┌───────────────────┐   ┌─────────┐
                    │ UserService :1414 │   │  MySQL  │  tbl_user / tbl_file
                    │  (注册/登录/鉴权)  │   └─────────┘
                    └─────────┬─────────┘        ▲
                     注册/发现 │                   │ 内容寻址 Blob 存储
                              ▼                   │ storage/blobs/<sha256>
                        ┌──────────┐        上传消息│
                        │  Consul  │   ┌───────────┴───┐   ┌──────────────┐
                        │  :8500   │   │  RabbitMQ     │──▶│ backup 消费者 │──▶ 阿里云 OSS
                        └──────────┘   │  oss.queue    │   └──────────────┘
                                       └───────────────┘
```

**三个可执行程序**
| 程序 | 角色 | 端口 |
|---|---|---|
| `server` | HTTP API 网关 + 静态站点（wfrest） | `:8888` |
| `UserService` | 用户微服务（srpc），注册到 Consul | `:1414` |
| `backup` | RabbitMQ 消费者 → 阿里云 OSS 备份 | — |

**技术栈**：C++17 · [workflow](https://github.com/sogou/workflow) · [srpc](https://github.com/sogou/srpc) · [wfrest](https://github.com/wfrest/wfrest) · MySQL · Consul(ppconsul) · RabbitMQ(SimpleAmqpClient) · 阿里云 OSS SDK · nlohmann/json · libjwt · 原生 HTML/CSS/JS 前端。

---

## 🚀 快速开始

首次构建请见 **[BUILD.md](BUILD.md)**（依赖安装 + 源码编译 + 建库）。已构建后：

```bash
# 1) 初始化数据库（首次）
mysql -uroot -p1234 < scripts/init_db.sql

# 2) 一键启动全部服务
bash scripts/start.sh

# 3) 打开浏览器
#    http://127.0.0.1:8888/
```

停止：`bash scripts/stop.sh`

启用 OSS 异步备份（可选）：复制 `scripts/oss.env.example` 为 `scripts/oss.env` 填入你的阿里云凭据，`start.sh` 会自动拉起 `backup`。

### 🐳 Docker 一键部署

无需手工安装依赖，编排会拉起 MySQL / Consul / RabbitMQ 并从源码构建应用镜像：

```bash
docker compose up -d --build
# 打开 http://localhost:8888/
```

镜像为多阶段构建：`builder` 阶段编译 Workflow / srpc / wfrest 等上游依赖与三个可执行文件，`runtime` 阶段仅保留运行期库与静态资源。生产环境请通过 `JWT_SECRET` 等环境变量覆盖默认密钥。

> 注：`Dockerfile` / `docker-compose.yml` 按惯例编写，如上游依赖分支发生 API 漂移，可在 `Dockerfile` 中将各 `git clone` 锁定到具体 tag。

---

## ⚙️ 配置

所有配置集中在 `config.json`（**环境变量优先级更高**，便于容器化部署）：

| 键 | 环境变量 | 说明 |
|---|---|---|
| `http_port` | `HTTP_PORT` | 网关端口 |
| `srpc_port` | `SRPC_PORT` | 用户服务端口 |
| `mysql_url` | `MYSQL_URL` | `mysql://user:pass@host/db` |
| `rabbitmq_url` | `RABBITMQ_URL` | `amqp://...` |
| `consul_url` | `CONSUL_URL` | Consul 地址 |
| `jwt_secret` | `JWT_SECRET` | **务必改为长随机串** |
| `storage_dir` | `STORAGE_DIR` | Blob 存储根目录 |
| `max_file_size` | `MAX_FILE_SIZE` | 单文件上限（字节） |
| `user_quota` | `USER_QUOTA` | 每用户存储配额（字节，默认 1 GB，**后端强制**） |
| — | `OSS_ENDPOINT/REGION/BUCKET/ACCESS_KEY_ID/ACCESS_KEY_SECRET` | OSS 备份（见 `scripts/oss.env.example`） |

---

## 📡 API

完整接口见 **[docs/API.md](docs/API.md)**。所有响应统一为 `{"code":0,"message":"ok","data":...}`（`code==0` 成功），受保护接口需 `Authorization: Bearer <token>`。

---

## 🔒 安全说明

- 密码加盐 SHA-256；JWT 密钥外置，请在生产环境替换 `jwt_secret`。
- 所有进入 SQL 的字符串统一转义，数值参数白名单化。
- 前端渲染统一使用 `textContent` / HTML 转义，防止 XSS。
- **当前为 HTTP 明文**：生产部署请置于 TLS 反向代理（Nginx/Caddy）之后。
- OSS AccessKey 建议使用**仅授权该 Bucket** 的 RAM 子账号密钥，切勿提交到仓库（`.gitignore` 已忽略 `scripts/oss.env`）。

---

## 🧪 测试

```bash
./bin/unit_tests                   # 54 项纯逻辑单元测试（SqlUtil/Totp/FileType/RateLimiter/JsonUtil/BlobStore/CryptoUtil，无需服务）
bash tests/integration.sh          # 173 项 API/边界/安全集成测试（含令牌/WebDAV/指标/收藏/搜索/版本/统计/标签/活动）
```

浏览器端到端测试（如环境可下载 Chromium）见 `tests/e2e/`：
`qr_validate`(11) · `format_validate`(12) · `preview_validate`(19) · `validate_validate`(17) · `e2e_p1`(11) · `e2e_share`(12) · `e2e_preview`(9) · `e2e_upload`(6) · `e2e_account`(11) · `e2e_devtoken`(7) · `e2e_fav`(5) · `e2e_search`(6) · `e2e_version`(7) · `e2e_stats`(8) · `e2e_tags`(7) · `e2e_activity`(7) · `e2e`(12 综合冒烟)。

CI（`.github/workflows/ci.yml`）会在推送/PR 时自动起中间件、编译依赖与项目，并依次跑单元测试与集成测试。

---

## 📁 项目结构

```
CloudDisk/
├── config.json            # 运行配置 (env > config.json > 默认)
├── CMakeLists.txt         # 构建 (产物输出到 bin/)
├── server/                # 服务端 C++ 后端 (分层)
│   ├── gateway/           # HTTP 网关：CloudiskServer(组合根)/GatewaySupport(支撑层)/handlers(分域处理器)/main.cpp
│   ├── user-service/      # 用户微服务 (srpc)
│   ├── backup-service/    # OSS 异步备份消费者
│   ├── rpc/               # UserService.proto 及生成代码
│   ├── common/            # 跨切面：Log.h / User.h
│   ├── config/            # Config.h
│   ├── util/              # CryptoUtil.* / SqlUtil.h / Thumbnailer.*
│   └── infra/             # OssManager.* (外部集成)
├── web/                   # Web 前端 (页面 + css + js/core 基础层 + js/views 页面控制器)
├── app/                   # 移动端占位：android/ ios/ (仅 README，Web 稳定后实现)
├── bin/                   # 编译产物 server / UserService / backup (gitignore)
├── scripts/               # init_db.sql / migrations/ / start.sh / stop.sh / oss.env.example
├── Dockerfile / docker-compose.yml / docker/  # 容器化一键部署
├── .github/workflows/ci.yml   # 持续集成
├── tests/                 # 集成测试与 e2e
├── docs/API.md            # 接口文档
└── docs/ARCHITECTURE.md   # 架构与目录设计蓝图（分层 / 设计模式 / 迁移计划）
```

---

## 🗺️ 后续路线（Roadmap）

已完成六期演进：多级文件夹/回收站 · 分享 · 预览/播放 · 上传增强（分片/断点/秒传/文件夹/离线） · 账户与管理（资料/2FA/限速/审计/管理后台） · 生态与运维（API 令牌/WebDAV/Prometheus/Docker/CI）。

后续可继续演进的方向：

- 更细粒度权限 / 多租户与团队空间
- 在线协作编辑、评论与版本历史
- HTTPS 终止与网关级限流（当前建议置于 TLS 反向代理之后）
- 全文检索与标签、分布式对象存储后端

---

## 🙏 致谢与来源

本项目在一个开源 C++ 云盘学习项目基础上重构：修复了服务注册端口不一致、SQL 注入、密钥硬编码等问题，重写了 API 层（统一响应/鉴权/内容寻址存储/秒传/搜索排序分页）、完善了配置与日志，并**全新实现了前端**。请在使用与再发布时遵循原始项目的开源许可并保留相应署名。
