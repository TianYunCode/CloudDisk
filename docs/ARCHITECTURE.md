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
- Stage 3：✅ 完成——**3a** 抽出共享支撑层 `GatewaySupport.{h,cpp}`（运行期配置全局、`Metrics`、`TokenRegistry`、文件/blob/JSON/SQL/审计/限速/Base32/TOTP/管理员守卫）。**3b** 按领域把 `register_*_module` 拆为 7 个翻译单元 `server/gateway/handlers/`（Auth/File/Trash/Share/Admin/Webdav/Extra）；`CloudiskServer.cpp` 3884→128 行（仅保留组合根 + 静态资源装配）。全量回归：构建干净、集成 173/0、e2e 14 套全绿。
- Stage 4：进行中——**4a ✅ 完成**：blob 存储子系统 `server/infra/BlobStore.{h,cpp}` 一处收敛并真实接线 4 个模式：`IBlobStore`/`IBlobBackup` 抽象 vs `LocalBlobStore`/`RabbitMqOssBackup`/`NullBlobBackup` 实现（**Strategy/Bridge**）、`init_blob_*` 依配置装配（**Abstract Factory**）、`blob_store()`/`blob_backup()` 唯一访问点（**Singleton**）、内容寻址去重（**Flyweight**）；组合根装配，各 handler 存储细节全部改走委托接口。全量回归：构建干净、集成 173/0、e2e 14 套全绿。**4b 待办**：鉴权 Strategy、请求中间件 Chain、Repository 接口等按需引入。
- Stage 5：进行中——**5a ✅ 完成**：Web 前端 `web/js/` 分层归位——`core/`（共享基础层：`api.js` 提供 `window.CV` 门面=Store/HTTP 客户端/Api 服务/toast/Theme、`sha256.js`、`qr.js`）与 `views/`（页面控制器：`auth.js` 登录注册页 / `app.js` 主应用 / `share.js` 公开分享页）；同步更新 3 个 HTML 的 `<script src>`、`qr_validate` 与 `integration.sh` 离线下载用例的引用路径。全量回归：集成 173/0、e2e 14 套全绿（浏览器级 Playwright 覆盖各页面加载与交互）。**5b 进行中**：已抽出前端纯逻辑模块——`core/format.js`（文件类型分类 + 图标常量）、`core/preview.js`（预览类型归类 + 极简 Markdown 渲染）、`core/validate.js`（输入校验规则，**与后端 `SqlUtil` 对齐的单一真源**）、`core/pager.js`（分页计算：pageCount/pageRange/pageButtons/infoHtml，纯函数，无 DOM 依赖）、`core/activity.js`（活动动作→展示元数据映射 = 后端只落 action 名、前端集中决定中文标签/图标/配色，Visitor 思想）、`core/bus.js`（**极简事件总线：Observer/Mediator 真实接线**——视图订阅 `data:changed`，各操作只需 `emit('data:changed')` 触发相关视图刷新，解耦"谁在刷新"与"什么时候刷新"；on/once/off/emit/clear/listenerCount，handler 抛错不阻断其它订阅者）、`core/crumbs.js`（面包屑构造：根目录永远前置的业务规则，纯函数）；均浏览器挂载 `window.CV`、Node 可 `require` 单测，与 DOM/渲染解耦；`app.js`/`auth.js` 改为引用这些模块。`app.js` 内部 7 处跨视图刷新（删除/移动/版本恢复/上传等）已从散落的 `loadList(); loadUser();` 改为 `emitChange()` = `Bus.emit('data:changed')`。新增 Node 单测 `format_validate.mjs`(12)、`preview_validate.mjs`(19)、`validate_validate.mjs`(17)、`pager_validate.mjs`(13)、`activity_validate.mjs`(10)、`bus_validate.mjs`(13，含 once/异常隔离/退订/事件隔离)、`crumbs_validate.mjs`(15)。**5b 待办**：`app.js` 内部按视图/组件进一步拆分，视图工厂（Factory Method，仅在能真实接线降耦时）。
- Stage 6（可测试性）：✅ **持续推进**——纯逻辑/策略对象抽出为独立可测模块：`server/util/Totp.{h,cpp}`（Base32/TOTP）、`server/util/FileType.{h,cpp}`（MIME/缩略图判定）、`server/util/RateLimiter.{h,cpp}`（登录失败滑动窗口限速器，时间可注入 → 确定性可测）、`server/util/JsonUtil.{h,cpp}`（`json_str`/`ids_from_json`/`ids_csv`，请求 JSON 提取与 id 列表安全处理，仅依赖 nlohmann/json）、`server/util/FormatUtil.h`（`human_size()` 人类可读字节格式化，header-only 纯函数）、`server/common/AuditAction.h`（**审计动作名单一真源**：24 处 `audit_log` 调用全部改用 constexpr 常量，消除散落的字符串字面量）；`CryptoUtil.h` 自包含 openssl 头；`web/js/core/api.js` 改为双导出（`module.exports` + `window.CV`），使其纯函数可在 Node 下直接测试。`tests/unit/unit_tests.cpp`（CMake `unit_tests` → `bin/unit_tests`）覆盖 `SqlUtil` / `Totp`（RFC6238 向量）/ `FileType` / `RateLimiter` / `JsonUtil` / `AuditAction` / `FormatUtil` / `LocalBlobStore` 去重 / `CryptoUtil`，共 **92 断言全绿**；CI 已加入构建并执行单元测试 + 14 个前端纯逻辑/契约套件。全量回归：单元 92/0、集成 173/0、e2e 全绿。
- Stage 7（前后端契约）：✅ **已建立七道自动化守卫**
  - `tests/e2e/audit_contract_validate.mjs`：解析 `AuditAction.h` 的动作常量与 `activity.js` 的 `ACT_META` 键，双向断言集合完全一致（后端缺标签 / 前端僵尸键 均报错），并做守卫自检。**该测试发现并修复了一个真实缺陷**：后端 3 个管理员动作（`admin_set_role`/`admin_set_quota`/`admin_set_disabled`，库中已有 **182 条**记录）此前在前端活动日志中显示为原始英文名，现已补齐中文标签/图标/配色。
  - `tests/e2e/route_contract_validate.mjs`：扫描 `server/gateway` 下全部 `.GET/.POST/...` 注册与前端 `web/js` 下全部 `'/api/...'` 字面量（截断查询串），断言四条不变量——① 前端调用的每个路径后端都已注册（防静默 404）；② 后端每个 `/api` 路由都有前端调用方（防死端点）；③ 非 `/api` 路由仅限页面/运维白名单（`/`、`/app`、`/share.html`、`/healthz`、`/metrics`）；④ **每条 `/api` 路由都已在 `docs/API.md` 中记录（防文档漂移）**。当前为 **65 条后端路由（60 条 `/api` + 5 条白名单）↔ 60 条前端调用路径 ↔ 60 条文档记录的三方一致**。
  - `tests/e2e/errcode_contract_validate.mjs`：区分「标准 HTTP 语义码」（400/401/403/404/409/410/413/429/500/503，前端走通用提示即可）与「自定义业务码」（必须有前端显式分支）。断言四条不变量——① 后端每个自定义业务码前端都有显式处理（防"发了没人接"）；② 前端显式比较的每个码后端确实会发出（防死分支）；③ `code=0` 是唯一成功哨兵且后端错误路径从不发出 0；④ 后端码均为正整数。当前后端共 11 个业务码，唯一自定义码 `4012`（需要两步验证码）已被 `auth.js` 显式处理。
  - `tests/e2e/config_contract_validate.mjs`：配置读取优先级为 `env > config.json > 内置默认`（见 `server/config/Config.h` 的 `get_str/get_int(key, env, def)`）。三处来源最易漂移，故断言五条不变量——① `config.json` 的每个键都被 `Config.h` 真正读取（防「改了配置却不生效」）；② 环境变量名唯一且为 `SCREAMING_SNAKE`；③ `docker-compose` 的 `app` 服务注入的每个变量都被 `Config.h` 或 `docker/entrypoint.sh` 消费（防孤立变量）；④ 未出现在 `config.json` 的配置项必须有内置默认值（防启动即崩）；⑤ **密钥卫生**——仓库内 `config.json` 的 `jwt_secret` 必须仍是占位符，且不含 GitHub PAT / 云 AccessKey / 私钥形态字符串（真实值只能经环境变量注入）。当前 12 个配置项，`config.json` 提供 10 个，`metrics_token` 与 `oss_bucket` 走默认值。
  - `tests/e2e/humansize_contract_validate.mjs`：**现场编译 C++ 探针**（仅 `#include "FormatUtil.h"`），让后端 `human_size()` 与前端 `api.js` 的 `humanSize()` 对同一批 28 个数值（负数 / 0 / 各单位段 / 取整边界 / TB 上限）逐值比对，要求输出**完全相同**，并校验两端单位档位序列一致；无 g++ 时优雅跳过跨语言部分。**该守卫发现并修复了一个真实缺陷**：前端 `humanSize` 对负数不做单位换算，直接原样输出（`-1048576` → `"-1048576 B"`），与后端不一致；现两端统一为「符号 + 幅值」（`-1048576` → `"-1.0 MB"`），因为「去重节省」这类差值指标理论上可为负，显示 `0 B` 会误导。
  - 顺带修复的**用户可见缺陷**：配额不足的 `413` 提示原先拼原始字节数（「存储空间不足: 剩余 1024 字节, 本次需 5242880 字节」），现改用 `fmtutil::human_size()` → 「存储空间不足: 剩余 1.0 KB, 本次需 5.0 MB」（`FileHandlers.cpp` 3 处；已用真实上传端到端验证）。
  - `tests/e2e/validate_contract_validate.mjs`：**现场编译 C++ 探针**（`#include "SqlUtil.h"`），对 22 个密码 + 15 个用户名输入（ASCII / 中文 / emoji 代理对 / 63-65 边界 / 非法字符 / 空串 / 超长）逐值比对前后端判定，并比对码点计数本身（`SqlUtil::utf8_length` vs `Validate.charLength`）。**该守卫发现并修复了一个真实缺陷**：密码长度两端计量单位不同——后端 `p.size()` 是 UTF-8 **字节数**、前端 `p.length` 是 UTF-16 **码元数**，于是 `"密码"`（2 字符/6 字节）后端放行而前端拦截、33 个汉字（99 字节）前端放行而后端 400；更严重的是 `"🔒🔒🔒"` 只有 **3 个字符**却因代理对计为 6 个码元而通过「至少 6 位」下限，`"密码"` 也因 6 字节通过后端下限——两端都能放过远弱于 6 字符的密码。现统一按 **Unicode 码点**计数（后端新增 `SqlUtil::utf8_length()` 并加 256 字节原始长度上限防滥用；前端新增 `Validate.charLength()` 用 `for...of` 正确迭代码点），并顺带修正 `app.js` 改密码处同样的 `newPw.length < 6` 码元判定、把 `validate.js` 接入 `app.html`（原先只有登录页加载它），以及统一前后端与页面的提示文案为「6-64 个字符」。已用真实注册/登录端到端验证：2 个汉字被拒、6 个汉字可注册并登录、33 个汉字两端一致接受。
  - `tests/e2e/sha256_contract_validate.mjs`：**三方对拍** SHA-256——前端 `sha256.js` 的纯 JS `sha256hex`、Node 权威 `crypto`、后端 `CryptoUtil::generate_hashcode`（现场编译探针并链接 `jwt/ssl/crypto`），对 22 种长度（0 至 64 KiB，含 55/56/57/63/64/65/119/120/121/127/128/129 等**全部经典填充边界**）逐值比对，另加 3 组 NIST 已知答案向量与输出形态校验。此项尤为关键：经 `http://<局域网 IP>:8888` 访问时页面**不是安全上下文**，`crypto.subtle` 不可用，`hashFile` 会回退到纯 JS 实现——**那才是实际生效路径**，而秒传/去重/引用计数删除全部依赖「同一份字节在任何一端算出的哈希完全相同」，任何偏差都会造成命中错误内容且症状隐蔽。本次核验结论：三个实现完全一致（未发现缺陷）；同时把 `sha256.js` 从 `})(window)` 改为双导出并挂到 `window.CV.Sha256`，使其可在 Node 下被永久测试（此前只能用 `vm` 桩临时加载）。
  - 七道守卫均已做**负向验证**（人为注入拼错的动作名/路径、把前端 `4012` 改成 `4013`、从 `docs/API.md` 抹掉一条路由记录、给 `config.json` 加一个无人读取的键，测试均精确报出问题项后恢复；`humansize` 与 `validate` 的负向验证即上述真实缺陷本身），并接入 CI（与 7 个前端纯逻辑套件同一步骤，共 14 个无需 npm 依赖的套件）。
  - **统一测试入口** `tests/run_all.sh`：一条命令按 4 层顺序跑完 C++ 单元 → 前端纯逻辑/契约 → 集成 → 浏览器 e2e，逐项 ✓/✗ 汇总，任一失败即非零退出（可作 pre-push 门禁）；支持 `--fast`（仅前两层，无需服务）、`--no-e2e`，以及 `E2E_SPACING` 调节 Chromium 启动间隔（连续启动偶发抖动时增大即可，非代码回归）。当前 31 项全绿。
- Stage 8（消除重复判定 / 单一真源）：✅ **已修复一处概念混淆**——「浏览器能把该文件当图片预览」（含 webp/svg/ico）与「后端能为该文件生成缩略图」（`is_thumbnailable`：png/jpg/jpeg/gif/bmp/tga/psd/ppm/pgm）是**两个不同集合**，但前端原先用同一个 `isImage(filename)` 同时决定这两件事。后果：每个 svg/webp/ico 文件在每次列表渲染时都会白发一次 `/api/file/thumb` 并收到 **404「该类型无缩略图」**，且因 `<img>` 先渲染再 `onerror` 回退到类型图标，用户会看到一次闪烁。
  - 修法（权威下发而非两端各自猜测）：`/api/file/list` 的每个文件项新增布尔字段 **`hasThumb`**，其值直接由缩略图端点内部使用的同一个 `is_thumbnailable()` 计算，因此二者**不可能漂移**；前端新增 `canThumb(f)` 优先采用 `hasThumb`（字段缺失时退回 `isImage` 以保持兼容），仅用于缩略图决策；`isImage()` 继续用于预览/相册（webp/svg/ico 浏览器确实能渲染，这一能力不应被一并砍掉）。`docs/API.md` 已记录该字段及上述概念区别。
  - 行为级回归守卫 `tests/e2e/e2e_thumb.mjs`（14 项）：真实上传 png/svg/webp/ico，断言 ① 列表下发的 `hasThumb` 与各类型真实可缩略性一致；② 稳态渲染（整页刷新后）**零个** `/api/file/thumb` 404；③ 只发出 **1** 次缩略图请求且返回 200；④ DOM 中仅 png 行存在 `<img class="thumb">`；⑤ svg/webp/ico 仍被识别为可预览图片；⑥ 无控制台/JS 错误。**负向验证**：把 `canThumb` 临时改回 `isImage`，测试立即报出 4 项失败（3 次白发 404、请求数 4 而非 1、状态 `200,404,404,404`、三行都渲染了 `<img class="thumb">`），恢复后 14/0 全绿。
