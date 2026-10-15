# 安徽新华学院编译原理实验报告（实验一 / 二 / 三）

这是安徽新华学院编译原理课程的三份实验报告。三个实验都用本仓库的 **ParseEngine** 实现：词法与语法规则写在 `.gram` 里，
生成期做词法/语法分析，产物是可编译的 C99 工程（本目录统一用 `--lang c`）。

| 实验 | 内容 | 文法文件 | 报告 |
| --- | --- | --- | --- |
| 实验一 | 文法类型的判断（Chomsky 0 / 1 / 2 / 3 型 + 四元组信息） | [`lab1_grammar_type/lab1_grammar_type.gram`](lab1_grammar_type/lab1_grammar_type.gram) | [`lab1_grammar_type/README.md`](lab1_grammar_type/README.md) |
| 实验二 | PL/0 词法分析程序 | [`lab2_lexer/lab2_lexer.gram`](lab2_lexer/lab2_lexer.gram) | [`lab2_lexer/README.md`](lab2_lexer/README.md) |
| 实验三 | PL/0 语法分析程序（LALR(1) + `error` 恢复） | [`lab3_parser/lab3_parser.gram`](lab3_parser/lab3_parser.gram) | [`lab3_parser/README.md`](lab3_parser/README.md)（§六 还指到 [`project/pl0-ir/`](../pl0-ir/README.md) 看完整的三元式生成） |

## 怎么跑

三个实验的用法完全一样：**生成 → 编译 → 喂输入**。

```bash
# 0) 先在仓库根目录编译出生成器（只需一次）
make

# 1) 实验一：文法类型判断
build/bin/ParseEngine project/lab_report_axhu/lab1_grammar_type/lab1_grammar_type.gram \
      --lang c --out-dir out/lab1
make -C out/lab1
./out/lab1/parser_demo < project/lab_report_axhu/lab1_grammar_type/case1.txt

# 2) 实验二：PL/0 词法分析
build/bin/ParseEngine project/lab_report_axhu/lab2_lexer/lab2_lexer.gram \
      --lang c --out-dir out/lab2
make -C out/lab2
./out/lab2/lexer_demo < project/lab_report_axhu/lab2_lexer/pl0_sample.pl0

# 3) 实验三：PL/0 语法分析
build/bin/ParseEngine project/lab_report_axhu/lab3_parser/lab3_parser.gram \
      --lang c --out-dir out/lab3
make -C out/lab3
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/good.pl0
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/bad.pl0
./out/lab3/parser_demo < project/lab_report_axhu/lab3_parser/bad_lex.pl0
```

> Windows 下 `out/` 里的产物是 `parser_demo.exe` / `lexer_demo.exe`，把上面的
> `./out/labN/xxx_demo` 换成 `out\labN\xxx_demo.exe` 即可；`make` 换 `mingw32-make`
> 或 `cmd /c "make -C out\lab1"`。

## 三个实验分别用到了 ParseEngine 的什么

| 实验 | 用到的能力 | 说明 |
| --- | --- | --- |
| 实验一 | `%token regex` + `%token NL "\n"` + `%tree` | 把「一行一条产生式」的文法描述语言本身写成一个 `.gram`，语法树在 `%main` 里走一遍就能做类型判定 |
| 实验二 | `%mode lex`（只导出词法器）+ `%main` | 产物里没有解析器，只有 `lexer.c` / `lexer.h`；关键字、双字符运算符靠生成期的「最长匹配优先」 |
| 实验三 | `%mode lalr` + `%left/%right` 优先级 + `%tree` + `error` 产生式 | 二义表达式靠优先级在生成期消解；出错靠 `error` 产生式恢复，一次解析报多个错 |

## 产物里每个文件是什么

`lab1` / `lab3` 是解析器产物：`parser.c`（运行时）、`parser_table.c`（分析表）、
`parser_token.c`（词法器）、`main.c`（本实验的驱动，由 `.gram` 里的 `%main` 生成）、
`parser.h` / `parser_token.h`，外加 `Makefile` 与 `CMakeLists.txt`。

`lab2` 是词法产物：只有 `lexer.c` / `lexer.h` / `parser_token.c` / `parser_token.h` /
`main.c`，**没有分析表也没有解析循环**。
