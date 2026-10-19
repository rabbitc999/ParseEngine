# 06 构建、模板与小测

生成目标与模式范围见 [12 支持矩阵](15-language-mode-support.md)。这里的增量构建
（Make/CMake/Cargo）与生成解析器的增量解析 API 是不同概念。

## 统一回归入口

```sh
sh ci/run.sh                 # C/C++/Python 核心回归及产物/打包检查
sh ci/run.sh --all-languages # 增加其余五语言及 Rust 增量回归
sh ci/run.sh --no-python     # 仅更新内嵌资产并构建，不运行 Python 测试
sh ci/run.sh --skip-vm-tests # 兼容旧选项：仅跳过产物清单与打包检查
```

入口每次都重新生成内嵌模板和离线手册，然后增量构建生成器，避免复用过期二进制。核心检查包括 DFA/标准库一致性、扩展正则、Unicode、自定义错误提示、生成文件保护、Earley/CYK，以及 C++ 的 GLR/Earley/CYK 增量语义复用。`--no-python` 仍需要 Python 来生成构建资产，只跳过测试。

`--all-languages` 要求 C#、Rust、Go、Java、Node.js 工具链齐全，缺少工具退出 2，不静默省略语言；Rust 依赖必须已缓存。其他检查失败后仍运行剩余独立检查，最后返回 1。构建失败直接返回 1，防止后续测试使用旧生成器。每步日志位于 `tmp/ci/logs/`。`--all-languages` 不能与 `--no-python` 同用。

## 增量解析专项回归

先在仓库根目录运行 `tools/lock.sh build`，再按目标后端执行：

```sh
python tests/glr_semantic_reuse.py
python tests/glr_semantic_reuse.py --mode earley
python tests/glr_semantic_reuse.py --mode cyk
python tests/glr_incremental_rust.py
python tests/glr_incremental_rust.py --mode earley
python tests/glr_incremental_rust.py --mode cyk
```

C++17 / Rust 测试检查副作用策略、token diff、packed forest 的传递失效、稳定 ID、后缀
重映射、空产生式和完整解析结果对照。每个模式都生成表达式与 nullable 两份产物。
Rust 使用 `cargo run --offline`，需要本机已有 `regex` 依赖缓存。
生成器和 Cargo 位于不同环境时，可在生成器环境加 `--generate-only`，再到对应的
`tmp/glr-incremental-rust-tests/`（Earley/CYK 分别在子目录 `earley/`、`cyk/`）运行
`cargo run --offline --quiet --bin incremental_check`，并在其 `nullable/` 子目录再运行一次。
这些专项脚本也已纳入统一入口：C++ 默认执行，Rust 在 `--all-languages` 时执行。

## 1. 依赖

| 用途 | 依赖 |
| --- | --- |
| 构建生成器 | C++17 编译器（g++ / clang++ / MSVC） |
| 构建方式 | `make`，或 CMake ≥ 3.15 |
| 模板内联 | Python 3（只在改模板后需要重新跑） |
| 编译生成产物 | C99 产物：C99 编译器 + `libm`；C++17 产物：C++17 编译器 + `libm`（Linux/macOS 直接可用，MinGW 也可） |
| 运行 Python 产物 | Python 3（只用标准库，不需要编译） |

生成器只用 C++17 标准库，没有任何第三方库；模板在构建期被内联进可执行文件，
运行时不读 `source/templates/`。

## 2. 构建

```sh
# 方式一：Makefile，产物 build/bin/ParseEngine（Windows 下是 .exe）
make

# 方式二：CMake，产物 build/ParseEngine
cmake -S source -B build && cmake --build build -j
```

### 发布模式

```sh
# Make：-O3 -DNDEBUG，产物在 build/bin/release/，中间文件在 build/make/release/
make -C source release -j4

# CMake >= 3.21：使用 source/ 下的 release preset
cd source && cmake --preset release
cmake --build --preset release --parallel 4

# Windows 使用 MinGW 时，首次配置显式选择生成器
cd source && cmake --preset release -G "MinGW Makefiles"

# CMake 3.15–3.20，或不使用 preset 时
cmake -S source -B build/cmake-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake-release --config Release --parallel 4
```

CMake 发布产物位于 `build/cmake-release/ParseEngine`；Visual Studio 等
多配置生成器位于 `build/cmake-release/Release/`，Windows 下文件带 `.exe` 后缀。
Release 使用工具链自带的优化参数并定义 `NDEBUG`。单配置 CMake 原本就默认
使用 Release；preset 提供独立构建目录，同时为多配置生成器选定 Release。
Make 的发布产物与普通产物分开保存，`make -C source clean` 会清理这两种 Make 构建。
自定义 `CXXFLAGS` 仍会覆盖 Make 的默认编译参数。

`source/Makefile` 把源码来源写成 `wildcard src/*.cpp`，`source/CMakeLists.txt` 用
`file(GLOB ... CONFIGURE_DEPENDS)`，所以新增 `.cpp`（例如再加一种分析模式）
不需要改构建脚本。

`make -C source` 的中间文件在 `build/make/`、产物在 `build/bin/`，和 CMake 的 `build/`
互不干扰（CMake 多配置生成器会再分一层 `build/<配置>/`）。`make help` 打印
可用的目标。Windows 上装了 Git Bash / MSYS2 时 Makefile 会自动用它当 shell
（`mkdir` / `rm` 两边通吃），没装也能照常编译。

生成器自身不依赖异常：所有会失败的下层函数都是「`bool` 返回 + `Error &` 出参」
（见 `source/include/ParseEngine/error.hpp`），所以关掉异常也能构建，适合嵌入式或
`-fno-exceptions` 的工程：

```sh
make -C source CXXFLAGS='-std=c++17 -O2 -Wall -Wextra -fno-exceptions'
```

用户输入错误一律走 `Error` 出参、由 `main` 打印 `错误：...`（带行号时是
`错误：第 N 行：...`）并以退出码 1 结束；只有内联模板缺失、分段标记对不上这类
生成器自身的问题才会走 `fatalInternal()`（打印后立即终止）。

## 3. 模板内联

产物模板在 `source/templates/*.tmpl`（当前 377 份，C / C++17 / Python / C# / Rust / Go
六套以及构建脚本、报告片段），构建期由 `tools/embed_templates.py` 生成为
`source/src/templates_data.cpp`：

```sh
python3 tools/embed_templates.py     # 等价于 make -C source templates
```

脚本按文件名排序，把每个模板以 `R"PGX(...)PGX"` 原始字符串存进
`ParseEngine::embeddedTemplates()`；它还会断言内容里不含 `)PGX"`。
  改了任何 `.tmpl` 都必须重跑脚本再 `make`，否则二进制里还是旧内容。`.template`
  仍然支持，仓库自带模板已经全部是 `.tmpl`。

开发期想直接读磁盘模板（跳过内联）：

```sh
SCRIPT_ENGINE_TEMPLATES=templates ./build/bin/ParseEngine --grammar-file example/calc.gram --out-dir /tmp/calc_out
```

`Utils::loadTemplate` 先查内联表，命中就不读磁盘；设置环境变量后会优先用
磁盘目录（并按名字缓存）。发行时不要依赖它。

## 4. 小测（`make -C source test-smoke` / `make -C source test-repro`）

> ⚠️ **`tools/smoke.sh` 目前是个「禁止跑冒烟测试」的桩**（文件里只有一句 echo）：下面这些
> 用法要等它放行后才可用；**当前验收姿势**是 `tools/gen_all.sh` 全量重生成 +
> `tools/diff.sh` 逐字节比对（循环里 `tools/quick-verify.sh`），再加自己挑几份**手工跑**
> 「生成 → 目标工具链编译 → 喂 manifest 的输入 → 比对 `.expected`」。

冒烟自检在 `tools/smoke.sh`（POSIX shell），Makefile 里包成两个目标（桩放行后才可用）：

```sh
make -C source test-smoke     # 按 example/manifest.tsv：生成 + 编译 + 喂输入 + 比对期望输出
make -C source test-repro     # 同一份文法生成到两个目录，产物与输出逐字一致
```

`test-smoke` 遍历 [`example/manifest.tsv`](../example/manifest.tsv)，每行是
`name<TAB>langs<TAB>input`：`langs` 是这份文法**实测支持**的语言档
（`c` / `cpp17` / `python` / `csharp` / `rust` / `go`，逗号分隔），
`input` 是喂给演示程序的固定输入（单行，`printf '%b'` 解 `\n` 之类的转义；
留空表示只做「生成 + 编译」）。脚本对每一档生成到临时目录、按该档编译、把输入
喂进去，再跟 `example/<name>.expected`（需要按语言分开时优先用
`example/<name>.<lang>.expected`）逐行比对；两侧都做 CRLF → LF 归一化
（C# 在 Windows 上是平台行尾）。`example/` 下每个 `.gram` 都必须在
manifest 里有行，漏写会报 FAIL。

某一档的工具链没装就整档跳过并打印一句（gcc / g++ / python /
dotnet / cargo / go，可用 `CC` / `CXX` / `PY` / `DOTNET` / `CARGO` / `GO` 覆盖路径），
跳过不算失败；有 FAIL 时退出码非零，CI 友好。改了
期望行为后用 `tools/smoke.sh capture` 重录 `example/<name>.expected`（取该行第一个
可用的语言档，与其它档输出不同时另存 `<name>.<lang>.expected`）。调试用
`SMOKE_ONLY=名字`（逗号分隔）只跑指定文法，`SMOKE_KEEP=1` 保留临时目录。不带
make 也能直接跑：`tools/smoke.sh [smoke|repro|capture] [生成器路径]`。Windows 上
需要 Git Bash / MSYS2（找不到 POSIX shell 时 `make` 会提示一句并以退出码 2 结束）。

`test-repro` 不比对期望文件，而是把同一份文法生成到两个目录、编译、喂同一输入，
要求两个产物目录与两次输出都逐字一致——只做重构（不改变行为）时用它兜底。

需要人工看一份产物时，链路还是下面这条：

```sh
# 生成 + 编译（不写 --lang 时是 C++17；加 --lang c 换成 C99 产物）
# 文法文件也可以直接写在命令行上（等价于 --grammar-file 文件）
./build/bin/ParseEngine example/pl0.gram --out-dir /tmp/pl0_out --lang cpp17
make -C /tmp/pl0_out

# 喂输入：读到换行（或 EOF）就开始解析，一行一份输入
printf 'x := 1 + 2 * 3 .' | /tmp/pl0_out/parser_demo
```

`%tree` 文法打印语法树（C++17 是 `tree->print()`），带语义动作的文法打印解析
结果（走用户代码里的 `PARSER_USER_DEMO_PRINT_VALUE`）。产物自带的
`CMakeLists.txt` 同样能用：

```sh
cmake -S /tmp/pl0_out -B /tmp/pl0_out/build && cmake --build /tmp/pl0_out/build
printf 'x := 1 + 2 * 3 .' | /tmp/pl0_out/build/parser_demo
```

改了模型 / 模板后建议按同样的方式手工过一遍受影响的示例：`example/` 下 46 份
文法在清单里分别覆盖 C 37、C++17 43、Python 20、C# 18、Rust 17、Go 16 份
（GLR / Earley / CYK 支持 C++17 / Python / C# / Rust / Go；其他限制见
`example/manifest.tsv` 表头），六个完整后端加 Java / JS 窄后端一共 **155** 个（文法, 语言）组合，
覆盖 LL(0) / LL(1) / LL(k) / LR(0) / SLR / LALR / LR(1) /
IELR / LR(k) / GLR / Earley / CYK / `dd`、`%value_type` / `%union` / `%destructor` / `%tree` /
`%copy` / 正则 Token / 条件编译 / `%epilogue` / `%main` / `%space`（空白当 Token）/ `%mode lex`（只出词法器）/ `%commit` / `%trivia`，
按 `example/manifest.tsv` 的 `langs` 列逐档生成和编译；GLR 示例只走五个支持档。
另外三份特殊的是
`example/namespace_demo.gram` 只出 C++17（`%namespace` 是 C++17 专用）、
`example/prefix_demo.gram` 只出 C99（`%prefix` 只对 C99 产物有意义）、
`example/python_demo.gram`（`%class` + Python 动作）只出 `--lang python` 产物，
用下面这条链路验收（Go 档同理：`--lang go` 之后 `go build` + `go run .`）：

```sh
./build/bin/ParseEngine --grammar-file example/python_demo.gram \
    --out-dir /tmp/pydemo_out --lang python        # 不需要 make
printf '1+2*3' | python3 /tmp/pydemo_out/main.py   # 解析成功 / 解析结果: 7
python3 -m py_compile /tmp/pydemo_out/parser.py /tmp/pydemo_out/main.py
```

把 `--lang c` / `--lang cpp17` 的产物一起跑同一份输入，三方输出（含错误消息与
退出码）应当逐字一致；`%tree` 模式也一样（`printf '1+*2'` 这类错误输入
建议一起喂）。

这套「生成 + 喂输入」也是给 AI / 脚本用的验收姿势：让 AI 跑 `python3 main.py`
（或 `python3 -c "import parser; ..."`）拿两三行输出判断文法对不对，比让它把产物、
日志读进上下文省得多，也不要求它先配好 C 工具链。

手工验收的编译参数建议 `gcc / g++ -Wall -Wextra -O2`；`tools/smoke.sh` 为了跑得
快用 `-O1`（不带告警），要看告警 / 做内存检查时按这里手工加一组
`-fsanitize=address,undefined` 再编一遍。C 产物里几处「按文法不同可能用不到」的静态函数
/ 参数（`Parser_LRDiscardTop`、`Parser_LRDiscardStack`、`%tree` 下
`Parser_LRRecover` 的 `treeMode`）分别用 `PARSER_UNUSED` 与 `(void)treeMode;`
显式标掉；`Parser_SetError` 按消息字段容量截断（`%.*s`），所以带「期望 Token」
列表的长错误也不会触发 `-Wformat-truncation`。

已知的少量告警（与 `%destructor` 无关，改这些代码时不用当回归）：不用 `$token`
的文法里 C 产物会报一条 `Parser_CurrentTokenText defined but not used`；C++17 产物
的公开层 `switch (status)` 会报几条 `-Wswitch`（枚举里那几个细分错误码不需要单独
分支）。
同一份文法在 C / C++17 / Python 三档生成的产物**语义**一致（同一输入下的输出与
错误消息逐字相同），C# / Rust / Go 三档与 Python 的语义对齐（`$$` / `$n` 的
展开方式相同），可以这样回归：

```sh
for lang in c cpp17; do
    ./build/bin/ParseEngine --grammar-file example/calc.gram \
        --out-dir /tmp/calc_$lang --lang $lang && make -C /tmp/calc_$lang
    printf '1+2*3' | /tmp/calc_$lang/parser_demo
done        # 两边都应当是「解析成功 / 解析结果: 7」

# Python 产物同一份输入也要一样（示例文法没写 C 专属语义值时）
./build/bin/ParseEngine --grammar-file example/python_demo.gram \
    --out-dir /tmp/calc_py --lang python
printf '1+2*3' | python3 /tmp/calc_py/main.py

# C# 产物（用只有 %tree 的示例文法）
./build/bin/ParseEngine --grammar-file example/pl0.gram \
    --out-dir /tmp/pl0_cs --lang csharp && (cd /tmp/pl0_cs && dotnet build)
printf 'x := 1 + 2 * 3 .' | (cd /tmp/pl0_cs && dotnet run --no-build)
```

只做重构（不改变行为）时直接跑 `make -C source test-repro`（同一份文法生成两次，产物与
输出逐字一致），也可以手工用同一份文法 + 同一个 `--lang` 生成到两个目录、
喂同一批输入对比输出。另外建议对 C++17 产物跑一遍
`g++ -std=c++17 -fsanitize=address,undefined` 再喂几组输入（含报错输入），
`unique_ptr` / 容器可以保证没有泄漏，这一步主要挡越界访问。

### LR(k) / LALR(k) 的小测

用 `example/lrk_demo.gram`（`S -> A X C | A Y D | B Y C | B X D; X -> E; Y -> E`，
输入是 `a e c` / `a e d` / `b e c` / `b e d` 四个串）核对几件事：

```sh
# 1) 冲突数随模式/ k 的变化：lr0 = 9、slr / lalr = 2、lr1 / ielr / lrk = 0、
#    lalrk 对任何 k 都是 2（LALR(k) 严格弱于 LR(k)）
./build/bin/ParseEngine --grammar-file example/lrk_demo.gram --mode lrk \
    --lookahead 2 --teach --report /tmp/lrk2.txt --out-dir /tmp/lrk2
grep -c "冲突（状态" /tmp/lrk2.txt          # 0
./build/bin/ParseEngine --grammar-file example/lrk_demo.gram --mode lalrk \
    --lookahead 2 --teach --report /tmp/lalrk2.txt --out-dir /tmp/lalrk2
grep -c "冲突（状态" /tmp/lalrk2.txt        # 2

# 2) 产物能编译、能跑，且 k = 1 / 2 / 3 接受的语言一致
for k in 1 2 3; do
    ./build/bin/ParseEngine --grammar-file example/lrk_demo.gram --mode lrk \
        --lookahead $k --out-dir /tmp/lrk_$k && make -C /tmp/lrk_$k
    for s in "a e c" "b e d" "a e"; do
        printf '%s' "$s" | /tmp/lrk_$k/parser_demo >/dev/null 2>&1 \
            && echo "$k [$s] 接受" || echo "$k [$s] 拒绝"
    done
done    # 四个合法串接受、a e / b c e 拒绝

# 3) --dump-slr-tables 能看到第二层的「归约列」（列 = k 元组展望符）
./build/bin/ParseEngine --grammar-file example/lrk_demo.gram --mode lrk \
    --lookahead 2 --dump-slr-tables --out-dir /tmp/lrk2 | head -30
```

错误恢复（`error` 产生式 / `%sync`）在 k > 1 下也要一起试：把
`example/error_recovery.gram` 与 `example/sync_recovery.gram` 用
`--mode lrk --lookahead 2` 生成，喂文法注释里那几组输入，结果应与 `%mode lalr`
（k = 1）一致。

### SLR(k) / LL(k) / LL(0) 的小测

```sh
# 1) SLR(k) 与 SLR(1) 的表一样（只差虚拟起始符号名），加长 k 也解不掉
#    lrk_demo 上的冲突（因为 FOLLOW_k(X) 与 FOLLOW_k(Y) 相等）
for k in 1 2 3; do
    ./build/bin/ParseEngine --grammar-file example/lrk_demo.gram --mode slrk \
        --lookahead $k --teach --report /tmp/slrk$k.txt --out-dir /tmp/slrk$k
    echo "SLR($k) 冲突数: $(grep -c '冲突（状态' /tmp/slrk$k.txt)"    # 都是 2
done

# 2) LL(k)：LL(2) 文法在 LL(1) 下报错、LL(2)/LL(3) 通过
./build/bin/ParseEngine --grammar-file example/llk_demo.gram --mode 'LL(1)' \
    --out-dir /tmp/ll1 --report /tmp/ll1.txt || true      # 文法不是 LL(1)
./build/bin/ParseEngine --grammar-file example/llk_demo.gram --mode 'LL(2)' \
    --out-dir /tmp/ll2 && make -C /tmp/ll2
for s in "a b c" "a c" "a b"; do
    printf '%s' "$s" | /tmp/ll2/parser_demo >/dev/null 2>&1 \
        && echo "LL(2) [$s] 接受" || echo "LL(2) [$s] 拒绝"
done        # a b c / a c 接受；a b 拒绝（还差一个 Token）

# 3) LL(0)：三种写法等价，都接受 `a b`、拒绝 `a`
for mode in "LL(0)" "ll0"; do
    ./build/bin/ParseEngine --grammar-file example/ll0_demo.gram --mode "$mode" \
        --out-dir "/tmp/ll0_$mode" && make -C "/tmp/ll0_$mode" >/dev/null
    printf 'a b' | "/tmp/ll0_$mode/parser_demo" && echo "  $mode 接受 a b"
done
./build/bin/ParseEngine --grammar-file example/ll0_demo.gram --mode llk \
    --lookahead 0 --out-dir /tmp/ll0_k0     # 与上面等价
```

六档后端（`--lang c` / `cpp17` / `python` / `csharp` / `rust` / `go`）都要各跑一遍上面
第 2、3 步：LL(k) 的列是 k 元组，各档后端的查表方式不同（C 用 `#if` 分档、其余
几档用运行时常量），
只测一种后端容易漏。

另外用 `--lang gram`（[文法导出档](14-languages-and-products.md#文法导出--lang-gram)）
把同一批文法的原文件 + 分析报告导一遍：它不该生成任何代码（产物只有
`<原文件名>.gram` / `<文法名>.analysis.md` / `RESULT.md`），且导出的 `.gram`
应当与输入**逐字节相同**（`cmp 原文件 产物目录/<原文件名>.gram`）、
`<文法名>.analysis.md` 里的分析报告与 `--report` 写出来的一致。

**k 没有上限，但别真去试大 k。** `%lookahead` / `--lookahead` 收 0..INT_MAX，
超过 3 只提醒一句、不拦。想确认这条链路还在，用一个小文法跑 k = 4 / 5 就够
（`FIRST_k` / `FOLLOW_k` 与列数随 k 指数级膨胀，再大不是跑不动就是编码放不下）：

```sh
# k = 5：提醒一句，但照常生成
./build/bin/ParseEngine --grammar-file example/llk_demo.gram --mode 'LL(5)' \
    --out-dir /tmp/ll5
# 输出里会有一行「提示：k = 5 偏大——…」

# 编码放不下：报错并给出这个文法的上限（不是策略上限，是 64 位列编码放不下）
./build/bin/ParseEngine --grammar-file example/calc.gram --mode lrk \
    --lookahead 1000000000 --out-dir /tmp/lrk_big
# 错误：k = 1000000000 太大——16 个 Token 的 k 元组列编码放不进 64 位（这个文法最多 k = 15）
```

### `--define` 的用法

```sh
# 条件为真的场景：调试版多几个 Token 与日志
./build/bin/ParseEngine --grammar-file example/if_demo.gram \
    --out-dir out-debug --define LOG --define TRACE

# 条件为假的场景：同上，但压掉 trace（TRACE && !QUIET 为假）
./build/bin/ParseEngine --grammar-file example/if_demo.gram \
    --out-dir out-quiet --define TRACE --define QUIET=1

# 只想看预处理结果（打印处理后的文本 + 行号映射，不生成产物）
./build/bin/ParseEngine --grammar-file example/if_demo.gram \
    --preprocess-only --define LOG
```

`--define NAME` 等价 `NAME=1`；`=0` / `=false` / `=no` / `=off`（大小写不敏感）与
空值为假，其余非空值为真；可重复，同名后传覆盖先传。


## 5. 调试生成过程

默认行为：生成完成后控制台只打印一行
`解析完成，解析详情参考 <out-dir>/RESULT.md`，报告全文不往终端倒。

| 手段 | 作用 |
| --- | --- |
| `--report FILE` | 把分析报告写到文件（内容与 `--full-report` 贴进 `RESULT.md` 的那段逐字一致） |
| `--full-report` | `RESULT.md` 里带上整份分析报告；默认是简化版，写到「未消解冲突」就收尾 |
| `--dump-lr-automaton` | 打印 LR(0)/SLR/LALR/LR(1)/IELR 状态、项集与转移 |
| `--dump-slr-tables` | 打印 ACTION / GOTO 表 |
| `RESULT.md` | 生成信息 + 冲突消解 + 未消解冲突（加 `--full-report` 再带报告全文），产物目录里直接看 |
| `SCRIPT_ENGINE_TEMPLATES=templates` | 改模板即时生效，不用重跑内联脚本 |

改分析算法时的建议：先 `--report` / `--dump-*` 看项集与表，再按 §4 的手工小测
把受影响的示例生成 + 编译一遍；如果只是重构（不改变行为），可以用同一份文法分别
生成到两个目录、喂同一批输入对比输出。

## 6. 代码从哪里读起

- 想看命令行怎么进：`source/src/main.cpp`
- 想看文法怎么解析：`source/src/grammar_loader.cpp` + `source/include/ParseEngine/grammar_types.hpp`
- 想看十一种分析：`source/src/analysis.cpp` 与
  `source/src/analysis_{ll1,llk,lr0,slr,slrk,lalr,lrk,lr1,ielr}.cpp`
  （见 [`06-analysis.md`](06-analysis.md)）
- 想看产物怎么拼：`source/src/project.cpp` + `source/src/code_gen_c99.cpp`（C99）/
  `source/src/code_gen_cpp17.cpp`（C++17）/ `source/src/code_gen_python.cpp`（Python）/
  `source/src/code_gen_csharp.cpp`（C#）、`source/src/code_gen_rust.cpp`（Rust）、
  `source/src/code_gen_go.cpp`（Go）
  + `source/templates/`（见 [`07-codegen.md`](07-codegen.md) §12）
- 想看动作怎么翻译：`source/src/action_translator.cpp`
- 想看正则怎么编译：`source/src/regex_dfa.cpp`

## 7. 仓库里不该提交的东西

`.gitignore` 已经忽略生成物与构建物：`out/`、`*_out/`、`build/`、
`/ParseEngine`、`src/*.o`、`parser.c` / `parser.h` / `parser_user.h` 等
产物文件名，以及编辑器/系统临时文件。`source/src/templates_data.cpp` 虽然由脚本
生成，但**要提交**，这样不装 Python 也能直接 `make`。
