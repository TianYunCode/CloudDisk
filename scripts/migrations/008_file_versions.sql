-- 文件版本历史: 每次“上传新版本/恢复”前, 归档当前 (hashcode,size) 为一条历史版本。
-- blob 内容寻址且被版本行引用, 回收站彻底删除/清空时的 blob GC 需将本表纳入存活判定。
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
