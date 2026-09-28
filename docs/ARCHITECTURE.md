# CloudVault 架构与目录设计（Architecture）

本文档定义 CloudVault 云库企业云盘的**目标分层架构**、**设计模式落位**与**分阶段迁移计划**。它是架构优化阶段的蓝图；每完成一个阶段都会更新本文并保证构建干净、测试全绿。

> 设计目标（贯穿全局）：高内聚、低耦合、易读性、易维护性、健壮性、易用性、可移植性、可复用性、资源确定性、可测试性。

---

## 1. 顶层目录结构（目标）

```
CloudDisk/
├── server/                 # 服务端（C++ 后端）
│   ├── gateway/            # HTTP 网关可执行程序（表现层入口）
│   │   ├── app/            # 组合根：main、bootstrap、模块注册（装配）
│   │   ├── http/           # 控制器/路由 handler（按业务模块拆分）
│   │   ├── service/        # 应用服务 / 业务用例
│   │   ├── repository/     # 数据访问：MySQL、blob 存储（接口 + 实现）
│   │   ├── domain/         # 领域模型与值对象（User、FsNode、Share…）
│   │   ├── infra/          # 基础设施：OSS、消息队列、外部集成
│   │   ├── config/         # 配置加载（env > json > 默认）
│   │   ├── common/         # 跨切面：日志、错误、指标
│   │   └── util/           # 通用工具：Crypto、Sql、Thumbnailer
│   ├── user-service/       # 用户微服务（srpc）
│   ├── backup-service/     # 备份消费者（MQ → OSS）
│   └── rpc/                # proto 定义与生成代码
├── web/                    # Web 前端（表现层）
│   ├── pages/              # 页面 HTML（index / app / share）
│   ├── css/                # 样式
│   ├── js/
│   │   ├── core/           # 基础设施：http 客户端、Store、事件总线、主题
│   │   ├── services/       # 领域 API 封装
│   │   ├── views/          # 视图模块（文件/收藏/标签/统计/活动/回收站/分享/账户/管理）
│   │   ├── components/     # 可复用 UI 组件（modal、toast、chip…）
│   │   └── app.js          # 应用装配（组合根）
│   └── assets/
├── app/                    # 移动端
│   ├── android/README.md   # 占位（Web 稳定后启动）
│   └── ios/README.md       # 占位
├── bin/                    # 编译产物（gitignore）
├── scripts/                # 迁移/初始化/运维脚本
├── docs/                   # 文档
├── tests/                  # 集成（integration.sh）+ e2e（playwright）
├── docker/                 # 容器编排
└── third_party/            # 第三方源码依赖（stb 等）
```

---

## 2. 分层职责（依赖方向自上而下，单向）

| 层 | 职责 | 依赖 |
|---|---|---|
| 表现层 gateway/http、web/views | 协议适配、参数解析、鉴权入口、渲染 | → service |
| 应用服务层 service | 业务用例编排、事务边界、审计/指标触发 | → repository / domain |
| 领域层 domain | 领域模型、值对象、领域规则 | 无外部依赖 |
| 数据访问层 repository | MySQL、blob 存储的读写；对上暴露接口 | → infra / domain |
| 基础设施层 infra | OSS、MQ、外部系统集成 | 第三方 SDK |
| 跨切面 common/util/config | 日志、错误、配置、加解密、SQL 构建 | 被各层复用 |

**低耦合关键**：上层依赖下层的**接口**而非实现（Repository、StorageBackend 抽象），由组合根（app/bootstrap）在启动时装配具体实现（依赖倒置）。

---

## 3. 设计模式落位（GoF 23 种）

### 创建型（Creational）
| # | 模式 | 落位 |
|---|---|---|
| 1 | Singleton | `Config::instance()`、`Log::instance()`、指标注册表（已有雏形） |
| 2 | Factory Method | 按模块创建 HTTP handler；前端 view 工厂按导航创建视图 |
| 3 | Abstract Factory | 存储后端族：`LocalStorageFactory` / `OssStorageFactory` 生产 `BlobStore` + `BackupPublisher` |
| 4 | Builder | `SqlQueryBuilder`（取代散落字符串拼接）、统计/响应 JSON Builder |
| 5 | Prototype | 默认配额/角色模板、分享参数模板的克隆 |

### 结构型（Structural）
| # | 模式 | 落位 |
|---|---|---|
| 6 | Adapter | wfrest `HttpReq/HttpResp` ↔ 内部 `Request/Response`；`MySQLResultCursor` ↔ 领域 `Row` |
| 7 | Bridge | `StorageBackend` 抽象与本地/OSS 实现分离；通知抽象与渠道实现分离 |
| 8 | Composite | 文件树 `FsNode`（文件夹/文件统一处理，递归软删/面包屑） |
| 9 | Decorator | 中间件装饰链：限流→鉴权→审计→指标 包裹 handler；响应压缩/缓存装饰 |
| 10 | Facade | `CloudiskServer` 作为后端子系统外观；前端 `Api` 外观 |
| 11 | Flyweight | 内容寻址 blob（相同 hash 共享物理文件 + 引用计数去重，已实现）；图标/标签元数据共享 |
| 12 | Proxy | `AuthProxy` 受保护资源前置校验；配额/令牌校验代理；缩略图惰性生成代理 |

### 行为型（Behavioral）
| # | 模式 | 落位 |
|---|---|---|
| 13 | Chain of Responsibility | 请求中间件链（各环节可中断/放行） |
| 14 | Command | 每个 API 操作封装为携带审计元数据的命令；回收站=可撤销命令 |
| 15 | Interpreter | 搜索关键字/过滤表达式解释；分享提取码规则 |
| 16 | Iterator | `MySQLResultCursor` 行迭代（已有）、分页迭代器 |
| 17 | Mediator | 前端事件总线/Store 作为视图间中介；后端模块注册中介 |
| 18 | Memento | 文件版本历史快照（`tbl_file_version`，已实现）；回收站快照 |
| 19 | Observer | 上传/删除/分享事件触发审计与指标监听器；前端 Store 订阅渲染 |
| 20 | State | 分享链接状态（有效/过期/超额/撤销）；文件状态（正常/回收站/永久删除）；上传任务状态机 |
| 21 | Strategy | 文件类型分类（`stats_category`，已有）、排序策略、鉴权策略（JWT/API 令牌/WebDAV Basic） |
| 22 | Template Method | 统一请求处理骨架：解析→鉴权→校验→执行→响应，handler 填充步骤 |
| 23 | Visitor | 文件树遍历访问者（统计/导出/批量打标）；审计动作→中文标签映射 |

> 原则：模式服务于质量目标，**只在真正降低耦合/提升复用时引入**，不为凑数而过度设计。

---

## 4. 质量属性如何达成

| 属性 | 落地手段 |
|---|---|
| 高内聚 | 每层/模块单一职责；按领域拆分文件，避免巨型文件 |
| 低耦合 | 依赖接口 + 依赖倒置；组合根装配；第三方隔离在 infra/util |
| 易读性 | 目录即文档、命名一致、统一响应信封 `{code,message,data}` |
| 易维护性 | 模块化、单一真源、文档 + CI 门禁 |
| 健壮性 | 统一错误处理、输入校验/白名单、SQL 转义、登录限流、事务、可重试 |
| 易用性 | 一致的 REST 契约、美观交互 UI、清晰错误信息 |
| 可移植性 | 配置外置（env>json>默认）、Docker 一键部署、无硬编码绝对路径 |
| 可复用性 | 通用工具/UI 组件、抽象接口、跨端复用同一套 API |
| 资源确定性 | 内容寻址去重 + 引用计数 GC、用户配额、workflow series 生命周期确定、审计 fire-and-forget 不阻塞 |
| 可测试性 | 分层可替身、纯函数工具、集成 + e2e 全覆盖、CI 自动跑测 |

---

## 5. 分阶段迁移计划（每阶段后必须：构建干净 + 集成 + e2e 全绿 + 提交）

- **Stage 0（基线）**：功能冻结。基线测试 = 173 集成 + 14 e2e 全绿。✅
- **Stage 1（本阶段）**：建立顶层分层骨架——`app/`（android、ios 占位）、`web/`（前端从 `static/` 迁入）、本架构蓝图文档。
- **Stage 2**：C++ 源码迁入 `server/` 分层树 + `bin/` 输出目录；更新 CMake / Dockerfile / 运维脚本；全量回归。
- **Stage 3**：拆分 3800 行单体 `CloudiskServer.cpp` 为分层翻译单元（http/service/repository/domain），提取共享工具/全局到明确的层。
- **Stage 4**：引入设计模式骨架（Strategy 鉴权、Chain 中间件、Repository 接口、StorageBackend 抽象工厂/桥接、Command+审计、Template Method handler…），逐模式重构并回归。
- **Stage 5**：前端分层（core/services/views/components）、事件总线（Mediator/Observer）、视图工厂（Factory Method）。

---

## 6. 当前进度

- Stage 0：✅ 完成（功能冻结于「用户活动日志」）。
- Stage 1：✅ 完成——`app/android`、`app/ios` 占位；前端 `static/` → `web/`；本蓝图文档。
- Stage 2：✅ 完成——C++ 源码迁入 `server/` 分层树（gateway/user-service/backup-service/rpc/common/config/util/infra），产物输出到 `bin/`；同步更新 CMake（`target_include_directories` 跨层解析裸 include）、Dockerfile、`.gitignore`/`.dockerignore`、`scripts/start.sh`、`docker/entrypoint.sh`、CI、BUILD.md。全量回归：构建干净、集成 173/0、e2e 14 套全绿。
- Stage 3：✅ 完成——**3a**：抽出共享支撑层 `GatewaySupport.{h,cpp}`（运行期配置全局、`Metrics`、`TokenRegistry`、文件/blob/JSON/SQL/审计/限速/Base32/TOTP/管理员守卫）。**3b**：按领域把 `register_*_module` 拆为 7 个独立翻译单元 `server/gateway/handlers/`：`AuthHandlers`（注册/登录/用户信息/账户/令牌）、`FileHandlers`（上传/列表/下载/分片/离线/预览/文件夹）、`TrashHandlers`（回收站）、`ShareHandlers`（分享）、`AdminHandlers`（后台/指标）、`WebdavHandlers`（WebDAV）、`ExtraHandlers`（收藏/搜索/版本/统计/标签/活动）。`CloudiskServer.cpp` 3884→128 行（仅保留组合根 `register_modules` + 静态资源装配）；各模块私有 helper 随模块下沉为 TU 私有匿名命名空间。全量回归：构建干净、集成 173/0、e2e 14 套全绿。
