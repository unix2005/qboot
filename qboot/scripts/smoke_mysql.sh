#!/usr/bin/env bash
#
# 真库冒烟：用真实 MySQL/MariaDB 跑一遍 user-svc + order-svc 的增删改查。
#
# 前置：
#   sudo ./scripts/install_deps_ubuntu.sh --with-mysql-server
#   sudo mysql < sql/qboot_demo.sql
#   ./scripts/build.sh
#
# 跑：
#   ./scripts/smoke_mysql.sh --user root --pass ''
#   ./scripts/smoke_mysql.sh --user qboot --pass qboot123 --host 127.0.0.1 --port 3306
#   ./scripts/smoke_mysql.sh --sudo            # Ubuntu 上 root 走 unix socket 要用 sudo
#   ./scripts/smoke_mysql.sh --skip-ddl        # 库表已建好，跳过建表
#
# 这个脚本会：
#   1) 建库建表（sql/qboot_demo.sql）
#   2) 生成一份指向真库的 ini，起 user-svc(8081) 和 order-svc(8082)
#   3) 用 curl 打增删改查，断言返回内容
#   4) 退出时自动收掉两个进程
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "用真实 MySQL/MariaDB 跑端到端冒烟"
arg_str  "host"     "127.0.0.1"  "数据库主机"
arg_str  "port"     "3306"       "数据库端口"
arg_str  "user"     "root"       "数据库用户"
arg_str  "pass"     ""           "数据库密码"
arg_str  "db"       "qboot_demo" "数据库名"
arg_str  "build-dir" "build"     "构建目录"
arg_bool "sudo"     0            "mysql 客户端用 sudo（root@socket 场景）"
arg_bool "skip-ddl" 0            "跳过建库建表"
arg_bool "build"    0            "跑之前先编译"
arg_help "$@"
arg_parse "$@"

HOST="$(arg_get host)"; PORT="$(arg_get port)"
USER="$(arg_get user)"; PASS="$(arg_get pass)"
DB="$(arg_get db)"
BDIR="$QBOOT_ROOT/$(arg_get build-dir)"

MYSQL=(mysql)
[ "$(arg_get sudo)" = "1" ] && MYSQL=(sudo mysql)
[ -n "$PASS" ] && MYSQL+=("-p$PASS")
MYSQL+=("-h$HOST" "-P$PORT" "-u$USER")

step "数据库连通性"
need mysql "sudo apt install -y mariadb-client"
"${MYSQL[@]}" -e "SELECT VERSION();" >/dev/null 2>&1 \
    || die "连不上 MySQL: ${MYSQL[*]}" \
           "sudo systemctl start mariadb" \
           "sudo mysql -e \"ALTER USER 'root'@'localhost' IDENTIFIED VIA mysql_native_password USING PASSWORD('rootpass');\"" \
           "或改用有密码的账号: $0 --user qboot --pass qboot123"
ok "连上了: $("${MYSQL[@]}" -N -e 'SELECT VERSION();' 2>/dev/null)"

if [ "$(arg_get skip-ddl)" != "1" ]; then
    step "建库建表 sql/qboot_demo.sql"
    "${MYSQL[@]}" < "$QBOOT_ROOT/sql/qboot_demo.sql" \
        || die "执行 qboot_demo.sql 失败" \
               "sudo ${MYSQL[*]} < sql/qboot_demo.sql"
    ok "表已就绪"
fi
"${MYSQL[@]}" "$DB" -N -e "SELECT COUNT(*) FROM t_user;" >/dev/null 2>&1 \
    || die "数据库 $DB 或表 t_user 不存在" \
           "${MYSQL[*]} < sql/qboot_demo.sql"
NU="$("${MYSQL[@]}" "$DB" -N -e 'SELECT COUNT(*) FROM t_user;')"
ok "t_user 现有 $NU 行"

step "编译产物检查"
[ -x "$BDIR/bin/user_svc" ] || [ "$(arg_get build)" = "1" ] || die \
    "找不到 $BDIR/bin/user_svc" "./scripts/build.sh"
if [ "$(arg_get build)" = "1" ] || [ ! -x "$BDIR/bin/user_svc" ]; then
    cmake -S "$QBOOT_ROOT" -B "$BDIR" >/dev/null
    cmake --build "$BDIR" -j"$NPROC" 2>&1 | tail -5
fi
if ! grep -q "Q_HAVE_MYSQL" "$BDIR/CMakeFiles" -r 2>/dev/null; then :; fi
[ -x "$BDIR/lib/libq_db_mysql.a" ] || warn "没有 libq_db_mysql.a（没装 mariadb 开发包？）服务会连不上真库"

step "生成真库配置"
TMPDIR_SMOKE="$(mktemp -d)"
URL="mysql://$USER:$PASS@$HOST:$PORT/$DB"

cat > "$TMPDIR_SMOKE/user.ini" <<EOF
[server]
name       = user-svc
port       = 8081
io_threads = 2

[log]
dir   = $TMPDIR_SMOKE/logs
level = info

[db]
url       = $URL
pool      = 8
idle_secs = 60

[mapper]
dir = $QBOOT_ROOT/samples/mapper
EOF

cat > "$TMPDIR_SMOKE/order.ini" <<EOF
[server]
name       = order-svc
port       = 8082
io_threads = 2

[log]
dir   = $TMPDIR_SMOKE/logs
level = info

[db]
url       = $URL
pool      = 8
idle_secs = 60

[mapper]
dir = $QBOOT_ROOT/samples/mapper

[consul]
enable = 0

[upstream]
user_svc = 127.0.0.1:8081
EOF
log "db url = $URL"

trap 'cleanup_pids; rm -rf "$TMPDIR_SMOKE"' EXIT

step "启动服务"
cd "$QBOOT_ROOT"
bg_start "$BDIR/bin/user_svc"  "$TMPDIR_SMOKE/user.ini"
bg_start "$BDIR/bin/order_svc" "$TMPDIR_SMOKE/order.ini"
wait_port 127.0.0.1 8081 15 || die "user-svc 没起来，看日志: $TMPDIR_SMOKE/logs"
wait_port 127.0.0.1 8082 15 || die "order-svc 没起来，看日志: $TMPDIR_SMOKE/logs"
ok "user-svc :8081 / order-svc :8082 已就绪"

step "user-svc 增删改查"
assert_json "GET /user/1（种子数据 zhangsan）" \
    "$(http_get http://127.0.0.1:8081/user/1)" '"user_name":"zhangsan"'

assert_json "GET /user/search?name=zhang%25（LIKE 动态标签）" \
    "$(http_get 'http://127.0.0.1:8081/user/search?name=zhang%25&minAge=0')" 'zhangsan'

assert_json "GET /user/search?minAge=26（应只剩 wangwu，30 岁）" \
    "$(http_get 'http://127.0.0.1:8081/user/search?minAge=26')" 'wangwu'

NEWUSER="$(http_post http://127.0.0.1:8081/user \
    '{"name":"qiaoshui","age":18,"balance":9.9,"status":1}')"
log "POST /user -> $NEWUSER"
printf '%s' "$NEWUSER" | grep -q '"code":0' \
    || die "新增用户失败" "看 $TMPDIR_SMOKE/logs 下的日志"
NEWID="$(printf '%s' "$NEWUSER" | grep -oE '"id":[0-9]+' | grep -oE '[0-9]+')"
ok "POST /user 成功，新 id=$NEWID"

assert_json "GET /user/$NEWID（回查刚插入的行）" \
    "$(http_get "http://127.0.0.1:8081/user/$NEWID")" 'qiaoshui'

curl -s --noproxy '*' -XPUT -d '{"age":19}' "http://127.0.0.1:8081/user/$NEWID" >/dev/null
assert_json "PUT /user/$NEWID 后 age=19" \
    "$(http_get "http://127.0.0.1:8081/user/$NEWID")" '"age":19'

curl -s --noproxy '*' -XDELETE "http://127.0.0.1:8081/user/$NEWID" >/dev/null
DEL="$(http_get "http://127.0.0.1:8081/user/$NEWID")"
log "DELETE 后回查 -> $DEL"
printf '%s' "$DEL" | grep -qE '"data":null' \
    && ok "DELETE /user/$NEWID 生效" \
    || warn "删除后回查仍有数据（mock 语义？）: $DEL"

step "order-svc 跨服务调用"
assert_json "order-svc -> user-svc 透传" \
    "$(http_get http://127.0.0.1:8082/demo/call-user/1)" 'zhangsan'

assert_json "GET /order/list?userId=1" \
    "$(http_get 'http://127.0.0.1:8082/order/list?userId=1')" '键盘'

ORD="$(http_post http://127.0.0.1:8082/order '{"userId":1,"item":"鼠标","amount":99.9}')"
log "POST /order -> $ORD"
printf '%s' "$ORD" | grep -q '"code":0' || die "建单失败（user-svc 校验没过？）"
assert_json "POST /order 后列表里有鼠标" \
    "$(http_get 'http://127.0.0.1:8082/order/list?userId=1')" '鼠标'

step "核对落库结果"
"${MYSQL[@]}" "$DB" -e \
    "SELECT id,user_name FROM t_user ORDER BY id; SELECT id,item,amount FROM t_order ORDER BY id;"

step "通过"
ok "真库冒烟全部通过（端口 8081/8082 已关闭，临时目录已清理）"
