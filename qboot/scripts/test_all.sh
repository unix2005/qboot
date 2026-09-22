#!/usr/bin/env bash
#
# 跑 qboot 的全部单元测试，并做一次库产物的完整性检查。
#
#   ./scripts/test_all.sh                  # 跑 build/ 下的测试
#   ./scripts/test_all.sh --build          # 先编译再跑
#   ./scripts/test_all.sh --asan           # 跑 ASan 构建（内存问题排查）
#   ./scripts/test_all.sh --filter mapper  # 只跑名字含 mapper 的
#   ./scripts/test_all.sh --verbose        # 打印测试的全部输出
#
# 退出码：全部通过 0，任一失败 1。
#
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

arg_init "跑 qboot 单元测试"
arg_str  "build-dir" "build"      "构建目录（相对仓库根）"
arg_str  "filter"    ""           "只跑名字包含该子串的测试"
arg_bool "build"     0            "跑之前先编译"
arg_bool "asan"      0            "用 ASan 构建（build-asan）"
arg_bool "verbose"   0            "打印测试完整输出"
arg_help "$@"
arg_parse "$@"

BDIR="$QBOOT_ROOT/$(arg_get build-dir)"
if [ "$(arg_get asan)" = "1" ]; then BDIR="$QBOOT_ROOT/build-asan"; fi
FILTER="$(arg_get filter)"
VERBOSE="$(arg_get verbose)"

if [ "$(arg_get asan)" = "1" ]; then
    step "ASan 构建"
    if [ ! -d "$BDIR" ]; then
        cmake -S "$QBOOT_ROOT" -B "$BDIR" -DQ_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
    fi
    cmake --build "$BDIR" -j"$NPROC" 2>&1 | tail -5
elif [ "$(arg_get build)" = "1" ] || [ ! -d "$BDIR" ]; then
    step "编译"
    [ -d "$BDIR" ] || cmake -S "$QBOOT_ROOT" -B "$BDIR"
    cmake --build "$BDIR" -j"$NPROC" 2>&1 | tail -5
fi

[ -d "$BDIR/bin" ] || die "找不到 $BDIR/bin，先跑 ./scripts/build.sh"

step "库产物检查"
# pipefail 开着，glob 匹配不到时 ls 返回 2 会把脚本打断，所以都加 || true
na="$(ls -1 "$BDIR"/lib/*.a 2>/dev/null | wc -l | tr -d ' ' || true)"
ns="$(ls -1 "$BDIR"/lib/*.so 2>/dev/null | wc -l | tr -d ' ' || true)"
ndy="$(ls -1 "$BDIR"/lib/*.dylib 2>/dev/null | wc -l | tr -d ' ' || true)"
ok "静态库 .a: $na 个"
if [ "$OS" = "Darwin" ]; then
    ok "动态库 .dylib(含软链): $ndy 个"
else
    ok "动态库 .so(含软链): $ns 个"
    [ "$na" -eq 0 ] && die "一个 .a 都没有，构建有问题" "cmake --build $BDIR -j$NPROC"
fi
[ "$na" -eq 0 ] && die "一个 .a 都没有，构建有问题" "cmake --build $BDIR -j$NPROC"

step "单元测试"
total_pass=0
total_fail=0
nbin=0
failed_bins=()

for bin in "$BDIR"/bin/test_*; do
    [ -x "$bin" ] || continue
    name="$(basename "$bin")"
    [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]] && continue
    nbin=$((nbin + 1))

    out="$(cd "$QBOOT_ROOT" && "$bin" 2>&1)"
    rc=$?
    if [ "$VERBOSE" = "1" ]; then printf '%s\n' "$out"; fi

    # 各测试收尾格式不统一：passed=N failed=M / pass=N fail=M
    p="$(printf '%s' "$out" | grep -oiE 'pass(ed)?=[0-9]+' | tail -1 | grep -oE '[0-9]+' || echo 0)"
    f="$(printf '%s' "$out" | grep -oiE 'fail(ed)?=[0-9]+' | tail -1 | grep -oE '[0-9]+' || echo 0)"
    [ -z "$p" ] && p=0
    [ -z "$f" ] && f=0

    if [ "$rc" -eq 0 ] && [ "$f" -eq 0 ]; then
        ok "$(printf '%-14s pass=%-4s' "$name" "$p")"
    else
        printf '%s[ FAIL  ]%s %s (exit=%d, fail=%s)\n' "$C_RED" "$C_OFF" "$name" "$rc" "$f"
        [ "$VERBOSE" != "1" ] && printf '%s\n' "$out" | tail -20
        failed_bins+=("$name")
    fi
    total_pass=$((total_pass + p))
    total_fail=$((total_fail + f))
done

[ "$nbin" -eq 0 ] && die "没有找到任何 test_* 可执行文件" \
    "./scripts/build.sh --tests 1"

step "汇总"
echo "  测试程序: $nbin 个"
echo "  断言通过: $total_pass"
echo "  断言失败: $total_fail"

if [ "${#failed_bins[@]}" -gt 0 ] || [ "$total_fail" -gt 0 ]; then
    die "以下测试未通过: ${failed_bins[*]}" \
        "./scripts/test_all.sh --verbose --filter ${failed_bins[0]}"
fi

# ---------------- 可选：ASan 下的样例冒烟 ----------------
if [ -d "$BDIR/bin/user_svc" ] || [ -x "$BDIR/bin/user_svc" ]; then
    step "样例服务启动自检（3 秒）"
    trap cleanup_pids EXIT
    (cd "$QBOOT_ROOT" && bg_start "$BDIR/bin/user_svc" samples/conf/user_svc.ini)
    if wait_port 127.0.0.1 8081 10; then
        r="$(http_get http://127.0.0.1:8081/health)"
        assert_json "GET /health" "$r" '"status":"up"'
    else
        warn "user_svc 没能在 10s 内起来（ASan 下启动会慢），跳过"
    fi
    cleanup_pids
    trap - EXIT
fi

ok "全部通过"
