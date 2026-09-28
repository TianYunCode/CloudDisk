-- 007_favorites.sql — 收藏夹 (文件 / 文件夹)
-- 幂等: 可重复执行
CREATE TABLE IF NOT EXISTS tbl_favorite (
  id         BIGINT   NOT NULL AUTO_INCREMENT,
  uid        INT      NOT NULL,
  item_type  TINYINT  NOT NULL DEFAULT 0,   -- 0=文件 1=文件夹
  item_id    BIGINT   NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_fav (uid, item_type, item_id),
  KEY idx_uid (uid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
