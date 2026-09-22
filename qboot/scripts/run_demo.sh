#!/usr/bin/env bash
#
# 一键跑 demo：起三个服务，打一遍接口，把结果打出来，然后收掉。
#
#   ./scripts/run_demo.sh                 # 跑完就退出（进程自动清理）
#   ./scripts/run_demo.sh --keep          # 跑完不退出，Ctrl-C 结束（方便自己 curl）
#   ./scripts/run_demo.sh --build         # 跑之前先编译
#   ./scripts/run_demo.sh --mode mysql    # 连真库跑（等价于 smoke_mysql.sh）
#
# 起来的三个服务：
#   minimal_svc :9090   最小骨架模板（templates/minimal_svc）
#   user-svc    :8081   HTTP + XML mapper + DB + Consul 注册
#   order-svc   :8082   服务间调用（order-svc -> user-svc）
#
# 默认用 mock:// 驱动，不需要任何数据库。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "一键跑 qboot demo"
arg_str  "mode"      "mock"   "mock=内存假驱动 / mysql=真库（需先建表）"
arg_str  "build-dir" "build"  "构建目录"
arg_str  "user"      "root"   "mysql 模式下的用户"
arg_str  "pass"      ""       "mysql 模式下的密码"
arg_str  "dbhost"    "127.0.0.1" "mysql 模式下的主机"
arg_bool "build"     0        "跑之前先编译"
arg_bool "keep"      0        "跑完保持运行，Ctrl-C 退出"
arg_help "$@"
arg_parse "$@"

MODE="$(arg_get mode)"
BDIR="$QBOOT_ROOT/$(arg_get build-dir)"

if [ "$MODE" = "mysql" ]; then
    exec "$QBOOT_ROOT/scripts/smoke_mysql.sh" \
        --user "$(arg_get user)" --pass "$(arg_get pass)" --host "$(arg_get dbhost)"
fi

step "编译"
[ -x "$BDIR/bin/user_svc" ] || [ "$(arg_get build)" = "1" ] || die \
    "找不到 $BDIR/bin/user_svc" "./scripts/build.sh"
if [ "$(arg_get build)" = "1" ] || [ ! -x "$BDIR/bin/user_svc" ]; then
    cmake -S "$QBOOT_ROOT" -B "$BDIR" >/dev/null
    cmake --build "$BDIR" -j"$NPROC" 2>&1 | tail -5
fi
[ -x "$BDIR/bin/order_svc" ] || warn "没有 order_svc（samples 没编？）"

trap cleanup_pids EXIT

step "启动服务"
cd "$QBOOT_ROOT"
bg_start "$BDIR/bin/user_svc"  samples/conf/user_svc.ini
bg_start "$BDIR/bin/order_svc" samples/conf/order_svc.ini

wait_port 127.0.0.1 8081 15 || die "user-svc 没起来" "./scripts/build.sh --samples 1"
wait_port 127.0.0.1 8082 15 || die "order-svc 没起来" "./scripts/build.sh --samples 1"
ok "user-svc :8081   order-svc :8082"

step "接口演示"
show() {  # show <说明> <命令输出>
    printf '  %s%-28s%s %s\n' "$C_DIM" "$1" "$C_OFF" "$2"
}

show "user-svc  查单个"   "$(http_get http://127.0.0.1:8081/user/1)"
show "user-svc  条件查询" "$(http_get 'http://127.0.0.1:8081/user/search?minAge=18')"
show "user-svc  新增"     "$(http_post http://127.0.0.1:8081/user '{"name":"qiaoshui","age":18,"balance":9.9,"status":1}')"
show "order-svc 列表"     "$(http_get 'http://127.0.0.1:8082/order/list?userId=1')"
show "order-svc 跨服务"   "$(http_get http://127.0.0.1:8082/demo/call-user/1)"
show "order-svc 建单"     "$(http_post http://127.0.0.1:8082/order '{"userId":1,"item":"鼠标","amount":99.9}')"
show "健康检查"           "$(http_get http://127.0.0.1:8082/health)"

step "断言"
assert_json "user-svc /user/1"        "$(http_get http://127.0.0.1:8081/user/1)" '"code":0'
assert_json "order-svc 跨服务调用"    "$(http_get http://127.0.0.1:8082/demo/call-user/1)" 'zhangsan'
assert_json "order-svc /order/list"   "$(http_get 'http://127.0.0.1:8082/order/list?userId=1')" '"code":0'

if [ "$(arg_get keep)" = "1" ]; then
    step "保持运行（Ctrl-C 退出）"
    cat <<EOF
  user-svc   : http://127.0.0.1:8081
  order-svc  : http://127.0.0.1:8082

  自己试：
    curl http://127.0.0.1:8081/user/1
    curl 'http://127.0.0.1:8081/user/search?name=lisi'
    curl -XPOST http://127.0.0.1:8081/user -d '{"name":"abc","age":1,"balance":0,"status":1}'
    curl http://127.0.0.1:8082/demo/call-user/1
    curl http://127.0.0.1:8082/order/list?userId=1
    curl -XPOST http://127.0.0.1:8082/order -d '{"userId":1,"item":"鼠标","amount":99.9}'
EOF
    wait
else
    ok "demo 跑完，进程已清理"
    cat <<EOF

想连真库再跑一遍：
    sudo mysql < sql/qboot_demo.sql
    ./scripts/smoke_mysql.sh --user root --pass ''
想验证 Consul：
    consul agent -dev -client=0.0.0.0 &
    ./scripts/smoke_consul.sh
EOF
fi
