-- 005: 账户资料 / 角色 / 配额 / 2FA + 审计日志
-- 注意: MySQL 8 不支持 ADD COLUMN IF NOT EXISTS, 本迁移仅执行一次。

ALTER TABLE tbl_user
    ADD COLUMN nickname     VARCHAR(64)  NOT NULL DEFAULT '',
    ADD COLUMN email        VARCHAR(128) NOT NULL DEFAULT '',
    ADD COLUMN avatar_hash  VARCHAR(64)  NOT NULL DEFAULT '',
    ADD COLUMN role         TINYINT      NOT NULL DEFAULT 0,   -- 0=普通用户 1=管理员
    ADD COLUMN disabled     TINYINT      NOT NULL DEFAULT 0,   -- 1=已禁用登录
    ADD COLUMN quota        BIGINT       NOT NULL DEFAULT 0,   -- 0=使用全局默认配额
    ADD COLUMN totp_secret  VARCHAR(64)  NOT NULL DEFAULT '',
    ADD COLUMN totp_enabled TINYINT      NOT NULL DEFAULT 0;

-- 引导管理员: 将最小 id 的用户设为管理员
UPDATE tbl_user SET role=1 WHERE id=(SELECT mid FROM (SELECT MIN(id) AS mid FROM tbl_user) t);

CREATE TABLE IF NOT EXISTS tbl_audit (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    uid        INT          NOT NULL DEFAULT 0,
    username   VARCHAR(64)  NOT NULL DEFAULT '',
    action     VARCHAR(48)  NOT NULL,
    detail     VARCHAR(512) NOT NULL DEFAULT '',
    ip         VARCHAR(64)  NOT NULL DEFAULT '',
    created_at DATETIME     DEFAULT CURRENT_TIMESTAMP,
    KEY idx_uid (uid, id),
    KEY idx_action (action, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
