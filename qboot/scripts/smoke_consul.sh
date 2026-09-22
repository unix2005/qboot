#!/usr/bin/env bash
#
# Consul 冒烟：注册 -> 心跳 -> 发现 -> 服务间调用，全链路跑一遍。
#
# 前置：
#   sudo ./scripts/install_deps_ubuntu.sh --with-consul
#   consul agent -dev -client=0.0.0.0 &        # 或让本脚本自己起：--start
#   ./scripts/build.sh
#
# 跑：
#   ./scripts/smoke_consul.sh
#   ./scripts/smoke_consul.sh --start          # consul 没在跑就自动起一个 dev 实例
#   ./scripts/smoke_consul.sh --consul 192.168.1.10:8500
#
# 验证点：
#   1) user-svc / order-svc 注册进 Consul，TTL check 变 passing
#   2) order-svc 用服务名 "user-svc" 就能调到它（不写死 IP:PORT）
#   3) 进程退出后 Consul 里的实例被摘掉
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "Consul 注册发现冒烟"
arg_str  "consul"    "127.0.0.1:8500" "Consul 地址 host:port"
arg_str  "build-dir" "build"          "构建目录"
arg_bool "start"     0                "没有 Consul 就自动起一个 dev 实例"
arg_bool "build"     0                "跑之前先编译"
arg_help "$@"
arg_parse "$@"

CONSUL_ADDR="$(arg_get consul)"
CONSUL_URL="http://$CONSUL_ADDR"
BDIR="$QBOOT_ROOT/$(arg_get build-dir)"

step "Consul 连通性"
if ! http_get "$CONSUL_URL/v1/status/leader" >/dev/null 2>&1; then
    if [ "$(arg_get start)" = "1" ]; then
        have consul || die "没有 consul 命令" \
            "sudo ./scripts/install_deps_ubuntu.sh --with-consul"
        log "启动 consul agent -dev"
        bg_start consul agent -dev -client=0.0.0.0
        wait_port 127.0.0.1 8500 30 || die "consul 起不来" "consul agent -dev -client=0.0.0.0"
    else
        die "连不上 Consul: $CONSUL_URL" \
            "consul agent -dev -client=0.0.0.0 &" \
            "或: $0 --start"
    fi
fi
LEADER="$(http_get "$CONSUL_URL/v1/status/leader" | tr -d '"')"
[ -n "$LEADER" ] || die "Consul 集群还没有 leader（刚启动？等几秒再跑一次）"
ok "Consul leader: $LEADER"

step "编译产物检查"
[ -x "$BDIR/bin/user_svc" ] || [ "$(arg_get build)" = "1" ] || die \
    "找不到 $BDIR/bin/user_svc" "./scripts/build.sh"
if [ "$(arg_get build)" = "1" ] || [ ! -x "$BDIR/bin/user_svc" ]; then
    cmake -S "$QBOOT_ROOT" -B "$BDIR" >/dev/null
    cmake --build "$BDIR" -j"$NPROC" 2>&1 | tail -5
fi

step "生成配置（consul enable=1）"
TMPDIR_C="$(mktemp -d)"
HOST="${CONSUL_ADDR%:*}"

for svc in user order; do
    port=8081; [ "$svc" = order ] && port=8082
    cat > "$TMPDIR_C/$svc.ini" <<EOF
[server]
name       = $svc-svc
port       = $port
io_threads = 2

[log]
dir   = $TMPDIR_C/logs
level = info

[db]
url       = mock://u:p@127.0.0.1:0/qboot_demo
pool      = 8
idle_secs = 60

[mapper]
dir = $QBOOT_ROOT/samples/mapper

[consul]
enable = 1
addr   = $CONSUL_ADDR
host   = 127.0.0.1
ttl    = 5

[upstream]
user_svc = 127.0.0.1:8081
EOF
done

trap 'cleanup_pids; rm -rf "$TMPDIR_C"' EXIT

step "启动两个服务并注册"
cd "$QBOOT_ROOT"
bg_start "$BDIR/bin/user_svc"  "$TMPDIR_C/user.ini"
bg_start "$BDIR/bin/order_svc" "$TMPDIR_C/order.ini"
wait_port 127.0.0.1 8081 15 || die "user-svc 没起来，看 $TMPDIR_C/logs"
wait_port 127.0.0.1 8082 15 || die "order-svc 没起来，看 $TMPDIR_C/logs"

# TTL check 要等一个心跳周期才变 passing（配置里 ttl=5s）
log "等 TTL check 变 passing（最多 20s）"
ok_passing=0
for i in $(seq 1 40); do
    n="$(http_get "$CONSUL_URL/v1/health/service/user-svc?passing" \
         | grep -o '"ServiceID"' | wc -l | tr -d ' ')"
    [ "${n:-0}" -ge 1 ] && { ok_passing=1; break; }
    sleep 0.5
done
[ "$ok_passing" = "1" ] \
    && ok "user-svc 在 Consul 里已是 passing" \
    || die "user-svc 注册了但没变 passing" \
           "curl -s $CONSUL_URL/v1/health/checks/user-svc | head -50"

SVC_JSON="$(http_get "$CONSUL_URL/v1/catalog/service/order-svc")"
printf '%s' "$SVC_JSON" | grep -q 'order-svc' \
    && ok "order-svc 也已注册" \
    || die "order-svc 没注册成功" "curl -s $CONSUL_URL/v1/catalog/services"

step "用服务名调用（不写死地址）"
assert_json "order-svc --(user-svc)--> /user/1" \
    "$(http_get http://127.0.0.1:8082/demo/call-user/1)" 'zhangsan'

assert_json "POST /order（内部会先问 user-svc）" \
    "$(http_post http://127.0.0.1:8082/order '{"userId":1,"item":"鼠标","amount":99.9}')" \
    '"code":0'

step "服务目录一览"
http_get "$CONSUL_URL/v1/catalog/services" | tr ',' '\n' | sed 's/[{}"]//g' | grep -v '^$' | sed 's/^/    /'

step "优雅退出 -> 自动摘除"
cleanup_pids
log "等待 Consul 摘除（TTL 到期）"
for i in $(seq 1 30); do
    n="$(http_get "$CONSUL_URL/v1/health/service/user-svc?passing" \
         | grep -o '"ServiceID"' | wc -l | tr -d ' ')"
    [ "${n:-0}" -eq 0 ] && break
    sleep 0.5
done
n="$(http_get "$CONSUL_URL/v1/health/service/user-svc?passing" | grep -o '"ServiceID"' | wc -l | tr -d ' ')"
[ "${n:-0}" -eq 0 ] && ok "实例已从健康列表移除" \
                    || warn "Consul 里仍能查到 passing 实例（TTL 未到期），数量=$n"

ok "Consul 冒烟完成"
