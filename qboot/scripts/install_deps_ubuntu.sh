#!/usr/bin/env bash
#
# 在 Ubuntu / Debian（含麒麟、统信等 Debian 系）上装 qboot 的编译与运行依赖。
#
#   sudo ./scripts/install_deps_ubuntu.sh                # 只装必选
#   sudo ./scripts/install_deps_ubuntu.sh --with-consul  # 顺便装 Consul
#   sudo ./scripts/install_deps_ubuntu.sh --with-mysql-server --with-nginx
#
# 必选：编译 HTTP / DB / XML mapper 必需的库和头文件
# 可选：Consul（服务注册发现）、MariaDB Server（真库冒烟）、nginx（接入层）
#
# 只装包，不碰仓库，不编译。装完跑 ./scripts/build.sh。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "在 Ubuntu/Debian 上安装 qboot 依赖"
arg_bool "with-consul"        0 "安装 Consul（服务注册发现）"
arg_bool "with-mysql-server"  0 "安装 MariaDB Server（真库冒烟用）"
arg_bool "with-nginx"         0 "安装 nginx（接入层示例）"
arg_bool "dry-run"            0 "只打印将要执行的命令，不真的执行"
arg_help "$@"
arg_parse "$@"

WITH_CONSUL="$(arg_get with-consul)"
WITH_MYSQL="$(arg_get with-mysql-server)"
WITH_NGINX="$(arg_get with-nginx)"
DRY_RUN="$(arg_get dry-run)"

APT=(apt-get)
if [ "$(id -u)" -ne 0 ]; then
    if have sudo; then APT=(sudo apt-get); else
        die "需要 root 权限（或用 sudo 运行本脚本）"
    fi
fi
if [ "$DRY_RUN" = "1" ]; then APT=(echo "[dry-run]" "${APT[@]}"); fi

step "检查系统"
[ -f /etc/os-release ] || die "只在 Debian/Ubuntu 系验证过，/etc/os-release 不存在"
# shellcheck disable=SC1091
. /etc/os-release
log "系统: ${PRETTY_NAME:-$ID}"
case "$ID" in
    ubuntu|debian|kylin|uos|deepin|linuxmint) ;;
    *) warn "未在 '$ID' 上验证过，apt 包名可能不同，继续尝试" ;;
esac

step "更新 apt 索引"
"${APT[@]}" update -qq || warn "apt update 失败（离线环境？），继续用已有索引"

step "安装编译工具链与 pkg-config"
"${APT[@]}" install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates curl

step "安装必选依赖库"
# libmariadb-dev 在老版本 Ubuntu 上叫 libmariadbclient-dev，都不在就退化到
# default-libmysqlclient-dev（MySQL 头文件兼容）
MARIA_PKG=libmariadb-dev
if ! apt-cache show "$MARIA_PKG" >/dev/null 2>&1; then
    MARIA_PKG=libmariadbclient-dev
fi
if ! apt-cache show "$MARIA_PKG" >/dev/null 2>&1; then
    warn "找不到 libmariadb-dev，退化为 default-libmysqlclient-dev（MySQL 协议兼容）"
    MARIA_PKG=default-libmysqlclient-dev
fi

"${APT[@]}" install -y --no-install-recommends \
    libevent-dev \
    libcurl4-openssl-dev \
    libxml2-dev \
    libjansson-dev \
    "$MARIA_PKG"

step "校验头文件与库是否到位"
check_pkg() { dpkg -s "$1" >/dev/null 2>&1 && ok "$1" || die "包未安装成功: $1" \
    "sudo apt install -y $1"; }
for p in build-essential cmake pkg-config libevent-dev libcurl4-openssl-dev \
         libxml2-dev libjansson-dev "$MARIA_PKG"; do
    check_pkg "$p"
done

# ---------------- 可选：Consul ----------------
if [ "$WITH_CONSUL" = "1" ]; then
    step "安装 Consul"
    if have consul; then
        ok "consul 已安装: $(consul version | head -1)"
    else
        CONSUL_VER=1.15.4
        ARCH="$(dpkg --print-architecture)"
        case "$ARCH" in
            amd64) CONSUL_ARCH=amd64 ;;
            arm64) CONSUL_ARCH=arm64 ;;
            *) die "未知的 CPU 架构: $ARCH，请手动下载 Consul" \
                   "https://developer.hashicorp.com/consul/downloads" ;;
        esac
        TMP="$(mktemp -d)"
        log "下载 consul ${CONSUL_VER} (${CONSUL_ARCH})"
        curl -fsSL -o "$TMP/consul.zip" \
            "https://releases.hashicorp.com/consul/${CONSUL_VER}/consul_${CONSUL_VER}_linux_${CONSUL_ARCH}.zip" \
            || die "下载 Consul 失败（网络不通？）" \
                   "手动下载: https://releases.hashicorp.com/consul/downloads"
        (cd "$TMP" && unzip -q consul.zip)
        install -m0755 "$TMP/consul" /usr/local/bin/consul
        rm -rf "$TMP"
        ok "consul 安装到 /usr/local/bin/consul"
    fi
    log "起开发模式: nohup consul agent -dev -client=0.0.0.0 &"
fi

# ---------------- 可选：MariaDB Server ----------------
if [ "$WITH_MYSQL" = "1" ]; then
    step "安装 MariaDB Server"
    "${APT[@]}" install -y mariadb-server
    have mysqld || have mariadbd \
        || die "安装后仍找不到 mysqld/mariadbd" "sudo apt install -y mariadb-server"
    if have systemctl && systemctl is-system-running >/dev/null 2>&1; then
        systemctl enable --now mariadb || warn "启动 mariadb 失败，手动: sudo systemctl start mariadb"
    else
        warn "容器里没有 systemd，手动启动: mysqld_safe --skip-grant-tables &"
    fi
    ok "MariaDB Server 已安装"
    log "建库建表: mysql -uroot < sql/qboot_demo.sql"
    log "跑冒烟:   ./scripts/smoke_mysql.sh --user root --pass ''"
fi

# ---------------- 可选：nginx ----------------
if [ "$WITH_NGINX" = "1" ]; then
    step "安装 nginx"
    "${APT[@]}" install -y nginx
    have nginx || die "nginx 安装失败" "sudo apt install -y nginx"
    ok "nginx 已安装"
    log "参考配置: deploy/nginx/conf.d/qboot.conf"
fi

step "完成"
cat <<EOF

依赖就绪。下一步：

    ./scripts/build.sh                 # 编译 + 安装到 /usr/local
    ./scripts/test_all.sh              # 跑单元测试
    ./scripts/run_demo.sh              # 起 user-svc / order-svc 跑一遍

可选组件没装的，用这些开关补：
    sudo ./scripts/install_deps_ubuntu.sh --with-consul --with-mysql-server --with-nginx
EOF
