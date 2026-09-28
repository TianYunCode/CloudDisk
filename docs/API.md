# CloudVault API 参考

- 基础地址：`http://<host>:8888`
- 统一响应封装：

```json
{ "code": 0, "message": "ok", "data": { } }
```

`code == 0` 表示成功；非 0 为业务/错误码（通常与 HTTP 状态码一致）。
受保护接口需携带：`Authorization: Bearer <token>`（下载接口也支持 `?token=` 查询参数，便于浏览器直链下载）。

---

## 认证

### POST `/api/auth/register` — 注册
Body（JSON）：`{ "username": "alice", "password": "secret123" }`
- 用户名：3–32 位，字母/数字/`_`/`-`
- 密码：6–64 位

| 状态 | 含义 |
|---|---|
| 200 | 注册成功 |
| 400 | 参数不合法 |
| 409 | 用户名已存在 |

### POST `/api/auth/login` — 登录
Body：`{ "username": "alice", "password": "secret123" }`

成功 `data`：
```json
{ "token": "eyJ...", "username": "alice", "createdAt": "2026-09-27 10:00:00" }
```
| 状态 | 含义 |
|---|---|
| 200 | 登录成功 |
| 401 | 用户名或密码错误 |

---

## 用户

### GET `/api/user/info` 🔒 — 当前用户信息与用量
成功 `data`：
```json
{ "username": "alice", "createdAt": "2026-09-27 10:00:00", "fileCount": 12, "storageUsed": 34567, "quota": 1073741824 }
```
`quota` 为该用户的存储配额（字节，由 `user_quota` 配置，后端强制）。

---

## 文件

### POST `/api/file/upload` 🔒 — 上传（多文件）
`multipart/form-data`，一个或多个文件字段（字段名任意）。
Query：`parentId`（可选，默认 0）——上传到指定文件夹。
成功 `data`：
```json
{ "files": [ { "filename": "a.txt", "hash": "<sha256>", "size": 22, "instant": false } ] }
```
`instant=true` 表示该内容此前已存在（去重，未重复写盘）。

| 状态 | 含义 |
|---|---|
| 200 | 成功 | 
| 400 | 非 multipart / 无文件 |
| 413 | 超过单文件大小上限，或超出用户存储配额（`存储空间不足`） |

### POST `/api/file/instant` 🔒 — 秒传探测
Body：`{ "filename": "a.txt", "hash": "<sha256>", "size": 22, "parentId": 0 }`
- 命中（blob 已存在）：`data.instant = true`，直接建立元数据，无需上传文件体。
- 未命中：`data.instant = false`，客户端需走完整上传。
- 命中但超出用户配额：返回 `413 存储空间不足`。

> 客户端上传前先在本地计算文件 SHA-256（浏览器端用 WebCrypto，非安全上下文回退纯 JS 实现），据此实现秒传。

### GET `/api/file/list` 🔒 — 列出目录内容（文件夹 + 文件）
Query：

| 参数 | 默认 | 说明 |
|---|---|---|
| `parentId` | 0 | 当前文件夹 id（0 = 根目录） |
| `limit` | 20（≤100） | 每页文件条数（文件夹不分页） |
| `offset` | 0 | 文件偏移 |
| `keyword` | — | 在当前目录内按名称模糊匹配（文件夹 + 文件） |
| `sort` | `created_at` | `filename` \| `size` \| `created_at` \| `last_update`（白名单） |
| `order` | `desc` | `asc` \| `desc` |

成功 `data`：
```json
{
  "parentId": 5,
  "breadcrumb": [ { "id": 3, "name": "Docs" }, { "id": 5, "name": "Sub" } ],
  "folders": [ { "id": 7, "name": "图片", "createdAt": "..." } ],
  "total": 42,
  "items": [
    { "id": 1, "filename": "a.txt", "hash": "<sha256>", "size": 22,
      "createdAt": "...", "lastUpdate": "..." } ]
}
```
`breadcrumb` 为从根到当前目录的祖先链（不含根）。

### GET `/api/file/download` 🔒 — 下载（按 id）
Query：`id`（必填，文件 id）。返回文件流，带 `Content-Disposition: attachment`。
支持 `?token=<jwt>` 以便浏览器直链下载。
| 状态 | 含义 |
|---|---|
| 200 | 文件流 |
| 404 | 文件不存在或内容缺失 |

### POST `/api/file/rename` 🔒 — 文件重命名（按 id）
Body：`{ "id": 1, "newname": "new.txt" }`
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 404 | 文件不存在 |
| 409 | 同目录下目标名已存在 |

---

## 文件夹

### POST `/api/folder/create` 🔒 — 新建文件夹
Body：`{ "name": "图片", "parentId": 0 }`
成功 `data`：`{ "id": 7, "name": "图片", "parentId": 0 }`
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 404 | 父文件夹不存在 |
| 409 | 同目录下已存在同名文件夹 |

### POST `/api/folder/rename` 🔒 — 文件夹重命名
Body：`{ "id": 7, "newname": "照片" }`（`404` 不存在 / `409` 同名冲突）

### POST `/api/fs/move` 🔒 — 移动（文件 + 文件夹，批量）
Body：`{ "fileIds": [1,2], "folderIds": [7], "targetId": 0 }`（`targetId` = 0 表示根目录）
文件夹连同整棵子树一起移动。
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 移动到自身 / 自身子目录 |
| 404 | 目标文件夹不存在 |
| 409 | 目标目录存在同名项 |

---

## 回收站

删除采用**软删除**：项目先进入回收站，可恢复；彻底删除时才回收无引用的 blob。

### POST `/api/fs/delete` 🔒 — 移入回收站（批量）
Body：`{ "fileIds": [1], "folderIds": [7] }`
删除文件夹时递归软删其整棵子树；期间 blob 保留。

### GET `/api/trash/list` 🔒 — 回收站列表
仅返回用户直接删除的顶层项。成功 `data`：
```json
{ "folders": [ { "id": 7, "name": "图片", "deletedAt": "..." } ],
  "files":   [ { "id": 1, "filename": "a.txt", "size": 22, "deletedAt": "..." } ] }
```

### POST `/api/trash/restore` 🔒 — 恢复（批量）
Body：`{ "fileIds": [1], "folderIds": [7] }`（文件夹连同子树恢复；`409` 原位置存在同名项）

### POST `/api/trash/delete` 🔒 — 彻底删除（批量）
Body：`{ "fileIds": [1], "folderIds": [7] }`
永久删除并回收无引用 blob。成功 `data`：`{ "blobsRemoved": 3 }`

### POST `/api/trash/empty` 🔒 — 清空回收站
永久删除当前用户回收站内所有项并回收 blob。

---

## 分享

公开链接 + 可选提取码 + 可选过期时间 + 可选下载次数上限；支持文件与文件夹（含子树）分享与转存。

### POST `/api/share/create` 🔒 — 创建分享
Body：`{ "fileId": 1 }` 或 `{ "folderId": 7 }`（二选一），可选 `code`(自定义提取码，字母数字≤16)、`autoCode`(true 随机 4 位)、`expireDays`(0=永久)、`maxDownloads`(0=不限)。
成功 `data`：`{ "token", "code", "isFolder", "name", "expireDays", "maxDownloads", "path": "/share.html?t=<token>" }`
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 参数错误（未指定对象 / 提取码非法 / 超范围） |
| 404 | 要分享的对象不存在 |

### GET `/api/share/mine` 🔒 — 我的分享列表
成功 `data.shares[]`：`{ id, token, code, isFolder, name, maxDownloads, downloads, views, createdAt, expireAt, expired }`

### POST `/api/share/cancel` 🔒 — 取消分享
Body：`{ "id": 3 }`（`404` 不存在）。取消后链接立即失效。

### GET `/api/share/info` — 分享信息（公开）
Query：`token`。返回 `{ name, isFolder, needCode, downloads, maxDownloads }`（不暴露内容）。每次访问计一次浏览量。
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 404 | 不存在 / 已取消 |
| 410 | 已过期 |

### GET `/api/share/browse` — 浏览分享内容（公开，需提取码）
Query：`token`、`code`(有提取码时必填)、`folderId`(文件夹分享内导航，默认根)。
返回 `{ isFolder, rootName, breadcrumb:[{id,name}], folders:[{id,name}], files:[{id,filename,size,hash}] }`
| 状态 | 含义 |
|---|---|
| 403 | 提取码错误 / 目录越界 |
| 404/410 | 不存在 / 已过期 |

### GET `/api/share/download` — 下载分享文件（公开，需提取码）
Query：`token`、`code`、`id`(文件夹分享时为子树内文件 id；文件分享时须等于被分享文件 id)。返回文件流并累加下载计数。
| 状态 | 含义 |
|---|---|
| 200 | 文件流 |
| 403 | 提取码错误 / 下载次数已用尽 / 文件不在分享范围 |

### POST `/api/share/save` 🔒 — 转存到我的网盘（需提取码）
Body：`{ "token", "code", "targetId" }`（`targetId`=0 根目录）。文件分享复制单文件；文件夹分享递归复制整棵子树（内容寻址去重，占用配额）。
成功 `data`：`{ "folders": <数>, "files": <数> }`
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 401 | 未登录 |
| 403 | 提取码错误 |
| 409 | 目标目录存在同名项 |
| 413 | 存储空间不足 |

---

## 在线预览

### GET `/api/file/content` — 内联返回文件内容（支持 Range）
Query：`id`、`token`。用于图片 / 视频 / 音频 / PDF / 文本的浏览器内联预览。
- 响应头 `Accept-Ranges: bytes`、`Content-Disposition: inline`、按扩展名推断的 `Content-Type`。
- 携带 `Range: bytes=start-end` 时返回 `206 Partial Content` + `Content-Range: bytes s-e/total`（视频/音频拖动定位依赖此能力）。
| 状态 | 含义 |
|---|---|
| 200 | 全量内容 |
| 206 | 分块内容（带 Range） |
| 401 | 未登录 |
| 404 | 文件不存在 / 内容缺失 |

### GET `/api/file/thumb` — 图片缩略图（JPEG）
Query：`id`、`token`、`size`(长边像素，默认 256，范围 32–512)。仅位图（png/jpg/jpeg/gif/bmp/tga/psd/ppm/pgm）支持；首次生成后落盘缓存（`storage/thumbs/<hash>_<size>.jpg`），响应 `Cache-Control: public, max-age=604800`。
| 状态 | 含义 |
|---|---|
| 200 | `image/jpeg` 缩略图 |
| 401 | 未登录 |
| 404 | 非图片类型 / 文件不存在 / 生成失败 |

---

## 上传增强

### POST `/api/upload/init` 🔒 — 初始化分片上传会话
Body：`{ filename, hash(sha256), size, parentId, chunkSize, totalChunks }`
- 若 `hash` 命中已有 blob → 直接建记录，返回 `{ instant:true, filename, hash, size }`（秒传）。
- 否则返回 `{ instant:false, uploadId, uploaded:[已收到的分片序号], chunkSize, totalChunks }`；`uploaded` 用于**断点续传**（相同 uid+hash+parentId 的进行中会话会被复用）。
| 状态 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 参数不合法 |
| 413 | 存储空间不足（按完整大小预检） |

### POST `/api/upload/chunk?uploadId=&index=` 🔒 — 上传单个分片
请求体为该分片的**原始二进制**（`application/octet-stream`）。`index` 从 0 计。
| 状态 | 含义 |
|---|---|
| 200 | 分片已写入 |
| 400 | 参数不合法 / 序号越界 / 空分片 |
| 404 | 会话不存在或已完成 |

### POST `/api/upload/complete` 🔒 — 合并分片
Body：`{ uploadId }`。服务端顺序拼接全部分片，**校验整文件 sha256** 与声明一致，落 blob（含引用去重 + OSS 备份）并建记录，清理临时分片。
| 状态 | 含义 |
|---|---|
| 200 | `{ filename, hash, size, instant }` |
| 400 | 分片缺失 / hash 校验失败 / 参数不合法 |
| 413 | 存储空间不足 |

### POST `/api/folder/ensure` 🔒 — 递归确保多级目录
Body：`{ parentId, path:"a/b/c" }`。逐段“存在即复用、缺失则创建”，返回叶子目录 `{ id, path }`。用于**文件夹上传**。幂等；层级上限 64。

### POST `/api/offline/create` 🔒 — URL 离线下载
Body：`{ url(http/https), parentId, filename? }`。立即入队并返回 `{ id, filename, status:0 }`，服务端后台抓取；完成后自动建文件记录（受配额与单文件大小上限约束）。
| 状态 | 含义 |
|---|---|
| 200 | 已入队 |
| 400 | 非 http/https 链接 |

### GET `/api/offline/list` 🔒 — 离线任务列表
返回 `tasks[]`：`{ id, url, filename, status(0下载中/1完成/2失败), size, message, fileId, createdAt }`。

---

## 账户与安全

登录 (`POST /api/auth/login`) 现由网关侧校验，新增能力：
- 支持可选 `code` 字段用于两步验证；开启 2FA 的账户未带 `code` 时返回业务码 **4012**（HTTP 401），前端据此提示输入动态码。
- **登录限速**：同一 `用户名|IP` 在 5 分钟内失败 5 次将被限流 5 分钟（HTTP 429）。
- **禁用检测**：被管理员禁用的账户登录返回 HTTP 403。
- 成功响应含 `role`（0 普通 / 1 管理员）。

### POST `/api/user/profile` 🔒 — 更新资料
Body：`{ nickname, email }`（邮箱做基本格式校验）。

### POST `/api/user/password` 🔒 — 修改密码
Body：`{ oldPassword, newPassword }`。校验原密码后以新盐重算 sha256；成功后建议前端重新登录。

### POST `/api/user/avatar` 🔒 — 上传头像
`multipart/form-data`，取首个图片字段，裁剪为 256px JPEG。

### GET `/api/user/avatar?uid=` — 读取头像 (公开)
返回 `image/jpeg`；无头像返回 404。

### 两步验证 (TOTP, RFC 6238)
- `POST /api/2fa/setup` 🔒 → `{ secret(base32), otpauth }`（生成密钥，未启用）。
- `POST /api/2fa/enable` 🔒 `{ code }` → 校验动态码后启用。
- `POST /api/2fa/disable` 🔒 `{ code }` → 校验动态码后关闭。

`GET /api/user/info` 现额外返回：`uid, nickname, email, avatar, hasAvatar, role, twoFactor`，`quota` 为该用户有效配额（个人配额优先，否则全局默认）。

---

## 管理后台 (需 role=1)

所有接口先经 `require_auth`，再校验 `role==1`，否则返回 HTTP 403。

| 方法 & 路径 | 说明 |
|---|---|
| GET `/api/admin/stats` | 概览：用户数、文件数、存储用量、去重块数、分享数、离线任务数、管理员数 |
| GET `/api/admin/users?limit=&offset=` | 用户列表（含角色/禁用/2FA/配额/用量/注册时间） |
| POST `/api/admin/user/quota` `{uid,quota}` | 设置个人配额（字节，0=全局默认） |
| POST `/api/admin/user/role` `{uid,role}` | 设置角色（拒绝降级最后一名管理员） |
| POST `/api/admin/user/disable` `{uid,disabled}` | 启用/禁用（不能禁用自己） |
| GET `/api/admin/audit?limit=&offset=&action=` | 审计日志（登录/注册/上传/分享/改密/2FA/管理操作等） |

> 管理员引导：迁移 `005` 将最小 id 用户设为管理员；全新安装时首个注册用户会被自动提升。

---

## 生态 · 个人访问令牌（API Token）

个人访问令牌供脚本、命令行、WebDAV 与第三方集成使用，可作为 `Authorization: Bearer <token>` 或 `?token=` 使用，也可作为 WebDAV 的 Basic 密码。令牌明文仅在创建时返回一次；服务端只保存其 SHA-256 摘要。前缀 `cvt_`。

| 方法 & 路径 | 说明 |
|---|---|
| POST `/api/tokens` `{name, expiresDays?}` | 创建令牌，返回明文 `token`（仅此一次）、`id`、`prefix`；`expiresDays` 为 0/省略表示永久 |
| GET `/api/tokens` | 列出本人未吊销的令牌（不含明文：名称/前缀/最近使用/过期时间） |
| POST `/api/tokens/revoke` `{id}` | 吊销令牌，内存注册表同步移除，立即失效 |

令牌解析由进程内注册表在启动时加载并在增删时更新，`require_auth` 在 JWT 未命中时回退解析，令牌鉴权对全部 `/api/*` 路由统一生效。

## 生态 · WebDAV

挂载点 `/webdav/`，映射当前用户的文件夹/文件树。认证使用 HTTP Basic：用户名为账户名，密码可用**账户密码**或**个人访问令牌**（推荐）。

| 方法 | 行为 |
|---|---|
| OPTIONS | 返回 `DAV: 1, 2` 与 `Allow` |
| PROPFIND (`Depth: 0/1`) | 返回 `207 Multistatus`：集合列出子文件夹与文件；文件返回自身属性 |
| GET / HEAD | 下载文件内容 / 仅返回头（`Content-Length`、`Content-Type`） |
| PUT | 上传（内容寻址去重 + 配额校验），成功 `201` |
| DELETE | 软删除到回收站，成功 `204` |
| MKCOL | 新建文件夹，成功 `201` |
| MOVE (`Destination` 头) | 文件/文件夹重命名或移动（文件夹会重写子孙物化路径），成功 `201` |

> 示例（rclone）：`rclone config` 选择 `webdav`，URL `http://<host>:8888/webdav/`，vendor `other`，user 为用户名，pass 为 `cvt_` 令牌。

## 生态 · 监控指标

### GET `/metrics` — Prometheus 文本曝露格式
默认无需认证（建议置于内网/反代之后）。若配置了 `metrics_token`（或环境变量 `METRICS_TOKEN`），抓取时须带 `?token=<值>` 或 `Authorization: Bearer <值>`，否则返回 `401`。导出计数器与仪表：登录成功/失败、上传/下载次数与字节、分享创建、令牌鉴权次数、WebDAV 请求数，以及用户数、文件数、逻辑存储用量、活跃分享数、运行时长。

## 收藏夹

| 方法 & 路径 | 说明 |
|---|---|
| POST `/api/favorite/toggle` `{itemType, itemId}` | 切换收藏（`itemType`：0=文件，1=文件夹）；已收藏则取消，返回 `{favorited}`；添加时校验条目归属当前用户且未删除，否则 `404` |
| POST `/api/favorite/batch` `{fileIds[], folderIds[]}` | 批量收藏（`INSERT IGNORE`，仅对归属自己且未删除的条目生效），供多选工具栏使用 |
| GET `/api/favorites` | 列出收藏的文件夹与文件（跳过已删除），含名称/大小/所在目录/收藏时间 |

## 全局搜索

| 方法 & 路径 | 说明 |
|---|---|
| GET `/api/search?q=&limit=` | 跨全部目录按名称搜索当前用户的文件夹与文件（均跳过已删除）。`q` 支持中文（服务端做 URL 解码），LIKE 元字符 `% _ \` 已转义防误通配；空 `q` 返回空列表；`limit` 默认 50、上限 200。返回 `{items:[{type,id,name,parentId,size}], count}`，文件夹在前 |

---

## 其它

### GET `/healthz` — 健康检查
`{ "code": 0, "data": { "status": "up" } }`

## 错误码约定
`code` 通常等于 HTTP 状态码：`400` 参数错误、`401` 未认证、`404` 不存在、`409` 冲突、`413` 过大、`500` 服务端错误、`503` 依赖服务不可用。
