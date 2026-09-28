-- 006_api_tokens.sql — 个人访问令牌 (API Token) + WebDAV 基础
-- 幂等: 可重复执行
CREATE TABLE IF NOT EXISTS tbl_token (
  id           BIGINT       NOT NULL AUTO_INCREMENT,
  uid          INT          NOT NULL,
  name         VARCHAR(64)  NOT NULL DEFAULT '',
  token_hash   VARCHAR(64)  NOT NULL,
  prefix       VARCHAR(16)  NOT NULL DEFAULT '',
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_used_at DATETIME     NULL,
  expires_at   DATETIME     NULL,
  revoked      TINYINT      NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_token_hash (token_hash),
  KEY idx_uid (uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
