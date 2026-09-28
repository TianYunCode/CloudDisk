#!/usr/bin/env bash
# =============================================================================
# run.sh —— 一键编译并运行 CloudVault
#   用法:
#     bash run.sh            # 编译 + (首次)建库 + 启动全部服务
#     bash run.sh restart    # 停止 -> 重新编译 -> 启动
#     bash run.sh stop       # 停止应用进程
#     bash run.sh --no-build # 跳过编译, 直接启动
# =============================================================================
set -euo pipefail
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJ_DIR"

# ---- 第三方库的编译/运行环境 (依赖装在 /usr/local) ----
export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"
export LIBRARY_PATH="/usr/local/lib:${LIBRARY_PATH:-}"
export CPLUS_INCLUDE_PATH="/usr/local/include:${CPLUS_INCLUDE_PATH:-}"

cmd="${1:-run}"

if [ "$cmd" = "stop" ]; then
  bash scripts/stop.sh
  exit 0
fi
if [ "$cmd" = "restart" ]; then
  bash scripts/stop.sh || true
  sleep 1
  cmd="run"
fi

# ---- 1) 编译 ----
if [ "$cmd" != "--no-build" ]; then
  echo "==> [1/3] 编译 (server / UserService / backup) ..."
  mkdir -p build
  if ! (cd build && cmake .. >/tmp/cv_cmake.log 2>&1 && make -j"$(nproc)" >/tmp/cv_make.log 2>&1); then
    echo "    [x] 编译失败, 最后的输出:"
    tail -n 25 /tmp/cv_make.log /tmp/cv_cmake.log
    exit 1
  fi
  echo "    编译完成: ./server ./UserService ./backup"
else
  echo "==> [1/3] 跳过编译 (--no-build)"
fi

# ---- 2) 初始化数据库 (仅首次) ----
echo "==> [2/3] 检查数据库 ..."
# 从 config.json 解析 mysql 连接 (user/pass/db)
read -r DBU DBP DBNAME < <(python3 - <<'PY'
import json, re
try:
    url = json.load(open("config.json")).get("mysql_url", "")
except Exception:
    url = ""
m = re.match(r"mysql://([^:]+):([^@]*)@[^/]+/(\w+)", url)
if m: print(m.group(1), m.group(2), m.group(3))
else:  print("root", "1234", "test")
PY
)
if mysql -u"$DBU" -p"$DBP" -e "SELECT 1 FROM ${DBNAME}.tbl_user LIMIT 1" >/dev/null 2>&1; then
  echo "    数据库已就绪 (${DBNAME})"
else
  echo "    未检测到数据表, 执行 scripts/init_db.sql ..."
  if mysql -u"$DBU" -p"$DBP" < scripts/init_db.sql >/dev/null 2>&1 \
     || sudo mysql < scripts/init_db.sql >/dev/null 2>&1; then
    echo "    建库完成"
  else
    echo "    [!] 自动建库失败, 请手动执行: mysql -u$DBU -p$DBP < scripts/init_db.sql"
  fi
fi

# ---- 3) 启动 ----
echo "==> [3/3] 启动服务 ..."
bash scripts/start.sh

# 打印可访问地址 (本机 + 局域网)
IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo
echo "本机访问:   http://127.0.0.1:8888/"
[ -n "$IP" ] && echo "局域网访问: http://$IP:8888/   (供其它电脑访问)"
