# 实验源代码

这里保存五次实验实际使用的 C-- 编译器源码。实现不再借用 C-- 超集；前端的可接受语言由 `frontend/cminus.gram` 唯一定义。

```text
source/
├── frontend/   严格 C-- 文法、AST、语义检查、四元式生成和边界测试
└── backend/    IR、数据流、SSA、寄存器分配、优化器、MIPS 后端及测试
```

## 前端

| 文件 | 内容 |
|---|---|
| `frontend/cminus.gram` | 附录 A 对应的 Token、产生式、优先级与归约动作 |
| `frontend/cminus_types.h` | AST 节点与生成器动作接口 |
| `frontend/cminus_epilogue.inc` | AST、符号表、类型检查和四元式生成 |
| `frontend/cminus_driver.inc` | 生成项目的命令行入口 |
| `frontend/check.ps1` | 正例、语义错误与超集语法拒绝测试 |

ParseEngine 根据文法生成 `cminus_parser.cpp` 和 `cminus_main.cpp`。语法中不存在 `for`、指针、`switch`、容器或接口；相应输入会在语法分析阶段被拒绝。

## 后端

`backend/` 来自本仓库 `library/transfer`，提供统一三地址 IR、CFG、数据流分析、SSA、寄存器分配、局部/全局优化、MIPS 生成和模拟器。前端生成的 `.quad` 可直接送入该流水线。

## 一键验证

在仓库根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File project/lab_report_nju/run-evidence.ps1
```

脚本会重新生成并编译 C-- 前端，验证 4 个合法程序和 4 个拒绝程序（语义错误与超集语法），再构建后端、运行 8 项测试，并把前端生成的四元式送入优化与 MIPS 模拟流程。所有生成物位于 `tmp/nju_lab/`。
