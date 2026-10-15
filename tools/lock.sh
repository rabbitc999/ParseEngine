#!/bin/sh
# tools/lock.sh —— 共享工作区里的「编译锁」（多 agent / 多人同时用一棵树时必用）
#
# 为什么需要：`source/src/templates_data.cpp` 与 `build/bin/ParseEngine.exe` 是**全仓共享
# 的生成物**，而 `make -C source templates` 会把 `source/templates/` 下**所有人**的模板一起
# 内联（`.tmpl` 还优先于同名的 `.template`）。两个人同时改、同时 make，就会把对方
# 的半成品编进二进制、拿去验收，看到一堆假差异。
#
# 用法：
#   tools/lock.sh build [make 参数...]   拿锁 → `make -C source templates && make -C source` → **绿了自动放锁**
#   tools/lock.sh acquire [说明]         手动拿锁（长时间改生成器、要跨好几次 make 时用）
#   tools/lock.sh release                放锁
#   tools/lock.sh status                 看锁在谁手里（在锁内则退出码 0，被占则 1）
#   tools/lock.sh steal [说明]           抢锁（确认持有者已经死了 / 走了才用）
#
# 规则（见 AGENTS.md §「并发与编译锁」）：**编译时持锁，编译出错时继续持锁**
# （错误还没处理完，树还是你的），**改完、编译绿了才放锁**。
#
# 锁是 `tmp/build.lock` 这个**目录**：`mkdir` 在 POSIX / Windows 上都是原子的，
# 比 flock 可移植（Git Bash 没有 flock）。目录里放 owner / pid / time / note。
#
# 环境变量：LOCK_WAIT=秒  acquire 拿不到时最多等这么久（默认 0，不等待）

set -u

PATH="/usr/bin:/bin:$PATH"
export PATH

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

lock="$root/tmp/build.lock"
mkdir -p "$root/tmp" 2>/dev/null || true

release_lock() {
    if [ -d "$lock" ]; then
        # Windows/MSYS 上 rm -rf 慢得多，优先 find -delete
        find "$lock" -depth -delete 2>/dev/null || rm -rf "$lock" 2>/dev/null || true
    fi
}

holder() {
    if [ -f "$lock/owner" ]; then cat "$lock/owner"; else echo "(未记录)"; fi
}

acquire_lock() {
    _note=${1:-}
    if mkdir "$lock" 2>/dev/null; then
        {
            printf '%s\n' "${LOCK_OWNER:-$(whoami 2>/dev/null || echo agent)}"
        } >"$lock/owner"
        printf '%s\n' "$$" >"$lock/pid"
        date '+%Y-%m-%d %H:%M:%S' >"$lock/time"
        printf '%s\n' "$_note" >"$lock/note"
        echo "已上锁：$(cat "$lock/owner") @ $(cat "$lock/time")${_note:+（$_note）}"
        return 0
    fi
    return 1
}

wait_for_lock() {
    _deadline=${LOCK_WAIT:-0}
    [ "$_deadline" -gt 0 ] 2>/dev/null || return 1
    _waited=0
    while [ "$_waited" -lt "$_deadline" ]; do
        sleep 2
        _waited=$((_waited + 2))
        mkdir "$lock" 2>/dev/null && return 0
    done
    return 1
}

report_busy() {
    echo "锁被占着：$(holder)" >&2
    [ -f "$lock/time" ] && echo "  上锁时间：$(cat "$lock/time")" >&2
    [ -f "$lock/note" ] && [ -s "$lock/note" ] && echo "  说明：$(cat "$lock/note")" >&2
    echo "  —— 等对方放锁；确认对方已经不在了才用 tools/lock.sh steal" >&2
}

cmd=${1:-status}
shift 2>/dev/null || true

case "$cmd" in
    acquire)
        if acquire_lock "${1:-}"; then
            exit 0
        fi
        if wait_for_lock; then
            LOCK_OWNER="${LOCK_OWNER:-$(whoami 2>/dev/null || echo agent)}"
            acquire_lock "${1:-（等待后拿到）}" >/dev/null
            echo "等到锁了：$(holder)"
            exit 0
        fi
        report_busy
        exit 1
        ;;
    release)
        if [ ! -d "$lock" ]; then
            echo "本来就没锁"
            exit 0
        fi
        _who=$(holder)
        release_lock
        echo "已放锁（原来是 $_who 的）"
        ;;
    status)
        if [ -d "$lock" ]; then
            report_busy
            exit 1
        fi
        echo "没锁，可以编译"
        exit 0
        ;;
    steal)
        if [ -d "$lock" ]; then
            echo "抢锁：原来是 $(holder) 的" >&2
            release_lock
        fi
        _note=${1:-抢来的}
        if acquire_lock "$_note"; then
            exit 0
        fi
        echo "抢锁失败（有人同时在建锁）" >&2
        exit 1
        ;;
    build)
        # 拿锁 → 内联模板 + 编译 → 绿了放锁；**失败则保留锁**（错误还没处理完）。
        # 默认一定要拿到锁才编：别人持锁时直接退出（除非 LOCK_REUSE=1 声明
        # 「锁就是我自己的」——手动 acquire 过之后接着编时用）。
        owned=0
        if [ "${LOCK_REUSE:-0}" = "1" ]; then
            if [ ! -d "$lock" ]; then
                echo "LOCK_REUSE=1 但根本没锁——先 tools/lock.sh acquire" >&2
                exit 2
            fi
            echo "沿用已持有的锁：$(holder)"
        elif acquire_lock "tools/lock.sh build"; then
            owned=1
        else
            report_busy
            exit 2
        fi
        echo "== make -C source templates =="
        if ! PE_LOCK_HELD=1 make -C source templates; then
            echo "" >&2
            echo "模板内联失败——**锁保留着**，改完再跑 tools/lock.sh build；确认不再动了才 release。" >&2
            exit 1
        fi
        echo "== make ${*:-} =="
        if ! PE_LOCK_HELD=1 make -C source "$@"; then
            echo "" >&2
            echo "编译失败——**锁保留着**（错误还没处理完），改完再跑 tools/lock.sh build；" >&2
            echo "确认不再动了就 tools/lock.sh release。" >&2
            exit 1
        fi
        if [ "$owned" -eq 1 ]; then
            release_lock
            echo "编译通过，已自动放锁"
        else
            echo "编译通过（锁是手动 acquire 的，留着；要放就 tools/lock.sh release）"
        fi
        ;;
    *)
        echo "用法：$0 {build [make参数...] | acquire [说明] | release | status | steal [说明]}" >&2
        exit 2
        ;;
esac
