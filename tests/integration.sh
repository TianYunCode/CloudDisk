#!/usr/bin/env bash
# =============================================================================
# CloudVault 集成测试 —— 覆盖全部 API 与边界/安全用例
# 用法: bash tests/integration.sh [BASE_URL]
#   默认 BASE_URL=http://127.0.0.1:8888
# 需要: curl, python3。要求后端(server + UserService + consul + mysql)已启动。
# =============================================================================
set -u
BASE="${1:-http://127.0.0.1:8888}"
PASS=0; FAIL=0
U="t_$(date +%s)_$RANDOM"      # 随机用户名, 保证可重复运行
PW="secret123"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

jq() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

ok()   { PASS=$((PASS+1)); printf "  \033[32m✓\033[0m %s\n" "$1"; }
bad()  { FAIL=$((FAIL+1)); printf "  \033[31m✗ %s\033[0m\n" "$1"; [ -n "${2:-}" ] && echo "      → $2"; }

# assert_code METHOD URL EXPECTED [DATA] [AUTH] [--form]
assert_code() {
  local m="$1" url="$2" exp="$3" data="${4:-}" auth="${5:-}" form="${6:-}"
  local args=(-s -o /dev/null -w "%{http_code}" -X "$m")
  [ -n "$auth" ] && args+=(-H "Authorization: Bearer $auth")
  if [ "$form" = "--form" ]; then args+=($data)
  elif [ -n "$data" ]; then args+=(-H "Content-Type: application/json" -d "$data"); fi
  local code; code=$(curl "${args[@]}" "$BASE$url")
  if [ "$code" = "$exp" ]; then ok "$m $url -> $code"; else bad "$m $url 期望 $exp 实际 $code"; fi
}

echo "== 健康检查 =="
assert_code GET /healthz 200

echo "== 注册 =="
assert_code POST /api/auth/register 200 "{\"username\":\"$U\",\"password\":\"$PW\"}"
assert_code POST /api/auth/register 409 "{\"username\":\"$U\",\"password\":\"$PW\"}"          # 重复
assert_code POST /api/auth/register 400 "{\"username\":\"ab\",\"password\":\"$PW\"}"          # 用户名太短
assert_code POST /api/auth/register 400 "{\"username\":\"okname\",\"password\":\"123\"}"      # 密码太短
assert_code POST /api/auth/register 400 '{"username":"bad name!","password":"secret123"}'     # 非法字符

echo "== 登录 =="
assert_code POST /api/auth/login 401 "{\"username\":\"$U\",\"password\":\"wrongpw\"}"
assert_code POST /api/auth/login 401 '{"username":"nobody_here_xyz","password":"whatever1"}'
R=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$U\",\"password\":\"$PW\"}")
TOKEN=$(echo "$R" | jq "d['data']['token']")
[ -n "$TOKEN" ] && [ "$TOKEN" != "None" ] && ok "登录返回 token" || bad "登录未返回 token" "$R"

echo "== 鉴权 =="
assert_code GET /api/user/info 401
assert_code GET /api/user/info 401 "" "not.a.valid.token"
assert_code GET /api/user/info 200 "" "$TOKEN"
QUOTA=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/user/info" | jq "d['data']['quota']")
[ -n "$QUOTA" ] && [ "$QUOTA" -gt 0 ] 2>/dev/null && ok "user/info 返回配额 quota=$QUOTA" || bad "user/info 未返回有效 quota" "$QUOTA"

echo "== 上传 =="
echo "content-alpha-$U" > "$TMP/alpha.txt"
echo "content-beta-$U"  > "$TMP/beta.txt"
UP=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload" -F "a=@$TMP/alpha.txt" -F "b=@$TMP/beta.txt")
CNT=$(echo "$UP" | jq "len(d['data']['files'])")
[ "$CNT" = "2" ] && ok "多文件上传返回 2 条" || bad "多文件上传结果异常" "$UP"
HASH=$(sha256sum "$TMP/alpha.txt" | awk '{print $1}')

echo "== 秒传 =="
INS=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/instant" -H 'Content-Type: application/json' -d "{\"filename\":\"alpha_copy.txt\",\"hash\":\"$HASH\",\"size\":20}")
[ "$(echo "$INS" | jq "d['data']['instant']")" = "True" ] && ok "相同内容 -> 秒传命中" || bad "秒传应命中" "$INS"
MISS=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/instant" -H 'Content-Type: application/json' -d '{"filename":"ghost.txt","hash":"0000000000000000000000000000000000000000000000000000000000000000","size":1}')
[ "$(echo "$MISS" | jq "d['data']['instant']")" = "False" ] && ok "未知内容 -> 秒传未命中" || bad "秒传应未命中" "$MISS"
assert_code POST /api/file/instant 400 '{"filename":"x","hash":"not-hex!!"}' "$TOKEN"       # 非法 hash

echo "== 列表 / 搜索 / 排序 / 分页 =="
LIST=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?limit=10")
[ "$(echo "$LIST" | jq "d['data']['total']")" = "3" ] && ok "列表 total=3" || bad "列表 total 异常" "$LIST"
S=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?keyword=copy")
[ "$(echo "$S" | jq "d['data']['total']")" = "1" ] && ok "搜索 copy 命中 1" || bad "搜索结果异常" "$S"
P=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?limit=2&offset=0")
[ "$(echo "$P" | jq "len(d['data']['items'])")" = "2" ] && ok "分页 limit=2 返回 2 条" || bad "分页异常" "$P"

echo "== SQL 注入防护 =="
assert_code GET "/api/file/list?keyword=%27%20OR%201%3D1--" 200 "" "$TOKEN"
assert_code POST /api/auth/login 401 "{\"username\":\"x' OR '1'='1\",\"password\":\"y\"}"
# 注入没有破坏数据: 该用户文件数应仍为 3
AFTER=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?limit=100" | jq "d['data']['total']")
[ "$AFTER" = "3" ] && ok "注入尝试后数据完好 (total=3)" || bad "数据被注入影响" "total=$AFTER"

echo "== 下载 (按 id) =="
fid_of() { curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=${2:-0}&limit=200" | python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(next((i['id'] for i in d['items'] if i['filename']=='$1'),''))" 2>/dev/null; }
folderid_of() { curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=${2:-0}&limit=200" | python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(next((i['id'] for i in d['folders'] if i['name']=='$1'),''))" 2>/dev/null; }
AID=$(fid_of alpha.txt)
DL=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/download?id=$AID")
[ "$DL" = "content-alpha-$U" ] && ok "下载内容正确" || bad "下载内容不符" "$DL"
assert_code GET "/api/file/download?id=999999999" 404 "" "$TOKEN"
assert_code GET "/api/file/download?id=$AID" 401          # 无 token

echo "== 重命名 (按 id) =="
BID=$(fid_of beta.txt)
assert_code POST /api/file/rename 200 "{\"id\":$BID,\"newname\":\"beta_renamed.txt\"}" "$TOKEN"
assert_code POST /api/file/rename 409 "{\"id\":$BID,\"newname\":\"alpha.txt\"}" "$TOKEN"   # 目标已存在

echo "== 软删除 + Blob 引用计数 + 回收站 =="
CID=$(fid_of alpha_copy.txt)
# alpha.txt 与 alpha_copy.txt 指向同一 blob; 软删其一, blob 应保留
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/fs/delete" -H 'Content-Type: application/json' -d "{\"fileIds\":[$CID],\"folderIds\":[]}" >/dev/null
STILL=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/download?id=$AID")
[ "$STILL" = "content-alpha-$U" ] && ok "软删副本后原文件仍可下载 (blob 保留)" || bad "blob 被误删" "$STILL"
# 回收站应含 alpha_copy.txt
TN=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/trash/list" | jq "len(d['data']['files'])")
[ "$TN" -ge 1 ] 2>/dev/null && ok "回收站含软删文件" || bad "回收站应含软删文件" "$TN"
# 恢复
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/trash/restore" -H 'Content-Type: application/json' -d "{\"fileIds\":[$CID],\"folderIds\":[]}" >/dev/null
RESTORED=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?limit=100" | jq "d['data']['total']")
[ "$RESTORED" = "3" ] && ok "恢复后根目录文件数=3" || bad "恢复异常" "total=$RESTORED"

echo "== 文件夹: 创建 / 列表 / 面包屑 / 上传 / 移动 =="
FOLDER=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/folder/create" -H 'Content-Type: application/json' -d '{"name":"MyDocs","parentId":0}')
DOCID=$(echo "$FOLDER" | jq "d['data']['id']")
[ -n "$DOCID" ] && [ "$DOCID" != "None" ] && ok "创建文件夹 MyDocs (id=$DOCID)" || bad "创建文件夹失败" "$FOLDER"
assert_code POST /api/folder/create 409 "{\"name\":\"MyDocs\",\"parentId\":0}" "$TOKEN"   # 同名冲突
ROOTF=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=0" | jq "len(d['data']['folders'])")
[ "$ROOTF" -ge 1 ] 2>/dev/null && ok "根目录含文件夹" || bad "根目录应含文件夹" "$ROOTF"
# 上传到文件夹
echo "in-folder-$U" > "$TMP/inf.txt"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=$DOCID" -F "f=@$TMP/inf.txt" >/dev/null
INF=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=$DOCID")
[ "$(echo "$INF" | jq "len(d['data']['items'])")" = "1" ] && ok "文件夹内含 1 文件" || bad "文件夹上传异常" "$INF"
[ "$(echo "$INF" | jq "d['data']['breadcrumb'][0]['name']")" = "MyDocs" ] && ok "面包屑正确" || bad "面包屑异常" "$INF"
# 子文件夹
SUB=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/folder/create" -H 'Content-Type: application/json' -d "{\"name\":\"Sub\",\"parentId\":$DOCID}")
SUBID=$(echo "$SUB" | jq "d['data']['id']")
# 移动根目录 alpha.txt 到 MyDocs
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/fs/move" -H 'Content-Type: application/json' -d "{\"fileIds\":[$AID],\"folderIds\":[],\"targetId\":$DOCID}" >/dev/null
MOVED=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=$DOCID" | jq "len(d['data']['items'])")
[ "$MOVED" = "2" ] && ok "移动后 MyDocs 含 2 文件" || bad "移动异常" "count=$MOVED"
# 不能移动到自身子目录
assert_code POST /api/fs/move 400 "{\"fileIds\":[],\"folderIds\":[$DOCID],\"targetId\":$SUBID}" "$TOKEN"

echo "== 文件夹递归删除 / 恢复 / 彻底删除 =="
UNIQ="folder-blob-$U-$RANDOM"; echo "$UNIQ" > "$TMP/fb.txt"
FBHASH=$(sha256sum "$TMP/fb.txt" | awk '{print $1}')
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=$SUBID" -F "f=@$TMP/fb.txt" >/dev/null
BLOBDIR="$(dirname "$0")/../storage/blobs"
[ -f "$BLOBDIR/$FBHASH" ] && ok "子文件夹内文件 blob 已落地" || bad "blob 未落地" "$FBHASH"
# 递归软删 MyDocs
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/fs/delete" -H 'Content-Type: application/json' -d "{\"fileIds\":[],\"folderIds\":[$DOCID]}" >/dev/null
GONE=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=0" | jq "len(d['data']['folders'])")
[ "$GONE" = "0" ] && ok "递归软删后根目录无文件夹" || bad "递归软删异常" "folders=$GONE"
[ -f "$BLOBDIR/$FBHASH" ] && ok "软删期间 blob 保留" || bad "软删不应删除 blob" "$FBHASH"
# 恢复整棵子树
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/trash/restore" -H 'Content-Type: application/json' -d "{\"fileIds\":[],\"folderIds\":[$DOCID]}" >/dev/null
RSUB=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/list?parentId=$SUBID" | jq "len(d['data']['items'])")
[ "$RSUB" = "1" ] && ok "恢复后子文件夹文件回归" || bad "恢复子树异常" "count=$RSUB"
# 再删 + 彻底删除, 校验 blob 回收
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/fs/delete" -H 'Content-Type: application/json' -d "{\"folderIds\":[$DOCID]}" >/dev/null
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/trash/delete" -H 'Content-Type: application/json' -d "{\"folderIds\":[$DOCID]}" >/dev/null
[ ! -f "$BLOBDIR/$FBHASH" ] && ok "彻底删除后无引用 blob 被回收" || bad "blob 未回收" "$FBHASH"
TRASHEMPTY=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/trash/list" | jq "len(d['data']['folders'])")
[ "$TRASHEMPTY" = "0" ] && ok "彻底删除后回收站无该文件夹" || bad "回收站残留" "$TRASHEMPTY"

echo "== 分享: 文件 (提取码 / 过期 / 限次) =="
echo "share-$U" > "$TMP/share.txt"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=0" -F "f=@$TMP/share.txt" >/dev/null
SFID=$(fid_of share.txt)
SC=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/share/create" -H 'Content-Type: application/json' -d "{\"fileId\":$SFID,\"code\":\"pass12\",\"maxDownloads\":1}")
STOK=$(echo "$SC" | jq "d['data']['token']")
[ -n "$STOK" ] && [ "$STOK" != "None" ] && ok "创建文件分享 (token=$STOK)" || bad "创建分享失败" "$SC"
NEEDCODE=$(curl -s "$BASE/api/share/info?token=$STOK" | jq "d['data']['needCode']")
[ "$NEEDCODE" = "True" ] && ok "info 标记需要提取码" || bad "info needCode 异常" "$NEEDCODE"
assert_code GET "/api/share/browse?token=$STOK&code=wrong" 403
BF=$(curl -s "$BASE/api/share/browse?token=$STOK&code=pass12" | jq "d['data']['files'][0]['filename']")
[ "$BF" = "share.txt" ] && ok "正确提取码可浏览" || bad "browse 异常" "$BF"
DLC=$(curl -s "$BASE/api/share/download?token=$STOK&code=pass12&id=$SFID")
[ "$DLC" = "share-$U" ] && ok "分享下载内容正确" || bad "分享下载异常" "$DLC"
assert_code GET "/api/share/download?token=$STOK&code=pass12&id=$SFID" 403   # 超过限次

echo "== 分享: 文件夹 + 转存 (第二用户) =="
SFOLD=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/folder/create" -H 'Content-Type: application/json' -d '{"name":"ShareDir","parentId":0}' | jq "d['data']['id']")
echo "indir-$U" > "$TMP/indir.txt"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=$SFOLD" -F "f=@$TMP/indir.txt" >/dev/null
FSHARE=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/share/create" -H 'Content-Type: application/json' -d "{\"folderId\":$SFOLD}" | jq "d['data']['token']")
[ -n "$FSHARE" ] && [ "$FSHARE" != "None" ] && ok "创建文件夹分享 (无提取码)" || bad "文件夹分享失败"
BROWSE=$(curl -s "$BASE/api/share/browse?token=$FSHARE" | jq "d['data']['files'][0]['filename']")
[ "$BROWSE" = "indir.txt" ] && ok "文件夹分享可浏览内容" || bad "文件夹 browse 异常" "$BROWSE"
# 第二用户转存
U2="tr2_$(date +%s)_$RANDOM"
curl -s -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' -d "{\"username\":\"$U2\",\"password\":\"$PW\"}" >/dev/null
TOKEN2=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$U2\",\"password\":\"$PW\"}" | jq "d['data']['token']")
SAVE=$(curl -s -H "Authorization: Bearer $TOKEN2" -X POST "$BASE/api/share/save" -H 'Content-Type: application/json' -d "{\"token\":\"$FSHARE\",\"targetId\":0}")
SF=$(echo "$SAVE" | jq "d['data']['folders']")
[ "$SF" -ge 1 ] 2>/dev/null && ok "第二用户转存文件夹成功" || bad "转存失败" "$SAVE"
U2DIR=$(curl -s -H "Authorization: Bearer $TOKEN2" "$BASE/api/file/list?parentId=0" | jq "d['data']['folders'][0]['id']")
U2FILE=$(curl -s -H "Authorization: Bearer $TOKEN2" "$BASE/api/file/list?parentId=$U2DIR" | jq "d['data']['items'][0]['filename']")
[ "$U2FILE" = "indir.txt" ] && ok "转存后子文件内容到位" || bad "转存内容缺失" "$U2FILE"
# 我的分享 + 取消
MINE=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/share/mine" | jq "len(d['data']['shares'])")
[ "$MINE" -ge 2 ] 2>/dev/null && ok "我的分享列表返回 $MINE 条" || bad "mine 列表异常" "$MINE"
SID=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/share/mine" | python3 -c "import sys,json;d=json.load(sys.stdin)['data']['shares'];print(next((s['id'] for s in d if s['token']=='$FSHARE'),''))" 2>/dev/null)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/share/cancel" -H 'Content-Type: application/json' -d "{\"id\":$SID}" >/dev/null
assert_code GET "/api/share/info?token=$FSHARE" 404   # 取消后立即失效

echo "== 预览: 内联内容 / Range / 缩略图 =="
python3 - "$TMP/pic.png" <<'PY'
import zlib,struct,sys
w,h=80,60;raw=bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w): raw+=bytes([(x*255)//w,(y*255)//h,100])
def ck(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
open(sys.argv[1],"wb").write(b"\x89PNG\r\n\x1a\n"+ck(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+ck(b"IDAT",zlib.compress(bytes(raw),9))+ck(b"IEND",b""))
PY
printf 'preview-body-0123456789' > "$TMP/pv.txt"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=0" -F "f=@$TMP/pic.png" >/dev/null
curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/file/upload?parentId=0" -F "f=@$TMP/pv.txt"  >/dev/null
PICID=$(fid_of pic.png); PVTID=$(fid_of pv.txt)
# 全量 content
CT=$(curl -s -D- -o /dev/null -H "Authorization: Bearer $TOKEN" "$BASE/api/file/content?id=$PVTID&token=$TOKEN" | grep -i "^Content-Type:" | tr -d '\r')
echo "$CT" | grep -qi "text/plain" && ok "文本 content 返回 text/plain" || bad "content 类型错误" "$CT"
AR=$(curl -s -D- -o /dev/null -H "Authorization: Bearer $TOKEN" "$BASE/api/file/content?id=$PVTID&token=$TOKEN" | grep -i "Accept-Ranges" | tr -d '\r')
[ -n "$AR" ] && ok "content 声明 Accept-Ranges" || bad "缺少 Accept-Ranges"
# Range 分块
RH=$(curl -s -D- -o "$TMP/part" -H "Authorization: Bearer $TOKEN" -H "Range: bytes=0-4" "$BASE/api/file/content?id=$PVTID&token=$TOKEN")
echo "$RH" | grep -q "206" && ok "Range 请求返回 206" || bad "Range 未返回 206"
echo "$RH" | grep -qi "Content-Range: bytes 0-4/23" && ok "Content-Range 头正确" || bad "Content-Range 头异常" "$(echo "$RH"|grep -i content-range)"
[ "$(cat "$TMP/part")" = "previ" ] && ok "Range 分块内容正确" || bad "分块内容错误" "$(cat "$TMP/part")"
# 缩略图
TMAGIC=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/thumb?id=$PICID&token=$TOKEN" | xxd -p -l3 2>/dev/null)
[ "$TMAGIC" = "ffd8ff" ] && ok "图片缩略图返回 JPEG" || bad "缩略图非 JPEG" "$TMAGIC"
assert_code GET "/api/file/thumb?id=$PVTID&token=$TOKEN" 404   # 文本无缩略图
# 鉴权
assert_code GET "/api/file/content?id=$PVTID" 401             # 缺 token

echo "== 上传增强: 分片/断点续传 =="
head -c 260000 /dev/urandom > "$TMP/chunk.bin"
CHASH=$(sha256sum "$TMP/chunk.bin" | cut -d' ' -f1); CSIZE=$(wc -c < "$TMP/chunk.bin"); CKS=100000; CKT=3
INIT=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/upload/init" -H 'Content-Type: application/json' \
  -d "{\"filename\":\"chunk.bin\",\"hash\":\"$CHASH\",\"size\":$CSIZE,\"parentId\":0,\"chunkSize\":$CKS,\"totalChunks\":$CKT}")
UPID=$(echo "$INIT" | jq "d['data']['uploadId']")
[ -n "$UPID" ] && ok "upload/init 返回 uploadId" || bad "init 失败" "$INIT"
split -b $CKS -d -a 3 "$TMP/chunk.bin" "$TMP/ck_"
# 先只传分片 0
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/ck_000" "$BASE/api/upload/chunk?uploadId=$UPID&index=0" >/dev/null
# 断点续传: 再次 init 报告已传 [0]
RES=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/upload/init" -H 'Content-Type: application/json' \
  -d "{\"filename\":\"chunk.bin\",\"hash\":\"$CHASH\",\"size\":$CSIZE,\"parentId\":0,\"chunkSize\":$CKS,\"totalChunks\":$CKT}")
echo "$RES" | grep -q '"uploaded":\[0\]' && ok "断点续传上报已传分片" || bad "续传上报异常" "$RES"
# 传完余下分片
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/ck_001" "$BASE/api/upload/chunk?uploadId=$UPID&index=1" >/dev/null
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/ck_002" "$BASE/api/upload/chunk?uploadId=$UPID&index=2" >/dev/null
COMP=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/upload/complete" -H 'Content-Type: application/json' -d "{\"uploadId\":\"$UPID\"}")
echo "$COMP" | grep -q '"上传成功"' && ok "upload/complete 合并成功" || bad "合并失败" "$COMP"
# 下载校验 hash 一致
CKID=$(fid_of chunk.bin)
curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/file/download?id=$CKID&token=$TOKEN" -o "$TMP/ckdl.bin"
[ "$(sha256sum "$TMP/ckdl.bin" | cut -d' ' -f1)" = "$CHASH" ] && ok "分片合并后内容校验通过" || bad "合并内容不一致"
# 秒传: 相同 hash init -> instant
INS=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/upload/init" -H 'Content-Type: application/json' \
  -d "{\"filename\":\"chunk2.bin\",\"hash\":\"$CHASH\",\"size\":$CSIZE,\"parentId\":0,\"chunkSize\":$CKS,\"totalChunks\":$CKT}")
echo "$INS" | grep -q '"instant":true' && ok "分片初始化命中秒传" || bad "秒传未命中" "$INS"
# 非法 uploadId
assert_code POST "/api/upload/complete" 400 '{"uploadId":"bad id!!"}' "$TOKEN"

echo "== 上传增强: 文件夹递归建目录 (ensure) =="
EN1=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/folder/ensure" -H 'Content-Type: application/json' -d '{"parentId":0,"path":"proj/src/util"}')
L1=$(echo "$EN1" | jq "d['data']['id']")
EN2=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/folder/ensure" -H 'Content-Type: application/json' -d '{"parentId":0,"path":"proj/src/util"}')
L2=$(echo "$EN2" | jq "d['data']['id']")
[ -n "$L1" ] && [ "$L1" = "$L2" ] && ok "folder/ensure 幂等复用同一叶子" || bad "ensure 幂等失败" "$L1 vs $L2"

echo "== 上传增强: URL 离线下载 =="
OFF=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/offline/create" -H 'Content-Type: application/json' -d "{\"url\":\"$BASE/static/js/api.js\",\"parentId\":0}")
OID=$(echo "$OFF" | jq "d['data']['id']")
[ -n "$OID" ] && ok "offline/create 入队" || bad "离线创建失败" "$OFF"
OFST=""
for _ in $(seq 1 10); do
  sleep 1
  OFST=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/offline/list" | python3 -c "import sys,json;d=json.load(sys.stdin)['data']['tasks'];print(next((t['status'] for t in d if t['id']==$OID),''))" 2>/dev/null)
  [ "$OFST" = "1" ] || [ "$OFST" = "2" ] && break
done
[ "$OFST" = "1" ] && ok "离线下载完成 (status=1)" || bad "离线下载未完成" "status=$OFST"
[ -n "$(fid_of api.js)" ] && ok "离线文件已入库 (api.js)" || bad "离线文件未入库"
assert_code POST "/api/offline/create" 400 '{"url":"ftp://x/y"}' "$TOKEN"   # 非 http/https

echo "== 账户: 资料 / 改密 =="
assert_code POST "/api/user/profile" 200 '{"nickname":"测试昵称","email":"me@example.com"}' "$TOKEN"
assert_code POST "/api/user/profile" 400 '{"email":"invalid-email"}' "$TOKEN"
NICK=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/user/info" | jq "d['data']['nickname']")
[ "$NICK" = "测试昵称" ] && ok "资料回读昵称正确" || bad "昵称回读异常" "$NICK"
# 改密后用新密码登录, 再改回
assert_code POST "/api/user/password" 401 "{\"oldPassword\":\"wrongwrong\",\"newPassword\":\"newpw12345\"}" "$TOKEN"
assert_code POST "/api/user/password" 200 "{\"oldPassword\":\"$PW\",\"newPassword\":\"newpw12345\"}" "$TOKEN"
assert_code POST /api/auth/login 200 "{\"username\":\"$U\",\"password\":\"newpw12345\"}"
TOKEN=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$U\",\"password\":\"newpw12345\"}" | jq "d['data']['token']")

echo "== 账户: 头像 =="
printf '\x89PNG\r\n\x1a\n' > "$TMP/mini.png"; head -c 2000 /dev/urandom >> "$TMP/mini.png"
python3 - "$TMP/mini.png" << 'PY' 2>/dev/null || true
import sys
try:
    from PIL import Image
    Image.new('RGB',(200,150),(30,144,255)).save(sys.argv[1])
except Exception: pass
PY
AVR=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/user/avatar" -F "f=@$TMP/mini.png")
echo "$AVR" | grep -q '"头像已更新"' && ok "头像上传成功" || bad "头像上传失败" "$AVR"
MYUID=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/user/info" | jq "d['data']['uid']")
AVCT=$(curl -s -o /dev/null -w "%{content_type}" "$BASE/api/user/avatar?uid=$MYUID")
[ "$AVCT" = "image/jpeg" ] && ok "头像可读且为 JPEG" || bad "头像读取异常" "$AVCT"

echo "== 账户: 两步验证 (TOTP) =="
totp_code() { python3 - "$1" << 'PY'
import sys,base64,hmac,hashlib,struct,time
s=sys.argv[1]; k=base64.b32decode(s+'='*((8-len(s)%8)%8))
c=int(time.time()//30); h=hmac.new(k,struct.pack('>Q',c),hashlib.sha1).digest()
o=h[19]&0xf; print('%06d'%((struct.unpack('>I',h[o:o+4])[0]&0x7fffffff)%1000000))
PY
}
SECRET=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "$BASE/api/2fa/setup" | jq "d['data']['secret']")
[ ${#SECRET} -eq 32 ] && ok "2FA 密钥生成 (32 位 base32)" || bad "2FA 密钥异常" "$SECRET"
assert_code POST "/api/2fa/enable" 401 '{"code":"000000"}' "$TOKEN"          # 错误验证码
C=$(totp_code "$SECRET")
assert_code POST "/api/2fa/enable" 200 "{\"code\":\"$C\"}" "$TOKEN"
assert_code POST /api/auth/login 401 "{\"username\":\"$U\",\"password\":\"newpw12345\"}"   # 缺验证码 (code=4012, http=401)
NEED=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$U\",\"password\":\"newpw12345\"}" | jq "d['code']")
[ "$NEED" = "4012" ] && ok "开启 2FA 后无码登录返回 code=4012" || bad "2FA 登录信令异常" "$NEED"
C=$(totp_code "$SECRET")
assert_code POST /api/auth/login 200 "{\"username\":\"$U\",\"password\":\"newpw12345\",\"code\":\"$C\"}"
C=$(totp_code "$SECRET")
assert_code POST "/api/2fa/disable" 200 "{\"code\":\"$C\"}" "$TOKEN"

echo "== 管理后台 (需管理员) =="
ADU="adm_$(date +%s)_$RANDOM"
curl -s -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' -d "{\"username\":\"$ADU\",\"password\":\"$PW\"}" >/dev/null
mysql -uroot -p1234 test -e "UPDATE tbl_user SET role=1 WHERE username='$ADU';" 2>/dev/null
ADT=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$ADU\",\"password\":\"$PW\"}" | jq "d['data']['token']")
# 普通用户访问 admin -> 403
assert_code GET /api/admin/stats 403 "" "$TOKEN"
assert_code GET /api/admin/stats 200 "" "$ADT"
ST=$(curl -s -H "Authorization: Bearer $ADT" "$BASE/api/admin/stats" | jq "d['data']['users']")
[ -n "$ST" ] && [ "$ST" -gt 0 ] 2>/dev/null && ok "admin/stats 返回用户数=$ST" || bad "admin/stats 异常" "$ST"
assert_code GET /api/admin/users 200 "" "$ADT"
assert_code GET /api/admin/audit 200 "" "$ADT"
# 配额调整并回读
TUID=$(mysql -uroot -p1234 test -N -e "SELECT id FROM tbl_user WHERE username='$U';" 2>/dev/null)
assert_code POST "/api/admin/user/quota" 200 "{\"uid\":$TUID,\"quota\":2147483648}" "$ADT"
NQ=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/user/info" | jq "d['data']['quota']")
[ "$NQ" = "2147483648" ] && ok "管理员调整配额生效 (2GB)" || bad "配额未生效" "$NQ"
# 角色与禁用
assert_code POST "/api/admin/user/role" 200 "{\"uid\":$TUID,\"role\":1}" "$ADT"
assert_code POST "/api/admin/user/role" 200 "{\"uid\":$TUID,\"role\":0}" "$ADT"
assert_code POST "/api/admin/user/disable" 200 "{\"uid\":$TUID,\"disabled\":true}" "$ADT"
assert_code POST /api/auth/login 403 "{\"username\":\"$U\",\"password\":\"newpw12345\"}"   # 已禁用
assert_code POST "/api/admin/user/disable" 200 "{\"uid\":$TUID,\"disabled\":false}" "$ADT"
ADUID=$(mysql -uroot -p1234 test -N -e "SELECT id FROM tbl_user WHERE username='$ADU';" 2>/dev/null)
assert_code POST "/api/admin/user/disable" 400 "{\"uid\":$ADUID,\"disabled\":true}" "$ADT"   # 不能禁用自己

echo "== 登录限速 =="
RLU="rl_$(date +%s)_$RANDOM"
curl -s -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' -d "{\"username\":\"$RLU\",\"password\":\"$PW\"}" >/dev/null
RLCODE=""
for i in 1 2 3 4 5 6; do RLCODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$RLU\",\"password\":\"wrong$i\"}"); done
[ "$RLCODE" = "429" ] && ok "连续失败触发限速 (429)" || bad "限速未触发" "最后一次=$RLCODE"

echo "== 生态 · API 令牌 / WebDAV / 指标 =="
# Prometheus 指标端点
assert_code GET /metrics 200
curl -s "$BASE/metrics" | grep -q "cloudvault_uploads_total" && ok "/metrics 含 cloudvault_ 指标" || bad "/metrics 缺少指标"

# 独立用户, 避免与前序状态耦合
P6U="p6_$(date +%s)_$RANDOM"
curl -s -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' -d "{\"username\":\"$P6U\",\"password\":\"$PW\"}" >/dev/null
P6T=$(curl -s -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"$P6U\",\"password\":\"$PW\"}" | jq "d['data']['token']")

# 创建令牌
CT=$(curl -s -X POST "$BASE/api/tokens" -H "Authorization: Bearer $P6T" -H 'Content-Type: application/json' -d '{"name":"itest"}')
TOK=$(echo "$CT" | jq "d['data']['token']")
case "$TOK" in cvt_*) ok "创建令牌返回 cvt_ 明文" ;; *) bad "令牌格式异常" "$CT" ;; esac
# 用令牌鉴权访问
assert_code GET /api/user/info 200 "" "$TOK"
# 列出 / 吊销
TCNT=$(curl -s -H "Authorization: Bearer $P6T" "$BASE/api/tokens" | jq "len(d['data']['tokens'])")
[ "$TCNT" -ge 1 ] 2>/dev/null && ok "令牌列表返回 $TCNT 条" || bad "令牌列表异常" "$TCNT"
TID=$(curl -s -H "Authorization: Bearer $P6T" "$BASE/api/tokens" | jq "d['data']['tokens'][0]['id']")
assert_code POST /api/tokens/revoke 200 "{\"id\":$TID}" "$P6T"
assert_code GET /api/user/info 401 "" "$TOK"      # 吊销后失效

# WebDAV (Basic 认证, 密码用新令牌)
WT=$(curl -s -X POST "$BASE/api/tokens" -H "Authorization: Bearer $P6T" -H 'Content-Type: application/json' -d '{"name":"dav"}' | jq "d['data']['token']")
wcode() { curl -s -o /dev/null -w "%{http_code}" -u "$P6U:$WT" "$@"; }
[ "$(curl -s -o /dev/null -w "%{http_code}" -X OPTIONS -u "$P6U:$WT" "$BASE/webdav/")" = "200" ] && ok "WebDAV OPTIONS 200" || bad "WebDAV OPTIONS"
[ "$(wcode -X PROPFIND -H 'Depth: 1' "$BASE/webdav/")" = "207" ] && ok "WebDAV PROPFIND 根 207" || bad "WebDAV PROPFIND 根"
[ "$(curl -s -o /dev/null -w "%{http_code}" -X PROPFIND "$BASE/webdav/")" = "401" ] && ok "WebDAV 无认证 401" || bad "WebDAV 无认证应 401"
[ "$(wcode -X MKCOL "$BASE/webdav/it")" = "201" ] && ok "WebDAV MKCOL 201" || bad "WebDAV MKCOL"
echo "webdav-body-$P6U" > "$TMP/dav.txt"
[ "$(wcode -T "$TMP/dav.txt" "$BASE/webdav/it/a.txt")" = "201" ] && ok "WebDAV PUT 201" || bad "WebDAV PUT"
DGET=$(curl -s -u "$P6U:$WT" "$BASE/webdav/it/a.txt")
[ "$DGET" = "webdav-body-$P6U" ] && ok "WebDAV GET 内容一致" || bad "WebDAV GET 内容" "$DGET"
curl -s -u "$P6U:$WT" -X PROPFIND -H 'Depth: 1' "$BASE/webdav/it/" | grep -q "a.txt" && ok "WebDAV PROPFIND 列出文件" || bad "WebDAV PROPFIND 列表"
[ "$(wcode -X MOVE -H "Destination: $BASE/webdav/it/b.txt" "$BASE/webdav/it/a.txt")" = "201" ] && ok "WebDAV MOVE 201" || bad "WebDAV MOVE"
[ "$(curl -s -u "$P6U:$WT" "$BASE/webdav/it/b.txt")" = "webdav-body-$P6U" ] && ok "WebDAV MOVE 后可读" || bad "WebDAV MOVE 后读取"
[ "$(wcode -X DELETE "$BASE/webdav/it")" = "204" ] && ok "WebDAV DELETE 204" || bad "WebDAV DELETE"
[ "$(wcode -X PROPFIND -H 'Depth: 0' "$BASE/webdav/it/")" = "404" ] && ok "WebDAV 删除后 404" || bad "WebDAV 删除后应 404"

echo "== 全局搜索 =="
SF=$(curl -s -X POST "$BASE/api/folder/create" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"name":"搜索夹QZX","parentId":0}' | jq "d['data']['id']")
# 中文关键字 (URL 编码), 应命中该文件夹
SC=$(curl -s -G -H "Authorization: Bearer $TOKEN" "$BASE/api/search" --data-urlencode "q=搜索夹QZX" | jq "d['data']['count']")
[ "$SC" -ge 1 ] 2>/dev/null && ok "全局搜索中文命中 ($SC)" || bad "搜索未命中" "$SC"
# 部分 ASCII 关键字
SC2=$(curl -s -G -H "Authorization: Bearer $TOKEN" "$BASE/api/search" --data-urlencode "q=QZX" | jq "d['data']['count']")
[ "$SC2" -ge 1 ] 2>/dev/null && ok "部分关键字命中 ($SC2)" || bad "部分关键字未命中" "$SC2"
# 空查询 -> 0 项
SC3=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/search?q=" | jq "len(d['data']['items'])")
[ "$SC3" = "0" ] && ok "空查询返回 0 项" || bad "空查询异常" "$SC3"
# LIKE 元字符不被当通配 (搜 % 不应匹配全部)
SC4=$(curl -s -G -H "Authorization: Bearer $TOKEN" "$BASE/api/search" --data-urlencode "q=%" | jq "len(d['data']['items'])")
[ "$SC4" = "0" ] && ok "LIKE 元字符已转义 (% 不通配)" || bad "元字符未转义" "$SC4"
assert_code GET /api/search 401    # 需鉴权

echo "== 收藏夹 =="
FAVFID=$(curl -s -X POST "$BASE/api/folder/create" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"name":"收藏测试夹","parentId":0}' | jq "d['data']['id']")
FAV1=$(curl -s -X POST "$BASE/api/favorite/toggle" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "{\"itemType\":1,\"itemId\":$FAVFID}" | jq "d['data']['favorited']")
[ "$FAV1" = "True" ] && ok "首次 toggle 收藏 (favorited=true)" || bad "toggle 未收藏" "$FAV1"
FCNT=$(curl -s -H "Authorization: Bearer $TOKEN" "$BASE/api/favorites" | jq "len(d['data']['items'])")
[ "$FCNT" -ge 1 ] 2>/dev/null && ok "收藏列表返回 $FCNT 项" || bad "收藏列表异常" "$FCNT"
assert_code POST /api/favorite/batch 200 "{\"fileIds\":[],\"folderIds\":[$FAVFID]}" "$TOKEN"   # 幂等
FAV2=$(curl -s -X POST "$BASE/api/favorite/toggle" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "{\"itemType\":1,\"itemId\":$FAVFID}" | jq "d['data']['favorited']")
[ "$FAV2" = "False" ] && ok "再次 toggle 取消收藏 (favorited=false)" || bad "toggle 未取消" "$FAV2"
assert_code POST /api/favorite/toggle 404 '{"itemType":0,"itemId":999999999}' "$TOKEN"    # 不存在
assert_code POST /api/favorite/toggle 400 '{"itemType":5,"itemId":1}' "$TOKEN"             # 非法类型
assert_code GET /api/favorites 401                                                          # 需鉴权

echo
echo "============================================"
printf "结果: \033[32m%d 通过\033[0m, " "$PASS"
if [ "$FAIL" -eq 0 ]; then printf "\033[32m0 失败\033[0m  ✅ 全部通过\n"; else printf "\033[31m%d 失败\033[0m ❌\n" "$FAIL"; fi
echo "============================================"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
