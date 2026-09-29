// =============================================================================
// AuditAction.h —— 审计动作名的单一真源 (前后端契约)
//
// 后端 audit_log(...) 只应使用本文件中的常量, 不再散落字符串字面量:
//   · 编译期防止拼写错误
//   · 一处枚举全部动作, 便于审阅与检索
//   · 前端 web/js/core/activity.js 的 ACT_META 必须覆盖这里的每个动作
//     (由 tests/e2e/audit_contract_validate.mjs 自动校验, 防止前后端漂移)
//
// 纯 constexpr 常量, 无任何依赖。
// =============================================================================
#ifndef CLOUDDISK_AUDIT_ACTION_H
#define CLOUDDISK_AUDIT_ACTION_H

namespace AuditAction {

// ---- 认证 ----
inline constexpr const char* Register          = "register";
inline constexpr const char* Login             = "login";
inline constexpr const char* LoginFail         = "login_fail";
inline constexpr const char* LoginBlocked      = "login_blocked";
inline constexpr const char* Login2faFail      = "login_2fa_fail";
inline constexpr const char* LoginDisabled     = "login_disabled";

// ---- 账户 ----
inline constexpr const char* PasswordChange    = "password_change";
inline constexpr const char* PasswordChangeFail = "password_change_fail";
inline constexpr const char* ProfileUpdate     = "profile_update";
inline constexpr const char* AvatarUpdate      = "avatar_update";
inline constexpr const char* TwoFaEnable       = "2fa_enable";
inline constexpr const char* TwoFaDisable      = "2fa_disable";
inline constexpr const char* TokenCreate       = "token_create";
inline constexpr const char* TokenRevoke       = "token_revoke";

// ---- 文件 / 文件夹 ----
inline constexpr const char* FileUpload        = "file_upload";
inline constexpr const char* FileDelete        = "file_delete";
inline constexpr const char* FileRestore       = "file_restore";
inline constexpr const char* FileRename        = "file_rename";
inline constexpr const char* FolderCreate      = "folder_create";

// ---- 分享 ----
inline constexpr const char* ShareCreate       = "share_create";

// ---- 管理员 ----
inline constexpr const char* AdminSetRole      = "admin_set_role";
inline constexpr const char* AdminSetQuota     = "admin_set_quota";
inline constexpr const char* AdminSetDisabled  = "admin_set_disabled";

} // namespace AuditAction

#endif // CLOUDDISK_AUDIT_ACTION_H
