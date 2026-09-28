# CloudVault iOS 客户端

> 状态：**规划中（占位）**。当前阶段集中打磨 Web 端，iOS 原生客户端将在 Web 端架构稳定后启动。

## 定位

CloudVault 云库企业云盘的 iOS 原生客户端，复用后端已提供的统一 REST API（见 [`docs/API.md`](../../docs/API.md)），实现与 Web 端一致的能力：鉴权、文件浏览与增删改、上传/下载、分享、回收站、收藏、标签、活动日志、存储统计等。

## 计划技术栈（暂定）

| 关注点 | 选型（暂定） |
|---|---|
| 语言 | Swift |
| UI | SwiftUI |
| 架构 | MVVM + Clean Architecture（Data / Domain / Presentation 分层） |
| 网络 | URLSession + async/await（对接 `/api/*`，Bearer Token） |
| 本地缓存 | Core Data / SwiftData + Keychain（令牌） |
| 依赖管理 | Swift Package Manager |

## 与后端的契约

- Base URL 可配置，默认对接网关 `http(s)://<host>:8888`。
- 认证：登录获取 JWT，后续请求携带 `Authorization: Bearer <token>`；支持两步验证（TOTP）与 API 令牌（`cvt_` 前缀）。
- 统一响应信封：`{ "code": 0, "message": "ok", "data": ... }`。

## 目录规划（实现阶段再落地）

```
ios/
├── CloudVault/
│   ├── Data/                # 网络客户端、DTO、Repository 实现
│   ├── Domain/              # 领域模型、UseCase、Repository 协议
│   └── Presentation/        # SwiftUI 视图、ViewModel、路由
├── CloudVaultTests/
└── Package.swift
```

## 现状

此目录目前仅包含本 README，作为整体项目分层结构的占位。具体实现将在后续迭代中补充。
