# bigint-calc：大整数计算器（C++17）

一份 `.gram` + 仓库里的 [`library/big_number`](../../library/big_number/README.md)，
就是一个**只受内存限制**的整数计算器：

```sh
$ printf '2^10000\n' | ./build/parser_demo.exe | head -c 40
1995063116880758384883742162683585083823
$ printf '5000!\n' | ./build/parser_demo.exe | wc -c
16328            # 16326 位
```

解析器由 ParseEngine 生成（`--lang cpp17`），**语义值直接就是 `bigint::BigInt`**：
`%value_type bigint::BigInt` 一行，动作里的 `$1 + $3` 就是大整数加法，
生成器与运行时都不知道「大数」这回事。

## 支持什么

只支持**整数**运算（这是有意的：要小数就换 `bigfloat::BigFloat`，规则同样不用改）：

| 写法 | 含义 | 优先级 / 结合性 |
| --- | --- | --- |
| `a + b`、`a - b` | 加减 | 最低，左结合 |
| `a * b`、`a / b`、`a % b` | 乘、除、取余（**向零截断**，与 C / C++ 的整数除法一致） | 中，左结合 |
| `-a` | 一元负号（`-2^2 == -4`） | 低于 `^`，用 `%prec UMINUS` 声明 |
| `a ^ b` | 幂（指数必须是非负整数；`2^3^2 == 2^9 == 512`，右结合） | 高 |
| `a !` | 后缀阶乘（只对非负整数：`5! == 120`、`0! == 1`） | 最高 |
| `( … )` | 括号 | — |
| `# …` | 到行尾的注释（`%trivia` 在词法器里跳过） | — |

一行一个表达式，每行打印一个结果；多行输入就多行结果。
不支持变量、函数调用、小数——这些都可以后面往文法里加规则，生成器不用改。

## 怎么跑

```sh
# 0) 仓库根目录先构建生成器（本工程只用它，不改它）
make

# 1) 生成 + 编译（产物在 build/，.gitignore 忽略）
make -C project/bigint-calc            # 等价于 make build

# 2) 试几个
make -C project/bigint-calc run        # = 2^10000
printf '5000!\n' | project/bigint-calc/build/parser_demo.exe
project/bigint-calc/build/parser_demo.exe project/bigint-calc/samples/big_div.expr

# 3) 自检：9 个正常样例 + 8 个错误样例，逐个比对 expected/
make -C project/bigint-calc check
```

`make` 的目标：`generate`（只生成）、`build`（生成 + 编译）、`run`、`run-stdin`、
`check`、`clean`。

## 样例（结果位数与耗时都是实测）

| 样例 | 表达式 | 结果位数 | 耗时 |
| --- | --- | --- | --- |
| `basics.expr` | `1+2*3`、`-7/2`、`100%7` … | ≤ 2 | 22 ms |
| `pow_fact.expr` | `2^3^2`、`-2^2`、`-3!`、`(2+3)!` | ≤ 7 | 20 ms |
| `pow_10000.expr` | `2^10000` | 3011 | 20 ms |
| `huge_mul.expr` | `(2^5000+1)*(2^5000-1)`、`2^10000-1` | 3011 | 20 ms |
| `factorial_1000.expr` | `1000!` | 2568 | 23 ms |
| `factorial_5000.expr` | `5000!` | **16326** | 30 ms |
| `big_div.expr` | `2^10000/3^1000`、`2^10000%3^1000`、`(a/b)*b+a%b` | 3011 | 32 ms |
| `big_literal.expr` | 直接写 **4000 位**的字面量、再 `*9` | 4001 | 22 ms |
| `modpow.expr` | `2^20000%1000000007`、`(2^1000-1)/3`、`500!/499!` | 301 | 29 ms |

耗时含进程启动（空跑约 20 ms），机器是 Windows + g++ 13。

期望输出不是「跑一遍记下来」就完事：[`expected/`](expected) 下的每一份都用
Python 的大整数按同样语义（`/`、`%` 向零截断）重算并对过，`5000!` 的 16326 位
逐位一致。

### 错误样例

| 样例 | 输入 | 报错（stderr 首行） |
| --- | --- | --- |
| `div_zero.expr` | `1/0` | `第 2 行：除数不能为 0` |
| `neg_power.expr` | `2^-1` | `第 2 行：指数不能是负数（只支持整数幂）：-1` |
| `neg_fact.expr` | `(-1)!` | `第 2 行：阶乘只对非负整数有意义：-1!` |
| `float.expr` | `1.5` | `第 2 行：只支持整数，不支持小数：'1.5'` |
| `huge_exponent.expr` | `2^99999999999999999999` | `第 2 行：指数太大（超过 64 位）：…` |
| `syntax.expr` | `1+` | `第 2 行第 3 列：语法错误：期望 NUMBER / MINUS / LPAREN 之一，实际是 END` |
| `paren.expr` | `(2+3` | `第 2 行第 5 列：语法错误：期望 PLUS / MINUS / STAR / SLASH 等 8 个之一，实际是 END` |
| `second_line.expr` | 第 2 行才是 `1/0` | `第 3 行：除数不能为 0`（报错指到那一行） |

（样例文件第 1 行是 `#` 注释，所以表达式行号是 2 / 3。）

## 目录结构

```
project/bigint-calc/
├── bigint_calc.gram     文法 + 语义动作 + 驱动（唯一需要你写的文件）
├── local.mk             生成工程的挂载点：-I library/big_number、链接 bigint.cpp、check
├── Makefile             生成 / 编译 / 运行 / 自验
├── check.sh             跑样例并比对期望输出
├── samples/             正常样例（*.expr）
├── expected/            正常样例的期望输出（已与 Python 大整数核对）
├── samples-error/       出错样例
├── expected-error/      出错样例的期望报错首行
└── build/               生成 + 编译产物（.gitignore 忽略，随时可重建）
```

## 实现要点（照抄时值得看的四处）

1. **字面量必须绕过内建 NUMBER**。内建 `NUMBER` 会把词素解析成 `int32` / `double`，
   几百位的字面量在词法层就被判「解析失败」。所以文法里自己声明了一条：
   `%token regex NUMBER "[0-9]+([.][0-9]*)?([eE][+-]?[0-9]+)?"` —— 词法器只认词素，
   数值交给 `BigInt::fromString`（任意长度）；正则里带上小数 / 指数形式，
   是为了能在语义层给一句「只支持整数，不支持小数」的人话。

2. **语义值就是大数**。`%value_type bigint::BigInt`（生成器补
   `typedef bigint::BigInt ParserValue_T;`），动作里 `$$ = $1 + $3;` 直接调
   `BigInt` 的运算符重载。C++17 产物用 `std::vector` 管语义值栈、不 memset，
   所以带 `std::vector` 的类型可以直接当 `ParserValue_T`。

3. **语义错误不抛异常**。除零走 `divmod()`（返回 `false` 而不是抛
   `std::domain_error`），负指数 / 负阶乘 / 超大参数都在动作调用的帮助函数里
   查一遍边界，把说明记进 `Calc_State()`；驱动读完解析结果再看它，报错并退出 1。
   `%epilogue` 放实现（进 `bigcalc_parser.cpp`，是独立编译单元），`%code ... %endcode`
   只放声明（进 `bigcalc_parser_user.h`，两个编译单元共享）。

4. **优先级用声明解决，不写嵌套的非终结符**。一份 `%left` / `%right` + 一个
   `expr`，`-2^2 == -4`、`2^3^2 == 512`、`-3! == -6` 全靠
   `%left PLUS MINUS` … `%left BANG` 的先后顺序 + `%prec UMINUS`。
   生成后的 `RESULT.md` 里有每条冲突的消解记录（`build/RESULT.md`）。

## 已知限制

- **指数 / 阶乘参数要能塞进 64 位**：`2^(10^20)` 这种会明确报错，不会去算
  （算出来也没有内存放得下）。底数、字面量、结果本身没有上限。
- **除法向零截断**（与 C / C++ 一致）：`-7/2 == -3`、`-7%2 == -1`；
  `a/b != floor(a/b)` 在负数上要注意。
- 阶乘是「大数 × 小整数」的迭代乘，`100000!` 这种量级的耗时是秒级；样例停在
  `5000!`（16326 位、30 ms）就是为了让自检跑得快。
- 不支持小数、变量、函数调用、进制前缀（`0x10` 会被当成 `0` 后面跟 `x10`）。
- 编译产物的 `statusName()` 会带 4 条 `-Wswitch` 警告（生成的公开层没给
  `ErrExpected` / `ErrUnexpectedEof` / `ErrLexUnterminated` / `ErrLexChar`
  单独列分支）——这是生成器的既有行为，其它工程与
  [`../../doc/09-build-and-test.md`](../../doc/09-build-and-test.md) 里也是这么记的。

## 相关文档

- 大数库本身：[`library/big_number/README.md`](../../library/big_number/README.md)
- 优先级与 `%prec` 的规则：[`../../doc/05-grammar.md`](../../doc/05-grammar.md)
- 语义动作怎么翻译成 C++：[`../../doc/07-codegen.md`](../../doc/07-codegen.md)
- `%code ... %endcode` / `%epilogue` / `%main` 三处代码的分工：
  [`../../doc/05-grammar.md`](../../doc/05-grammar.md)、[`../../doc/08-generated-code.md`](../../doc/08-generated-code.md)
