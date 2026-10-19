# 自定义错误提示函数

C99 与 C++17 生成的词法器和解析器支持在运行时替换错误文案。无需修改 `.gram`，也无需编辑生成代码。回调接收错误码、行号、列号和默认提示；替换文案不会改变返回状态、结构化诊断字段、错误数量限制或恢复流程。

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

## 生效时机与测试

回调在新错误进入错误表时调用一次，读取 `errors()`、`lastError()` 或 C 的错误查询函数不会再次触发。更换回调不会改写已记录的提示。增量缓存中的既有诊断保留原文案；需要按新配置重新格式化时，请执行普通完整解析。

回归运行 `python tests/error_formatter.py`，覆盖 C/C++ 词法、LL(1)、LALR、递归下降，以及 C++ GLR、Earley、CYK 的词法错误和语法错误；检查回调上下文、关闭回调、空文案及 C 缓冲区终止保护。
