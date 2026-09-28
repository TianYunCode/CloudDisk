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
