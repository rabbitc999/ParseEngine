#!/bin/sh
# tools/quick-verify.sh —— **快速验收**：挑一撮覆盖面最广的 C 文法，拿两份二进制
# 各生成一遍再 diff。约 10~20 秒，用来在改代码的循环里反复验；全量 109 组
# + 产物冒烟留给「提交前跑一次」。
#
# 用法：
#   tools/quick-verify.sh <新二进制> <基准二进制> [额外文法...]
#
# 例（配合 tools/isolated-build.sh 的增量模式，一轮几秒）：
#   TMP=… tools/isolated-build.sh c99                  # 增量：只重编改过的文件
#   tools/quick-verify.sh tmp/isolated/c99/build/bin/ParseEngine.exe \
#                         tmp/isolated/head/build/bin/ParseEngine.exe
#
# 默认这 16 份的挑选依据（覆盖 C99 后端的各个分支）：
#   calc / calc_prec           普通 LALR、优先级
#   lrk_demo / ielr_demo       其它 LR 模式
#   json / ini                 LL(1)、%mode lex 之外常见形态
#   pl0_ll1                    LL(1)
#   ll_follow_recovery         LL + %sync（lr_mode=false 那一支）
#   c89_subset / custom_tree   %tree（tree_mode 两支）
#   destructor_demo / union_demo %union、%destructor
#   alias_demo                 同时有 %token regex 与 %commit
#   commit_demo                只有 %commit（has_regex=false）
#   space_demo                 %space（跳空白两个分支）
#   lexer_only                 %mode lex（array_mode=false）
#
# 要临时加几份：把文法名（不含 .gram）追加在参数后面。

set -u

PATH="/usr/bin:/bin:$PATH"
export PATH

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$root" || exit 2

new=${1:-}
base=${2:-}
if [ -z "$new" ] || [ -z "$base" ]; then
    echo "用法：$0 <新二进制> <基准二进制> [额外文法...]" >&2
    exit 2
fi
shift 2

grammars="calc calc_prec lrk_demo ielr_demo json ini pl0_ll1 ll_follow_recovery \
c89_subset custom_tree destructor_demo union_demo alias_demo commit_demo \
space_demo lexer_only"
grammars="$grammars $*"

out="tmp/quick"
if [ -d "$out" ]; then
    find "$out" -depth -delete 2>/dev/null || rm -rf "$out"
fi
mkdir -p "$out/new" "$out/base" || exit 2

run_one() {
    bin=$1
    dest=$2
    name=$3
    "$bin" --grammar-file "example/$name.gram" --out-dir "$dest/$name" \
        --lang c >/dev/null 2>&1
}

ok=0
bad=""
for name in $grammars; do
    if [ ! -f "example/$name.gram" ]; then
        echo "跳过（没有 example/$name.gram）" >&2
        continue
    fi
    run_one "$new" "$out/new" "$name"
    run_one "$base" "$out/base" "$name"
    if diff -r "$out/base/$name" "$out/new/$name" >/dev/null 2>&1; then
        ok=$((ok + 1))
    else
        bad="$bad $name"
    fi
done

echo "快速验收：$ok 份一致"
if [ -n "$bad" ]; then
    echo "有差异：$bad" >&2
    echo "细看：diff -r tmp/quick/base/<文法> tmp/quick/new/<文法>" >&2
    exit 1
fi
exit 0
