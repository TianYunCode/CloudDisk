-- 迁移 003: 分享 (公开链接 / 提取码 / 过期 / 下载限次)
-- 用法: mysql -uroot -p1234 test < scripts/migrations/003_shares.sql
USE test;

CREATE TABLE IF NOT EXISTS tbl_share (
  id            BIGINT AUTO_INCREMENT PRIMARY KEY,
  uid           INT           NOT NULL,
  token         VARCHAR(32)   NOT NULL,
  code          VARCHAR(16)   NOT NULL DEFAULT '',   -- 提取码, 空表示无
  file_id       BIGINT        NOT NULL DEFAULT 0,     -- 文件分享
  folder_id     BIGINT        NOT NULL DEFAULT 0,     -- 文件夹分享
  is_folder     TINYINT       NOT NULL DEFAULT 0,
  name          VARCHAR(256)  NOT NULL DEFAULT '',    -- 展示名快照
  expire_at     DATETIME      NULL,                   -- NULL = 永久
  max_downloads INT           NOT NULL DEFAULT 0,      -- 0 = 不限
  downloads     INT           NOT NULL DEFAULT 0,
  views         INT           NOT NULL DEFAULT 0,
  revoked       TINYINT       NOT NULL DEFAULT 0,
  created_at    DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_token (token),
  KEY idx_uid (uid, revoked)
) ENGINE=InnoDB;
