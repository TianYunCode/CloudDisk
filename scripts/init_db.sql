-- CloudDisk 数据库初始化脚本
-- 用法: mysql -uroot -p1234 < scripts/init_db.sql
-- 连接串: mysql://root:1234@localhost/test

ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '1234';
FLUSH PRIVILEGES;

CREATE DATABASE IF NOT EXISTS test DEFAULT CHARACTER SET utf8mb4;
USE test;

-- 用户表: 字段顺序需与 UserService.cpp signin_callback 的 record[0..4] 对应
CREATE TABLE IF NOT EXISTS tbl_user (
  id         INT AUTO_INCREMENT PRIMARY KEY,
  username   VARCHAR(64)  NOT NULL UNIQUE,
  password   VARCHAR(256) NOT NULL,          -- 加盐 sha256
  salt       VARCHAR(64)  NOT NULL DEFAULT '',
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  tomb       INT          NOT NULL DEFAULT 0, -- 软删除标记
  nickname     VARCHAR(64)  NOT NULL DEFAULT '',
  email        VARCHAR(128) NOT NULL DEFAULT '',
  avatar_hash  VARCHAR(64)  NOT NULL DEFAULT '',
  role         TINYINT      NOT NULL DEFAULT 0,  -- 0=普通 1=管理员
  disabled     TINYINT      NOT NULL DEFAULT 0,
  quota        BIGINT       NOT NULL DEFAULT 0,  -- 0=全局默认
  totp_secret  VARCHAR(64)  NOT NULL DEFAULT '',
  totp_enabled TINYINT      NOT NULL DEFAULT 0
) ENGINE=InnoDB;

-- 审计日志
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
) ENGINE=InnoDB;

-- 文件夹表 (物化路径 path: 形如 "/12/15/", 便于一条 SQL 处理整棵子树)
-- dkey: 未删除时为 0; 软删除时置为自身 id, 从而让 UNIQUE 只约束"未删除"的同名项
CREATE TABLE IF NOT EXISTS tbl_folder (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  uid          INT           NOT NULL,
  name         VARCHAR(256)  NOT NULL,
  parent_id    BIGINT        NOT NULL DEFAULT 0,   -- 0 = 根目录
  path         VARCHAR(1024) NOT NULL DEFAULT '/', -- 含自身: "/<ancestors>/<id>/"
  created_at   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  deleted      TINYINT       NOT NULL DEFAULT 0,
  deleted_at   DATETIME      NULL,
  trashed_root TINYINT       NOT NULL DEFAULT 0,   -- 1 = 用户直接删除的项(回收站顶层显示)
  dkey         BIGINT        NOT NULL DEFAULT 0,
  UNIQUE KEY uk_folder (uid, parent_id, name, dkey),
  KEY idx_uid_parent (uid, parent_id),
  KEY idx_uid_deleted (uid, deleted),
  KEY idx_path (uid, path(255))
) ENGINE=InnoDB;

-- 文件元数据表 (内容寻址: hashcode 指向 storage/blobs/<hashcode>)
CREATE TABLE IF NOT EXISTS tbl_file (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  uid          INT             NOT NULL,
  parent_id    BIGINT          NOT NULL DEFAULT 0,   -- 所在文件夹 (0 = 根)
  filename     VARCHAR(256)    NOT NULL,
  hashcode     VARCHAR(128)    NOT NULL,             -- blob 键 (sha256)
  size         BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_update  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted      TINYINT         NOT NULL DEFAULT 0,
  deleted_at   DATETIME        NULL,
  trashed_root TINYINT         NOT NULL DEFAULT 0,
  dkey         BIGINT          NOT NULL DEFAULT 0,
  UNIQUE KEY uk_file (uid, parent_id, filename, dkey),
  KEY idx_uid_parent (uid, parent_id),
  KEY idx_uid_deleted (uid, deleted),
  KEY idx_hash (hashcode)
) ENGINE=InnoDB;

-- 分享表 (公开链接 / 提取码 / 过期 / 下载限次)
CREATE TABLE IF NOT EXISTS tbl_share (
  id            BIGINT AUTO_INCREMENT PRIMARY KEY,
  uid           INT           NOT NULL,
  token         VARCHAR(32)   NOT NULL,
  code          VARCHAR(16)   NOT NULL DEFAULT '',
  file_id       BIGINT        NOT NULL DEFAULT 0,
  folder_id     BIGINT        NOT NULL DEFAULT 0,
  is_folder     TINYINT       NOT NULL DEFAULT 0,
  name          VARCHAR(256)  NOT NULL DEFAULT '',
  expire_at     DATETIME      NULL,
  max_downloads INT           NOT NULL DEFAULT 0,
  downloads     INT           NOT NULL DEFAULT 0,
  views         INT           NOT NULL DEFAULT 0,
  revoked       TINYINT       NOT NULL DEFAULT 0,
  created_at    DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_token (token),
  KEY idx_uid (uid, revoked)
) ENGINE=InnoDB;
-- 004: 分片/断点续传上传会话 + URL 离线下载任务
-- 幂等: 若已存在则跳过

CREATE TABLE IF NOT EXISTS tbl_upload (
    id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    upload_id     VARCHAR(32)  NOT NULL,
    uid           INT          NOT NULL,
    hashcode      VARCHAR(64)  NOT NULL,
    filename      VARCHAR(255) NOT NULL,
    size          BIGINT       NOT NULL DEFAULT 0,
    parent_id     BIGINT       NOT NULL DEFAULT 0,
    chunk_size    INT          NOT NULL DEFAULT 0,
    total_chunks  INT          NOT NULL DEFAULT 0,
    status        TINYINT      NOT NULL DEFAULT 0,   -- 0=进行中 1=完成
    created_at    DATETIME     DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_upload_id (upload_id),
    KEY idx_uid_hash (uid, hashcode, parent_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS tbl_offline (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    uid         INT           NOT NULL,
    url         VARCHAR(2048) NOT NULL,
    filename    VARCHAR(255)  NOT NULL,
    parent_id   BIGINT        NOT NULL DEFAULT 0,
    status      TINYINT       NOT NULL DEFAULT 0,   -- 0=下载中 1=完成 2=失败
    size        BIGINT        NOT NULL DEFAULT 0,
    message     VARCHAR(255)  DEFAULT '',
    file_id     BIGINT        NOT NULL DEFAULT 0,
    created_at  DATETIME      DEFAULT CURRENT_TIMESTAMP,
    KEY idx_uid (uid, status, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

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

CREATE TABLE IF NOT EXISTS tbl_favorite (
    id         BIGINT   NOT NULL AUTO_INCREMENT,
    uid        INT      NOT NULL,
    item_type  TINYINT  NOT NULL DEFAULT 0,
    item_id    BIGINT   NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_fav (uid, item_type, item_id),
    KEY idx_uid (uid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 文件版本历史
CREATE TABLE IF NOT EXISTS tbl_file_version (
  id         BIGINT NOT NULL AUTO_INCREMENT,
  file_id    BIGINT NOT NULL,
  uid        INT NOT NULL,
  hashcode   VARCHAR(128) NOT NULL,
  size       BIGINT UNSIGNED NOT NULL DEFAULT 0,
  note       VARCHAR(256) NOT NULL DEFAULT '',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_file (file_id, id),
  KEY idx_uid (uid),
  KEY idx_hash (hashcode)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
