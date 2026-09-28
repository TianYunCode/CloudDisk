-- 迁移 002: 文件夹 + 回收站
-- 用法: mysql -uroot -p1234 test < scripts/migrations/002_folders_trash.sql
USE test;

CREATE TABLE IF NOT EXISTS tbl_folder (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  uid          INT           NOT NULL,
  name         VARCHAR(256)  NOT NULL,
  parent_id    BIGINT        NOT NULL DEFAULT 0,
  path         VARCHAR(1024) NOT NULL DEFAULT '/',
  created_at   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  deleted      TINYINT       NOT NULL DEFAULT 0,
  deleted_at   DATETIME      NULL,
  trashed_root TINYINT       NOT NULL DEFAULT 0,
  dkey         BIGINT        NOT NULL DEFAULT 0,
  UNIQUE KEY uk_folder (uid, parent_id, name, dkey),
  KEY idx_uid_parent (uid, parent_id),
  KEY idx_uid_deleted (uid, deleted),
  KEY idx_path (uid, path(255))
) ENGINE=InnoDB;

ALTER TABLE tbl_file
  ADD COLUMN parent_id    BIGINT  NOT NULL DEFAULT 0 AFTER uid,
  ADD COLUMN deleted      TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN deleted_at   DATETIME NULL,
  ADD COLUMN trashed_root TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN dkey         BIGINT  NOT NULL DEFAULT 0;

ALTER TABLE tbl_file DROP INDEX uk_uid_filename;
ALTER TABLE tbl_file ADD UNIQUE KEY uk_file (uid, parent_id, filename, dkey);
ALTER TABLE tbl_file ADD KEY idx_uid_parent (uid, parent_id);
ALTER TABLE tbl_file ADD KEY idx_uid_deleted (uid, deleted);
