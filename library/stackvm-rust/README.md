# stackvm Rust 版

独立 Cargo 库及命令行，读取 `library/stackvm` 使用的 `.quad`，提供解析、组装、化简、执行、数据区和原生函数。大整数复用 `library/big_number-rust`。

```sh
cd library/stackvm-rust
cargo test --offline
cargo run --offline -- ../stackvm/examples/13_heap.quad --expected
cargo run --offline -- disasm ../stackvm/examples/minimal.quad
cargo run --offline -- ../../tmp/demo.quad --data ../../tmp/demo.data.bin --layout ../../tmp/demo.layout
```

库入口是 `parse_quads` / `load_quad_file`、`optimize`、`assemble`、`Machine::run`。`DataArea::from_parts` 接收 `.data.bin` 字节与 `.layout` 文本；`Machine::set_data` 挂载数据区。支持四元式算术、分支、调用和递归、堆、下标检查、间接调用、协程，以及字符串、大整数和容器原生函数。`Program::exports` 与 C++ 版保持同一含义；裸四元式没有可见性语法，所以默认包含全部标签。

`set_time_callback` 注入宿主单调纳秒时钟，结果写入 `RunResult` 的起止和耗时字段；
`set_instruction_hook(N, ...)` 每完成 N 条四元式回调一次，`N=0` 禁用。协程的独立 PC、调用帧、
待绑定参数、等待关系及最终状态与 C++ 版保持一致，摘要位于 `RunResult.coroutines`。

目前执行器直接运行四元式；`assemble` 同时生成 `instructions` 供反汇编和检查。因此 `RunResult.steps` 统计四元式，`max_stack_depth` 尚不与 C++ 的栈指令执行统计一致。优化器保守处理常量折叠、代数化简和不可达代码，尚未实现 C++ 版的完整死赋值删除；格式化函数覆盖常见 `%d/%u/%x/%f/%s/%c`，复杂 printf 旗标的细节仍应以 C++ 版为准。

验证：Rust 测试逐个运行 `library/stackvm/examples/*.quad`，并比较优化前后的输出；另测堆错误、函数指针、协程、精确计时、指令钩子、原生函数与反汇编。
