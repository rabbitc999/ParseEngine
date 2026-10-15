# simple_ast Rust 版

独立 Cargo 库，把 Rust 构造的 `Program` / `Type` / `Node` 降为 `library/transfer` 与 `library/stackvm` 通用的四元式和数据区格式。

```sh
cd library/simple_ast-rust
cargo test --offline
cargo run --offline -- tmp/demo
# 输出：tmp/demo.quad、tmp/demo.data、tmp/demo.data.bin、tmp/demo.data.h、tmp/demo.layout
```

库入口 `generate(&program, "module")` 返回 `CodegenResult`；可用 `export_data_binary`、`export_data_layout` 和 `export_data_header` 导出其余文件。类型、数组、结构体、方法、运算符重载、可空引用、lambda 和带载荷枚举采用与 C++ 版相同的四元式/数据区约定。Rust 的节点构造器使用蛇形命名（例如 `Node::struct_new`、`Node::if_then_else`）。

测试覆盖数组和结构体浮点字段在 Rust VM 中运行、二进制布局、方法调用、引用诊断、枚举 tag 和 lambda。还用 C++ 栈机运行了 Rust AST 生成的产物（输出 `30`），并用 Rust 栈机运行了 C++ AST 的演示产物（输出 `30 30`）。C++ `simple_ast-tests` 的 64 项断言也在本次验证中通过。
