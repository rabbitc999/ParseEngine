# C-- 前端

本目录是南京大学编译原理实验所定义的 **C--** 前端，不是 C、C++ 或仓库中 `magel` 的别名。

## 实现边界

- `cminus.gram`：严格实现附录 A 的 Token、产生式、优先级和悬空 `else` 规则；
- `cminus_types.h`：解析动作使用的 POD 语义值接口；
- `cminus_epilogue.inc`：C-- AST、作用域符号表、类型检查和四元式生成；
- `cminus_driver.inc`：生成项目的命令行入口；
- `samples/`：合法程序、语义错误程序和必须拒绝的超集语法。

文法没有 `for`、指针、`switch`、容器、接口或扩展类型。`read()` 和 `write(int)` 作为实验运行时预定义函数登记在符号表中，它们仍通过文法既有的普通函数调用产生式解析。

## 生成

```powershell
build/bin/ParseEngine.exe `
  project/lab_report_nju/source/frontend/cminus.gram `
  --lang cpp17 --out-dir tmp/nju_lab/cminus-generated
```

随后编译生成的 `cminus_parser.cpp` 与 `cminus_main.cpp`。解析成功后，程序输出 `<前缀>.ast` 和 `<前缀>.quad`；语法错误按 `Error type B` 报告，语义错误按实验约定的错误类型编号报告。
