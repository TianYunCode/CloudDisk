-- 文件标签体系: 用户自定义彩色标签, 多对多关联文件。
CREATE TABLE IF NOT EXISTS tbl_tag (
  id         BIGINT NOT NULL AUTO_INCREMENT,
  uid        INT NOT NULL,
  name       VARCHAR(64) NOT NULL,
  color      VARCHAR(16) NOT NULL DEFAULT '#6366f1',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_tag (uid, name),
  KEY idx_uid (uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS tbl_file_tag (
  id         BIGINT NOT NULL AUTO_INCREMENT,
  tag_id     BIGINT NOT NULL,
  file_id    BIGINT NOT NULL,
  uid        INT NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_file_tag (tag_id, file_id),
  KEY idx_file (file_id),
  KEY idx_uid (uid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
