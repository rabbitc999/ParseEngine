# 自定义错误提示函数

C99、C++17、Python、C#、Rust、Go、Java 和 JavaScript 全部八种目标语言都支持替换错误文案，覆盖各后端当前支持的解析模式。无需修改 `.gram`，也无需编辑生成代码。回调接收错误码、行号、列号和默认提示；替换文案不会改变返回状态、结构化诊断字段、错误数量限制或恢复流程。`--lang gram` 导出文法，没有运行时回调。

## C++17

```cpp
Parser::Engine engine;
engine.setErrorFormatter([](std::int32_t code, std::uint32_t line,
                           std::uint32_t column,
                           const Parser::ParserMemory::String &message) {
    if (code == static_cast<std::int32_t>(Parser::Status::ErrLexChar))
        return Parser::ParserMemory::String("这里有无法识别的字符，请检查输入");
    return message;
});
```

`ErrorFormatter` 是 `std::function`，可捕获应用的语言设置、消息表或其他上下文。回调按 Engine 实例保存，连续解析时继续生效；`engine.setErrorFormatter({})` 恢复默认提示。返回空字符串会保留为空文案。不要在回调里重入同一个引擎；回调抛出的异常向调用方传播。

## C99

```c
#include "parser.h" /* 纯词法模式使用 lexer.h */
#include <stdio.h>

static void formatError(int32_t code, uint32_t line, uint32_t column,
                        const char *message, char *output,
                        uint32_t capacity, void *userData)
{
    const char *prefix = (const char *)userData;
    snprintf(output, capacity, "%s [%d] %u:%u %s",
             prefix, (int)code, (unsigned)line, (unsigned)column, message);
}

/* 在 Parser_RunText / Parser_LexInit 之前配置。 */
/* Parser_SetErrorFormatter(formatError, "我的应用"); */
```

`output` 的初始内容是默认文案，`message` 是独立的只读副本，因此回调可以安全地用默认提示拼接新文案。缓冲区容量包含末尾零，运行时始终强制最后一个字节为零；超长提示应由回调按容量截断。`message` 和 `output` 只在本次回调期间有效，不要保存其地址。

C API 沿用现有错误表的全局配置方式：设置跨解析保留，`Parser_SetErrorFormatter(NULL, NULL)` 恢复默认；调用方负责 `userData` 的生命周期。不要在回调里重入运行时或并发修改全局配置。

## Python

```python
engine = parser.Parser()
engine.set_error_formatter(
    lambda code, line, column, message: f"输入有误（{line}:{column}）：{message}")
engine.set_error_formatter(None)  # 恢复默认
```

回调必须返回字符串，返回空字符串表示空文案；返回其他类型会抛出 `TypeError`。设置时拒绝不可调用的非 `None` 参数。回调异常向调用方传播。

## C#

```csharp
var engine = new Parser();
engine.setErrorFormatter((code, line, column, message) =>
    $"输入有误（{line}:{column}）：{message}");
engine.setErrorFormatter(null);
```

公开委托为 `Parser.ErrorFormatter`。返回空字符串表示空文案，返回 `null` 保留默认提示。异常向调用方传播。

## Rust

```rust
let mut engine = parser::Parser::new();
engine.set_error_formatter(Some(Box::new(|code, line, column, message| {
    format!("输入有误（{}:{}）：{}", line, column, message)
})));
engine.set_error_formatter(None);
```

`parser::ErrorFormatter` 为持有的 `Box<dyn FnMut(i32, u32, u32, &str) -> String>`，支持 `move` 捕获应用上下文，返回空 `String` 表示空文案。闭包必须满足被持有对象的生命周期要求；panic 按 Rust 的常规规则传播。

## Go

```go
engine := NewParser()
engine.SetErrorFormatter(func(code, line, column int32, message string) string {
    return fmt.Sprintf("输入有误（%d:%d）：%s", line, column, message)
})
engine.SetErrorFormatter(nil)
```

公开函数类型为 `ErrorFormatter`，支持捕获上下文。返回空字符串表示空文案，panic 由调用方处理。

## Java

```java
Parser.ErrorFormatter formatter = (code, line, column, message) ->
    "输入有误（" + line + ":" + column + "）：" + message;
Parser engine = new Parser(text, formatter);
engine.setErrorFormatter(null);
```

Java 当前支持 DD、Earley、CYK。词法分析在构造函数期间执行，因此需要通过新增的构造重载配置词法错误提示；原来的 `new Parser(text)` 保持可用。`setErrorFormatter` 设置后续解析失败的提示；`errorMessage()` 返回本次解析保存的文案，重复读取不会再调用回调。词法错误仍抛出 `IllegalArgumentException`，异常消息经过回调；回调自身异常向外传播。返回 `null` 保留默认提示。

## JavaScript / TypeScript

```typescript
import { Parser } from "./parser.ts";

const engine = new Parser(text, (code, line, column, message) =>
    `输入有误（${line}:${column}）：${message}`);
engine.setErrorFormatter(null);
```

JavaScript 产物使用可直接运行的 TypeScript，当前支持 DD、Earley、CYK。构造函数的第二个可选参数覆盖词法和语法错误；词法错误仍抛出 `Error`。`setErrorFormatter` 与 `errorMessage()` 的生效时机和 Java 一致，导入模块不会执行命令行演示。直接执行 `node parser.ts` 仍保留演示行为。

Java 和 JavaScript 的默认提示沿用各自原有格式，已包含位置；回调返回的文案作为最终提示，不会再次加前缀。

## 生效时机与测试

回调在新错误进入错误表时调用一次；Java/JavaScript 在 `parse()` 返回失败前保存提示。读取错误查询函数不会再次触发。更换回调不会改写已记录的提示。GLR 等模式一次解析可能产生多条诊断，回调按记录顺序逐条执行。各语言的实例配置跨连续解析保留，实例之间互不影响；C 沿用全局配置。不要在回调中重入当前实例。增量缓存中的既有诊断保留原文案；需要按新配置重新格式化时，请执行普通完整解析。

回归运行 `python tests/error_formatter.py`，覆盖 C/C++ 词法、LL(1)、LALR、递归下降，以及 C++ GLR、Earley、CYK 的词法错误和语法错误；检查回调上下文、关闭回调、空文案及 C 缓冲区终止保护。

`python tests/error_formatter_languages.py` 覆盖其余六语言的全部支持模式，共 70 组生成工程，验证词法及语法错误、回调顺序、默认提示、位置、实例隔离、空文案和恢复默认。可用 `--langs python,java,js` 或 `--modes dd,earley,cyk` 选择子集。所有被选择的工具链都必须可用，不静默跳过；Rust 使用已缓存的 `regex` crate 离线构建。
