#!/usr/bin/env bash
# qboot 脚本公共函数库。
#
# 用法：各脚本开头
#     source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
#
# 约定：
#   - 所有脚本都能在仓库任意目录下被调用（自己推断 QBOOT_ROOT）
#   - 出错立即退出（set -euo pipefail），并在退出前打印可复制的修复命令
#   - 输出带颜色，但 TTY 不是终端时自动关掉，方便重定向到日志文件

# 不要在这里 set -euo pipefail：本文件是被 source 的，
# 由调用方自己设置，否则这里的 set 会污染调用方的错误处理。

QBOOT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export QBOOT_ROOT

# ---------------- 输出 ----------------

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
    C_BLU=$'\033[34m'; C_DIM=$'\033[2m';  C_OFF=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YEL=""; C_BLU=""; C_DIM=""; C_OFF=""
fi

log()  { printf '%s[ qboot ]%s %s\n' "$C_BLU" "$C_OFF" "$*"; }
ok()   { printf '%s[   OK  ]%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '%s[  WARN ]%s %s\n' "$C_YEL" "$C_OFF" "$*"; }
step() { printf '%s\n--- %s%s%s\n' "$C_DIM" "$C_OFF" "$*" "$C_DIM"; }

# die <提示> <修复命令...>
# 打印错误 + 可直接复制的修复命令，然后以 1 退出。
die() {
    local msg="$1"; shift
    printf '%s[ FAIL  ]%s %s\n' "$C_RED" "$C_OFF" "$msg" >&2
    if [ "$#" -gt 0 ]; then
        printf '%s  修  复:%s\n' "$C_YEL" "$C_OFF" >&2
        local c
        for c in "$@"; do printf '            %s\n' "$c" >&2; done
    fi
    exit 1
}

# have <命令> —— 命令存在返回 0
have() { command -v "$1" >/dev/null 2>&1; }

# need <命令> <安装提示> —— 不存在就 die
need() {
    have "$1" && return 0
    die "缺少命令: $1" "${2:-sudo apt install -y $1}"
}

# ---------------- 平台 ----------------

OS="$(uname -s)"
NPROC=1
case "$OS" in
    Linux)  NPROC="$(nproc 2>/dev/null || echo 4)" ;;
    Darwin) NPROC="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" ;;
esac
export OS NPROC

# ---------------- 命令行参数解析 ----------------
#
# 用法：
#   arg_init "说明"
#   arg_str  "prefix"   "$DEFAULT_PREFIX"  "安装前缀"
#   arg_int  "jobs"     "$NPROC"           "并行编译数"
#   arg_bool "clean"    0                  "是否先清空 build"
#   arg_help "$@"          # 传 "$@"
#   arg_get  prefix        # 取值
#
# 支持 --key=value 与 --key value 两种写法，bool 支持 --flag 裸写。
#
# 这里刻意不用关联数组（declare -A）：macOS 自带的是 bash 3.2，没有关联数组，
# 用两个平行数组就能在 bash 3.2 / 4 / 5 上都跑得通，方便本机验证。

Q_ARG_K=()      # 选项名
Q_ARG_V=()      # 选项值（字符串形式）
Q_ARG_BOOLK=()  # 哪些选项是 bool

arg_init() { Q_ARGS_DESC="$*"; }

_arg_index() {  # 返回下标，找不到返回 -1
    local i
    for i in "${!Q_ARG_K[@]}"; do
        [ "${Q_ARG_K[$i]}" = "$1" ] && { printf '%s' "$i"; return 0; }
    done
    printf -- '-1'
}

arg_str()  { Q_ARG_K+=("$1"); Q_ARG_V+=("$2"); }
arg_int()  { Q_ARG_K+=("$1"); Q_ARG_V+=("$2"); }
arg_bool() { Q_ARG_K+=("$1"); Q_ARG_V+=("$2"); Q_ARG_BOOLK+=("$1"); }

_arg_set() {
    local i
    i="$(_arg_index "$1")"
    if [ "$i" = "-1" ]; then Q_ARG_K+=("$1"); Q_ARG_V+=("$2")
    else Q_ARG_V[$i]="$2"; fi
}

_arg_isbool() {
    local b
    [ "${#Q_ARG_BOOLK[@]}" -eq 0 ] && return 1
    for b in "${Q_ARG_BOOLK[@]}"; do [ "$b" = "$1" ] && return 0; done
    return 1
}

arg_help() {
    local i
    for i in "$@"; do
        case "$i" in
            -h|--help)
                printf '用法: %s [选项]\n\n%s\n\n选项:\n' "$0" "$Q_ARGS_DESC"
                local k
                for k in "${!Q_ARG_K[@]}"; do
                    printf '  --%-14s 默认: %s\n' "${Q_ARG_K[$k]}" "${Q_ARG_V[$k]}"
                done
                printf '  --help          显示本帮助\n'
                exit 0
                ;;
        esac
    done
}

arg_parse() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --*=*)
                local k="${1%%=*}"; k="${k#--}"; local v="${1#*=}"
                _arg_set "$k" "$v"
                shift
                ;;
            --*)
                local k="${1#--}"
                if _arg_isbool "$k"; then
                    _arg_set "$k" 1
                else
                    [ "$#" -ge 2 ] || die "选项 --$k 需要值（用 --help 看用法）"
                    _arg_set "$k" "$2"
                    shift
                fi
                shift
                ;;
            *) die "无法识别的参数: $1（用 --help 看用法）" ;;
        esac
    done
}

arg_get() {
    local i
    i="$(_arg_index "$1")"
    [ "$i" = "-1" ] && { printf ''; return 0; }
    printf '%s' "${Q_ARG_V[$i]}"
}

# ---------------- 小工具 ----------------

# wait_port <主机> <端口> <超时秒> —— 等服务端口起来
wait_port() {
    local host="$1" port="$2" timeout="${3:-10}" i=0
    while [ "$i" -lt "$((timeout * 10))" ]; do
        # fd 3 在子 shell 里开，子 shell 退出时自动关掉。
        # 千万别在主 shell 里写 "exec 3<&- 2>/dev/null"：
        # exec 的重定向是永久生效的，那会把整个脚本的 stderr 丢进 /dev/null，
        # 后面的报错就全看不见了。
        if (exec 3<>/dev/tcp/"$host"/"$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# http_get <url> —— 发 GET，避免 curl 走代理（环境里常有 http_proxy）
http_get() { curl -s --noproxy '*' "$@"; }

# http_post <url> <body>
http_post() { curl -s --noproxy '*' -XPOST -d "$2" "$1"; }

# assert_json <说明> <实际> <期望子串>
assert_json() {
    local desc="$1" got="$2" want="$3"
    if printf '%s' "$got" | grep -qF "$want"; then
        ok "$desc"
        return 0
    fi
    die "$desc" \
        "期望包含: $want" \
        "实际收到: $got"
}

# 启动后台进程并记录 pid，脚本退出时自动收掉。
# 注意：不要在子 shell 里调 bg_start（写成 ( cd x && bg_start ... )），
# 子 shell 里的 Q_PIDS 是副本，父脚本收不到这些 pid，进程会泄漏。
Q_PIDS=()
bg_start() {
    # 默认丢弃后台进程输出；调试时设 QBOOT_BG_LOG_DIR=/tmp/qblog
    # 就能把每个进程的 stdout/stderr 存成 /tmp/qblog/<pid>.log
    if [ -n "${QBOOT_BG_LOG_DIR:-}" ]; then
        mkdir -p "$QBOOT_BG_LOG_DIR"
        "$@" >"$QBOOT_BG_LOG_DIR/$$.log" 2>&1 &
    else
        "$@" >/dev/null 2>&1 &
    fi
    Q_PIDS+=("$!")
}
cleanup_pids() {
    local p
    [ "${#Q_PIDS[@]}" -eq 0 ] && return 0
    for p in "${Q_PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    Q_PIDS=()
}
