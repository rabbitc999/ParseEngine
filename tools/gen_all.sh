#!/bin/sh
# tools/gen_all.sh —— 把 example/manifest.tsv 里的每一对 (文法, 语言) 都生成一遍，
# 产物放到给定目录下 <name>.<lang>/。重构前后各跑一次，用于逐字节比对产物。
#
# 用法：tools/gen_all.sh <生成器> <输出目录>
#
# 注意：清单走 fd 3，生成器的 stdin 接 /dev/null —— 否则生成器会把还没有读到的
# 清单行吃掉（`while read < file` 里，子进程继承的就是那份清单）。
set -u

gen=$1
out=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root" || exit 2
rm -rf "$out"
mkdir -p "$out"

count=0
failed=0
exec 3< example/manifest.tsv
while IFS="$(printf '\t')" read -r name langs input <&3; do
    case "$name" in
        "" | \#*) continue ;;
    esac
    for lang in $(echo "$langs" | tr ',' ' '); do
        # GEN_ALL_LANGS=c 只生成这一档（逗号分隔）：只改某档生成器/模板时用它，
        # 别让 csharp（MSBuild+NuGet）和 rust（cargo）陪着一起跑——那两档最慢，
        # 全 5 档的 109 组要 1 分半以上，只跑 c 档约 20 秒。
        if [ -n "${GEN_ALL_LANGS:-}" ]; then
            case ",$GEN_ALL_LANGS," in
                *",$lang,"*) ;;
                *) continue ;;
            esac
        fi
        dir="$out/$name.$lang"
        if "$gen" --grammar-file "example/$name.gram" --out-dir "$dir" --lang "$lang" \
               </dev/null >"$out/$name.$lang.log" 2>&1; then
            rm -f "$out/$name.$lang.log"
        else
            echo "[FAIL] $name ($lang): $(head -1 "$out/$name.$lang.log")" >&2
            failed=$((failed + 1))
        fi
        count=$((count + 1))
    done
done
exec 3<&-
echo "生成 $count 组产物 -> $out（失败 $failed 组）"
[ "$failed" -eq 0 ]
