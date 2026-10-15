# `project/pl0-super`：PL/0 超集 → 目标机汇编（简化代码，不走 IR）

这份工程是「四元式 / IR 路线」之外的补偿方案：**不建中间表示**。文法归约时攒一棵
"简化代码"树，只做四趟能一句话说清、能量出次数的化简，然后直接发**栈式目标机汇编**，
再用自带的解释器跑出结果——每个样例都是真跑出数字的，不是只打印语法树。

换路线的理由：四元式 / IR 那条线层次多、每改一处要同时理解四五层，维护成本高而收益
没有兑现；四元式优化引擎本身仍保留在
[`../../library/transfer/README.md`](../../library/transfer/README.md)。

## 语言（PL/0 的超集）

| 分类 | 有什么 |
| --- | --- |
| 声明 | `struct`（字段表用 `( … )`，因为 `{ }`、`(* *)` 是注释）· `const` · `var 名字: int\|float\|结构体` · `array 名字[N]: 类型`（一维，元素可以是结构体）· `str` · `ref`（匿名指针：绑定一次、能读能写、**不能改绑定**）· `procedure`（可嵌套、可递归） |
| 语句 | `:=` · `if … then … [else …]` · `while … do` · `for i := a to\|downto b do` · `goto` / `label` · `call` · `write` · `begin … end` |
| 表达式 | `+ - * / %`、比较 `= <> < <= > >=`、`and or not`、括号、`int(x)` / `float(x)`、数组下标、结构体字段（`p.x`、`ps[i].a`） |
| 注释 | `{ … }`、`(* … *)`、`//` 到行尾（词法器里当 trivia 跳过） |

语义上的两个关键取舍（细节见[技术报告](技术报告.md)）：

1. **结构体按字段扁平化**：`var p: Point;` 在目标机里就是 `main$p$x`、`main$p$y` 两个变量，
   `p.x` 在语法期就解析成普通变量名，所以目标机完全不需要知道结构体。
2. **引用是"绑定一次的名字别名"**：`ref r := p.x;` 记下目标设计式，之后每次用 `r` 都展开成
   目标（能读能写）；因为绑定只发生在声明处，所以"没有改地址的权力"是**语法**保证的。

## 目录（`project/` 约定）

```
project/pl0-super/
├── lalr1/             **LALR(1) 前端**：pl0-super.gram（文法 + 语义动作）
│                      + README.md（算法说明与优缺点）
├── ll1/               **LL(1) 前端**：lexer.cpp + parser.cpp（递归下降预测分析）
│                      + README.md（算法说明与优缺点）
├── 语法分析对比.md     两种算法的对比：文法要求 / 表与内存 / 错误恢复 / 实测 / 选型
├── pl0super.hpp       库接口：词法 / 简化 / 代码生成 / 目标机
├── pl0super_detail.hpp 内部小工具（关键字表、浮点打印、常量构造）
├── simplify.cpp       三趟简化：常量折叠 / 代数化简 / 死分支
├── codegen.cpp        后缀式发射 + 窥孔 → 目标机汇编
├── vm.cpp             栈式目标机（自带解释器）
├── main.cpp           手写前端的命令行：--asm / --stats / --expect
├── samples/           正常样例（附 `{ 期望: … }`）
├── expected/          正常样例的期望输出（已逐一跑过）
├── samples-error/     出错样例
├── expected-error/    出错样例的期望报错
├── check.sh           跑样例并比对（走 lalr1/ 生成出来的前端）
├── Makefile           构建 + 自验（make grammar / tool / check / asm / stats）
└── tools/gen_diagrams.py  用代码生成技术报告里的 Mermaid 图
```

## 怎么跑

```sh
make -C project/pl0-super grammar    # 生成 LALR(1) 前端（lalr1/pl0-super.gram）
make -C project/pl0-super            # 构建 build/pl0-super（生成前端 + 库）
make -C project/pl0-super check      # 12 个正常样例 + 2 个出错样例（生成 → 目标机 → 比对期望）
make -C project/pl0-super run        # 跑 samples/array_sum.p0s
make -C project/pl0-super stats      # 跑样例并打印各趟简化次数
make -C project/pl0-super asm        # 打印 samples/struct_ref.p0s 的目标机汇编
python project/pl0-super/tools/gen_diagrams.py --check project/pl0-super/技术报告.md

# 另一条前端（LL(1)，手写递归下降）：
g++ -std=c++17 -O2 -Iproject/pl0-super -Iproject/pl0-super/ll1 \
    project/pl0-super/ll1/lexer.cpp project/pl0-super/ll1/parser.cpp \
    project/pl0-super/simplify.cpp project/pl0-super/codegen.cpp project/pl0-super/vm.cpp \
    project/pl0-super/main.cpp -o project/pl0-super/build/pl0-super-lib
```

命令行直接跑也行：

```sh
project/pl0-super/build/pl0-super samples/float_ops.p0s          # 编译 + 目标机执行
project/pl0-super/build/pl0-super --asm samples/struct_ref.p0s   # 只看汇编
project/pl0-super/build/pl0-super --stats samples/simplify_demo.p0s  # 看简化次数
```

## 样例与实测

`make -C project/pl0-super check` 的实测输出：

```
[通过] array_sum / bool_logic / downto_loop / float_div / float_ops / nested_proc
[通过] proc_rec / ref_swap / simplify_demo / string_cat / struct_array / struct_ref / while_if
[通过] div_zero_const（报错）/ ref_rebind（报错）
pl0-super：全部样例通过
```

| 样例 | 覆盖什么 | 期望输出 | 简化（折叠/代数/死分支/窥孔） | 汇编行数 |
| --- | --- | --- | --- | --- |
| `array_sum.p0s` | 数组 + `for to` + 浮点 | `30 16` | 2 / 0 / 0 / 0 | 57 |
| `downto_loop.p0s` | `for downto` | `6` | 0 / 0 / 0 / 0 | — |
| `while_if.p0s` | `while` / `if` / `goto` / `label` | `55 100` | 0 / 0 / 0 / 0 | — |
| `bool_logic.p0s` | `and` / `or` / `not` + 比较 | `1 2` | 0 / 0 / 0 / 0 | — |
| `float_ops.p0s` | 浮点算术 + `int()` | `7.5 7 1` | 0 / 0 / 0 / 0 | — |
| `float_div.p0s` | 浮点除法 + 截断 | `3.5 3` | 0 / 0 / 0 / 0 | — |
| `string_cat.p0s` | 字符串拼接 | `hello-world-42` | 0 / 0 / 0 / 0 | — |
| `proc_rec.p0s` | 过程 + 递归 | `120` | 0 / 0 / 0 / 0 | — |
| `nested_proc.p0s` | 嵌套过程读外层局部变量 | `10` | 0 / 0 / 0 / 0 | 27 |
| `struct_ref.p0s` | 结构体字段 + 引用 + 整体复制 | `7 1.5 7` | 0 / 0 / 0 / 0 | 69 |
| `struct_array.p0s` | 结构体数组 + 引用指向数组元素字段 | `6 8` | 2 / 0 / 0 / 0 | 69 |
| `simplify_demo.p0s` | **专门演示四条简化** | `46` | **6 / 2 / 3 / 2** | — |
| `samples-error/div_zero_const.p0s` | 常量除零要在编译期报 | 报错「除数是常量 0」 | — | — |
| `samples-error/ref_rebind.p0s` | 引用只能绑设计式（`a + 1` 不行） | 报错「这里要符号 ;」 | — | — |

## 状态：文法里的语义动作已经接上

**`pl0-super.gram` 是这一版的正式前端**：产生式的动作在归约时就把"简化代码"建进池子
（照 `project/pl0-ir/pl0.gram` 的分工），`%main` 拿池子拼出的 `Program` 直接跑库——
没有"先检查再让库解一遍"的重复解析。`make grammar` 出前端，`make check` 走的就是它。

两份前端的效率对比（同一台机器、2007 行样例、`-O2` 串行 5 次）：

| 指标 | 生成的前端（`build/pl0-super`） | 手写前端（`build/pl0-super-lib`） |
| --- | --- | --- |
| 端到端 | min 20.7 ms / avg **21.3 ms** | min 18.5 ms / avg **19.6 ms** |
| 可执行大小 | 2,055,291 B | 1,202,195 B |
| 前端源码 | 3940 行（`pl0sgram_parser.cpp`，机器产出） | `lexer.cpp` 156 + `parser.cpp` 894（人写） |
| 前端编译时间 | **801 ms** | 441 + 1206 = 1647 ms |

读法（完整口径见[技术报告第 8 节](技术报告.md)）：生成的那份现在**只差 ~8%**（表驱动
LALR + 池子下标 vs 递归下降的直连指针），但仍**大一倍**（分析表 + 动作层都编进了可执行）；
它赢在**编译快一倍**和**改文法只改一处**。`parser.cpp` 保留作参考实现，两条路径的输出
由同一批样例逐字比对（`make check` 用生成的，`build/pl0-super-lib` 用手写的）。

还缺 `CMakeLists.txt`（`Makefile` 已经够用，CMake 只是仓库另一边约定）。

已知限制（都写进了代码注释）：过程**没有参数**（靠全局变量与嵌套作用域）、数组只支持
**一维**、结构体字段不能是 `str`、字符串只能和字符串比、`ref` 的目标必须是设计式
（变量 / 字段 / 数组元素），不支持把整个结构体当值传给过程；`if` / `while` / `for` 的分支
**必须写 `begin … end`**。
