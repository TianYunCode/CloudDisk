#!/usr/bin/env bash
# 启动 CloudDisk 的全部依赖服务和应用进程。
# 前提: 已按 BUILD.md 安装依赖并 cmake 编译出 server / UserService (/ backup)。
set -u

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${CLOUDDISK_RUN_DIR:-$HOME/clouddisk-run}"
LOG_DIR="$RUN_DIR/logs"
export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "$LOG_DIR"

echo "[1/5] 检查/启动 MySQL ..."
sudo systemctl start mysql 2>/dev/null || sudo service mysql start 2>/dev/null || true

echo "[2/5] 检查/启动 RabbitMQ ..."
sudo systemctl start rabbitmq-server 2>/dev/null || sudo service rabbitmq-server start 2>/dev/null || true

echo "[2b] 确保 RabbitMQ 拓扑 (oss.direct -> oss.queue, key=oss) ..."
# management 插件与 durable 拓扑会持久化, 一般只需首次执行; 这里做幂等且短超时的 best-effort 声明。
# 若 mgmt API (:15672) 未就绪则跳过, 不阻塞核心服务启动。
if curl -s -m 3 -u guest:guest http://127.0.0.1:15672/api/overview >/dev/null 2>&1; then
  RB="curl -s -m 5 -u guest:guest"
  $RB -X PUT  http://127.0.0.1:15672/api/exchanges/%2f/oss.direct -H 'content-type:application/json' -d '{"type":"direct","durable":true}' -o /dev/null || true
  $RB -X PUT  http://127.0.0.1:15672/api/queues/%2f/oss.queue      -H 'content-type:application/json' -d '{"durable":true}' -o /dev/null || true
  $RB -X POST http://127.0.0.1:15672/api/bindings/%2f/e/oss.direct/q/oss.queue -H 'content-type:application/json' -d '{"routing_key":"oss"}' -o /dev/null || true
  echo "    拓扑已声明"
else
  echo "    mgmt API 未就绪, 跳过 (首次请手动: sudo rabbitmq-plugins enable rabbitmq_management)"
fi

echo "[3/5] 启动 Consul (dev) ..."
if ! curl -s -m 3 http://127.0.0.1:8500/v1/status/leader >/dev/null 2>&1; then
  setsid nohup consul agent -dev -client=127.0.0.1 -log-level=warn > "$LOG_DIR/consul.log" 2>&1 < /dev/null &
  sleep 4
fi
curl -s http://127.0.0.1:8500/v1/status/leader >/dev/null && echo "    Consul OK" || echo "    Consul 未就绪"

echo "[4/5] 启动 UserService (SRPC :1414, 注册到 Consul) ..."
cd "$PROJ_DIR"
setsid nohup ./bin/UserService > "$LOG_DIR/userservice.log" 2>&1 < /dev/null &
ok=""
for i in 1 2 3 4 5 6; do
  sleep 2
  if curl -s -m 3 "http://127.0.0.1:8500/v1/health/service/UserService?passing=true" | grep -qE '"Port":[[:space:]]*1414'; then ok=1; break; fi
done
[ -n "$ok" ] && echo "    UserService 已注册 (passing)" || echo "    UserService 注册检查失败, 见 $LOG_DIR/userservice.log"

echo "[5/5] 启动 HTTP 网关 server (:8888) ..."
setsid nohup ./bin/server > "$LOG_DIR/server.log" 2>&1 < /dev/null &
sleep 3
ss -ltn 2>/dev/null | grep -q ':8888' && echo "    server 监听 :8888" || echo "    server 未监听, 见 $LOG_DIR/server.log"

# 可选: 若配置了 OSS 凭据 (scripts/oss.env), 顺带启动 backup 异步备份进程
if [ -f "$PROJ_DIR/scripts/oss.env" ]; then
  echo "[+]  检测到 scripts/oss.env, 启动 backup (OSS 异步备份) ..."
  set -a; . "$PROJ_DIR/scripts/oss.env"; set +a
  setsid nohup ./bin/backup > "$LOG_DIR/backup.log" 2>&1 < /dev/null &
  sleep 1
  echo "    backup 已启动 (bucket: ${OSS_BUCKET:-未设置}), 见 $LOG_DIR/backup.log"
fi

echo
echo "完成。打开 http://127.0.0.1:8888/ 开始使用 (注册 / 登录 / 上传)。"
echo "日志目录: $LOG_DIR"
echo "提示: 未配置 scripts/oss.env 时不启动 backup; OSS 备份需有效的阿里云凭据。"
