#!/usr/bin/env bash
# CloudVault 容器入口: 等待依赖就绪 -> 初始化库表 -> 启动三个进程
set -euo pipefail

MYSQL_HOST="${MYSQL_HOST:-mysql}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-1234}"
MYSQL_DATABASE="${MYSQL_DATABASE:-test}"

wait_for() {
  local host="$1" port="$2" name="$3"
  echo "[entrypoint] 等待 ${name} (${host}:${port}) ..."
  for _ in $(seq 1 60); do
    if nc -z "$host" "$port" 2>/dev/null; then echo "[entrypoint] ${name} 就绪"; return 0; fi
    sleep 2
  done
  echo "[entrypoint] 等待 ${name} 超时" >&2; return 1
}

wait_for "$MYSQL_HOST" "$MYSQL_PORT" MySQL
wait_for "${CONSUL_HOST:-consul}" "${CONSUL_PORT:-8500}" Consul || true
wait_for "${RABBITMQ_HOST:-rabbitmq}" "${RABBITMQ_PORT:-5672}" RabbitMQ || true

echo "[entrypoint] 初始化数据库结构 ..."
mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" -uroot -p"$MYSQL_ROOT_PASSWORD" \
      "$MYSQL_DATABASE" < /app/scripts/init_db.sql || echo "[entrypoint] 初始化跳过/已存在"

cd /app
export LD_LIBRARY_PATH=/usr/local/lib

echo "[entrypoint] 启动 UserService ..."
./UserService >/app/UserService.log 2>&1 &
echo "[entrypoint] 启动 backup ..."
./backup >/app/backup.log 2>&1 &

echo "[entrypoint] 启动 server (前台) ..."
exec ./bin/server
