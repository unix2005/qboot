#!/usr/bin/env bash
#
# 编译 qboot：静态库(.a) + 动态库(.so) + 样例 + 测试，可选 install 到系统目录。
#
#   ./scripts/build.sh                       # 默认 Release，装到 /usr/local
#   ./scripts/build.sh --no-install          # 只编译，产物在 build/lib build/bin
#   ./scripts/build.sh --type Debug --clean  # 重新 Debug 构建
#   ./scripts/build.sh --prefix /opt/qboot   # 装到别处
#   ./scripts/build.sh --asan                # ASan 构建（排查内存问题用）
#
# 装完以后写自己的服务：
#   cc -o my_svc my_svc.c $(pkg-config --cflags --libs qboot)
# 或
#   find_package(qboot REQUIRED)  +  target_link_libraries(my_svc PRIVATE qboot::q_http ...)
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "编译并安装 qboot"
arg_str  "prefix"     "/usr/local"   "安装前缀"
arg_str  "type"       "Release"      "构建类型: Release|Debug|RelWithDebInfo"
arg_int  "jobs"       "$NPROC"       "并行编译数"
arg_str  "build-dir"  "build"        "构建目录（相对仓库根）"
arg_bool "shared"     1              "同时产出动态库 .so"
arg_bool "http"       1              "构建 libq_http（需要 libevent）"
arg_bool "samples"    1              "构建样例服务"
arg_bool "tools"      1              "构建运维工具"
arg_bool "tests"      1              "构建单元测试"
arg_bool "asan"       0              "开 AddressSanitizer"
arg_bool "tsan"       0              "开 ThreadSanitizer"
arg_bool "install"    1              "编译后执行 install"
arg_bool "clean"      0              "先删除构建目录"
arg_help "$@"
arg_parse "$@"

PREFIX="$(arg_get prefix)"
BTYPE="$(arg_get type)"
JOBS="$(arg_get jobs)"
BDIR="$QBOOT_ROOT/$(arg_get build-dir)"
INSTALL="$(arg_get install)"

step "环境检查"
need cmake "sudo apt install -y cmake"
need cc    "sudo apt install -y build-essential"
need pkg-config "sudo apt install -y pkg-config" || true
log "cmake : $(cmake --version | head -1)"
log "cc    : $(cc --version | head -1)"
log "prefix: $PREFIX"

# 每个库都依赖的第三方组件，提前查，缺了就给出一条能直接复制的修复命令
step "依赖检查"
missing=0
check_pc() {   # check_pc <pkg-config 名> <apt 包名>
    if have pkg-config && pkg-config --exists "$1"; then
        ok "$1 ($(pkg-config --modversion "$1" 2>/dev/null || echo '?'))"
    elif ls /usr/include/$2.h /usr/include/*/$2.h >/dev/null 2>&1; then
        ok "$2 头文件存在"
    else
        warn "缺少 $1"; missing=1
    fi
}
check_header() { # check_header <头文件> <apt 包名>
    if ls /usr/include/$1 /usr/include/*/$1 /usr/local/include/$1 >/dev/null 2>&1; then
        ok "$1"
    else
        warn "缺少 $1"; missing=1
    fi
}
check_header event2/event.h libevent-dev
check_header curl/curl.h    libcurl4-openssl-dev
check_header libxml2/libxml/xmlversion.h libxml2-dev
check_header jansson.h      libjansson-dev
if [ "$missing" = "1" ]; then
    die "缺少第三方依赖" \
        "sudo ./scripts/install_deps_ubuntu.sh"
fi

step "配置"
if [ "$(arg_get clean)" = "1" ] && [ -d "$BDIR" ]; then
    log "删除 $BDIR"
    rm -rf "$BDIR"
fi

CMAKE_ARGS=(
    -S "$QBOOT_ROOT" -B "$BDIR"
    -DCMAKE_BUILD_TYPE="$BTYPE"
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DQ_BUILD_SHARED="$(arg_get shared)"
    -DQ_BUILD_HTTP="$(arg_get http)"
    -DQ_BUILD_SAMPLES="$(arg_get samples)"
    -DQ_BUILD_TOOLS="$(arg_get tools)"
    -DQ_BUILD_TESTS="$(arg_get tests)"
    -DQ_ENABLE_ASAN="$(arg_get asan)"
    -DQ_ENABLE_TSAN="$(arg_get tsan)"
)
log "cmake ${CMAKE_ARGS[*]}"
cmake "${CMAKE_ARGS[@]}" 2>&1 | tee /tmp/qboot-cmake.log | grep -E '^--' || true
grep -qiE "^CMake Error|Could NOT find" /tmp/qboot-cmake.log \
    && die "cmake 配置失败，看完整日志: /tmp/qboot-cmake.log" \
           "sudo ./scripts/install_deps_ubuntu.sh"

step "编译（jobs=$JOBS）"
cmake --build "$BDIR" -j"$JOBS" 2>&1 | tail -40

step "产物"
echo "--- 静态库 (.a) ---"
ls -1 "$BDIR"/lib/*.a 2>/dev/null | xargs -n1 basename || warn "没有 .a"
echo "--- 动态库 (.so/.dylib) ---"
ls -1 "$BDIR"/lib/*.so "$BDIR"/lib/*.dylib 2>/dev/null | xargs -n1 basename \
    || warn "没有动态库（--shared 0 时正常）"
echo "--- 可执行文件 ---"
ls -1 "$BDIR"/bin/* 2>/dev/null | xargs -n1 basename || true

if [ "$INSTALL" != "1" ]; then
    step "跳过 install（--install 0）"
    log "产物目录: $BDIR/lib  $BDIR/bin"
    exit 0
fi

step "安装到 $PREFIX"
if [ ! -w "$(dirname "$PREFIX")" ] && [ "$(id -u)" -ne 0 ]; then
    log "需要 root 权限写入 $PREFIX，用 sudo 执行 install"
    sudo cmake --install "$BDIR"
else
    cmake --install "$BDIR"
fi
ok "已安装"

step "验证安装结果"
echo "--- 头文件 ---"
ls -1 "$PREFIX"/include/q/*.h 2>/dev/null | xargs -n1 basename | tr '\n' ' '; echo
echo "--- pkg-config ---"
if have pkg-config; then
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    pkg-config --exists qboot \
        && ok "qboot $(pkg-config --modversion qboot): $(pkg-config --libs qboot)" \
        || die "pkg-config 找不到 qboot" \
               "export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig"
else
    warn "没装 pkg-config，跳过（用 CMake 的 find_package(qboot) 一样能接入）"
fi
echo "--- CMake package ---"
ls "$PREFIX"/lib/cmake/qboot/qboot-config.cmake >/dev/null 2>&1 \
    && ok "find_package(qboot) 可用: -Dqboot_DIR=$PREFIX/lib/cmake/qboot" \
    || warn "没生成 qboot-config.cmake"

if [ "$OS" = "Linux" ] && [ "$PREFIX" != "/usr/local" ] && [ "$PREFIX" != "/usr" ]; then
    step "动态库路径"
    warn "装到非标准目录，运行时要告知动态链接器："
    echo "    echo $PREFIX/lib | sudo tee /etc/ld.so.conf.d/qboot.conf && sudo ldconfig"
    echo "  或"
    echo "    export LD_LIBRARY_PATH=$PREFIX/lib"
fi

step "完成"
cat <<EOF

下一步：
    ./scripts/test_all.sh                       # 跑单元测试
    ./scripts/run_demo.sh                       # 起 demo 跑一遍端到端

写自己的服务（两种接入方式都行）：
    cc -o my_svc my_svc.c \$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags --libs qboot)
    cp -r templates/minimal_svc my_svc && cd my_svc && cmake -S . -B build -Dqboot_DIR=$PREFIX/lib/cmake/qboot
EOF
