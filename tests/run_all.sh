#!/usr/bin/env bash
# =============================================================================
# run_all.sh —— 一键运行全部测试 (4 层)
#
#   1. C++ 单元测试        bin/unit_tests            (无需任何服务)
#   2. 前端纯逻辑 + 契约    tests/e2e/*_validate.mjs  (无需任何服务)
#   3. 集成测试            tests/integration.sh      (需 server + UserService + MySQL)
#   4. 浏览器 e2e          tests/e2e/e2e*.mjs        (需 server; Playwright/Chromium)
#
# 用法:
#   bash tests/run_all.sh           # 全部 4 层
#   bash tests/run_all.sh --fast    # 只跑 1+2 层 (秒级, 无需服务)
#   bash tests/run_all.sh --no-e2e  # 跑 1+2+3 层 (跳过浏览器 e2e)
#
# 任一层失败则整体以非零码退出, 可直接用作 CI / pre-push 门禁。
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/usr/local/lib}"

MODE="all"
case "${1:-}" in
  --fast)   MODE="fast" ;;
  --no-e2e) MODE="noe2e" ;;
  "")       ;;
  *) echo "未知参数: $1 (可用: --fast | --no-e2e)"; exit 2 ;;
esac

# 浏览器 e2e 之间需要间隔: 连续启动 Chromium 会造成偶发抖动 (非代码回归)
E2E_SPACING="${E2E_SPACING:-3}"

FAILED=()
PASSED=()

section() { printf '\n\033[1;36m==== %s ====\033[0m\n' "$1"; }
ok()      { PASSED+=("$1"); printf '  \033[32m✓\033[0m %s\n' "$1"; }
bad()     { FAILED+=("$1"); printf '  \033[31m✗\033[0m %s\n' "$1"; }

# ---------------------------------------------------------------- 1. C++ 单元测试
section "1/4  C++ 单元测试 (bin/unit_tests)"
if [ ! -x bin/unit_tests ]; then
  bad "bin/unit_tests 不存在 (请先构建: cd build && cmake .. && make unit_tests)"
else
  out="$(./bin/unit_tests 2>&1)"; rc=$?
  echo "$out" | tail -2
  if [ $rc -eq 0 ] && ! echo "$out" | grep -qE '[1-9][0-9]* 失败'; then ok "C++ 单元测试"; else bad "C++ 单元测试 (rc=$rc)"; fi
fi

# ------------------------------------------------- 2. 前端纯逻辑 + 前后端契约测试
section "2/4  前端纯逻辑 + 契约测试 (Node, 无需服务)"
NODE_SUITES=(
  format_validate preview_validate validate_validate pager_validate
  activity_validate bus_validate crumbs_validate
  audit_contract_validate route_contract_validate errcode_contract_validate
  config_contract_validate humansize_contract_validate
  qr_validate
)
for t in "${NODE_SUITES[@]}"; do
  f="tests/e2e/$t.mjs"
  if [ ! -f "$f" ]; then bad "$t (文件缺失)"; continue; fi
  if node "$f" >/dev/null 2>&1; then ok "$t"; else bad "$t"; fi
done

# 服务可用性预检 (第 3/4 层需要)
SERVER_UP=0
if curl -sf --max-time 3 http://127.0.0.1:8888/healthz >/dev/null 2>&1; then SERVER_UP=1; fi

if [ "$MODE" = "fast" ]; then
  section "已指定 --fast, 跳过集成测试与浏览器 e2e"
else
  # ------------------------------------------------------------- 3. 集成测试
  section "3/4  集成测试 (tests/integration.sh)"
  if [ "$SERVER_UP" -ne 1 ]; then
    bad "集成测试 (server 未运行: 需 bin/server + bin/UserService + MySQL)"
  else
    out="$(bash tests/integration.sh 2>&1)"; rc=$?
    echo "$out" | tail -3
    if [ $rc -eq 0 ] && ! echo "$out" | grep -qE '[1-9][0-9]* 失败'; then ok "集成测试"; else bad "集成测试 (rc=$rc)"; fi
  fi

  # ---------------------------------------------------------- 4. 浏览器 e2e
  section "4/4  浏览器 e2e (Playwright/Chromium)"
  if [ "$MODE" = "noe2e" ]; then
    printf '  已指定 --no-e2e, 跳过\n'
  elif [ "$SERVER_UP" -ne 1 ]; then
    bad "浏览器 e2e (server 未运行)"
  else
    for f in tests/e2e/e2e*.mjs; do
      name="$(basename "$f")"
      out="$(node "$f" 2>&1)"; rc=$?
      if [ $rc -eq 0 ] && ! echo "$out" | grep -qiE '✗|失败: [1-9]'; then
        ok "$name"
      else
        bad "$name (rc=$rc; 若单独重跑通过则为 Chromium 启动抖动, 可增大 E2E_SPACING)"
      fi
      sleep "$E2E_SPACING"
    done
  fi
fi

# ------------------------------------------------------------------- 汇总
section "汇总"
printf '通过 %d 项, 失败 %d 项\n' "${#PASSED[@]}" "${#FAILED[@]}"
if [ "${#FAILED[@]}" -gt 0 ]; then
  printf '\n\033[31m失败项:\033[0m\n'
  for x in "${FAILED[@]}"; do printf '  - %s\n' "$x"; done
  exit 1
fi
printf '\n\033[32m✅ 全部通过\033[0m\n'
exit 0
