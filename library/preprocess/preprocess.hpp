/**
 * @file preprocess.hpp
 * @brief 通用文本预处理器：%define / %undefine / %if / %include 与行号映射。
 *
 * 这个库把「源码进编译器之前的那一步」独立出来，流程参考 metro_editor 的
 * `MetroLang/Compiler/PreProcessor.cs`：
 *
 *   - 指令一律「单独一行、行首（允许缩进）」。**语言无关**：默认同时认 `#`
 *     （C / C++ / Objective-C 家族）与 `%`（yacc / `.gram` / metro_editor 家族）；
 *   - 符号：`#define NAME [VALUE]` / `#undef NAME`（C 风格），中性写法是
 *     `%symbol` / `%unsymbol`；`%define` 这种 yacc 风格要显式打开
 *     `percentDefineIsSymbol`，免得把 `.gram` 自己的 `%define NAME "正则"` 吃掉；
 *   - 条件：`if` / `ifdef` / `ifndef` / `elif` / `else` / `endif`（两个前缀都认）；
 *   - 包含：`include`、`import`（等价 `include_once`）、`include_once`、`once`、
 *     `pragma once`；同一个文件默认只展开一次，并带循环与深度保护；
 *   - 出错不中断：一路收诊断，最后一次性交给调用方（metro_editor 也是这样
 *     收集 `List<string> errors` 的）。
 *
 * 默认「**丢掉的每一行都用空行占位**」：指令行与条件为假的行都会输出一个空行，
 * 于是输出的行号与输入一一对应（`%include` 展开的地方除外），上层按原文件行号
 * 报错时不用额外换算；想要紧凑输出把 `Options::preserveLines` 关掉即可。
 *
 * 在此之上补了两件本仓库需要的东西：
 *
 *   - 条件表达式支持 `|| && ! ()`、`NAME == VALUE`、`defined(NAME)`
 *     （与 ParseEngine 生成器的 `--define` 语义保持一致）；
 *   - 输出带**行号映射**（第 i 行来自哪个文件的哪一行），需要时还能直接吐
 *     `#line` 标记，方便上层按原文件报错。
 *
 * 只依赖 C++17 标准库，不参与 ParseEngine 主程序的构建。
 */

#ifndef PARSE_ENGINE_PREPROCESS_HPP
#define PARSE_ENGINE_PREPROCESS_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace preprocess
{

/**
 * @brief 一个符号定义（`-D NAME` 或 `%define NAME VALUE`）。
 *
 * `value` 为空串表示「只定义、没有值」，此时符号为真。
 */
struct Define
{
    std::string name;
    std::string value;
};

/**
 * @brief 一次包含请求（交给自定义 loader 判断怎么处理）。
 */
struct IncludeRequest
{
    /** 指令里写的原文路径（引号内），例如 `../data/stations.csv`。 */
    std::string path;

    /** 库按惯例（当前文件目录 → includeDirs → 原样）找到的绝对路径；没找到为空。 */
    std::string resolvedPath;

    /** 包含方文件（绝对路径或 `runText` 的标签）。 */
    std::string fromFile;

    /** `%include` 相对路径的基准目录。 */
    std::string baseDir;

    /** true = `%import` 触发的，false = `%include` / `%include_once`。 */
    bool imported = false;

    /** 这次是否按"只展开一次"处理（`%import` / `%include_once` / 默认打开时为 true）。 */
    bool once = false;
};

/**
 * @brief 自定义 loader 的返回值。
 *
 * 常见用法：
 *
 *   - `useDefault()`：我不处理，交回库的默认逻辑（读文件）；
 *   - `useFile()`：路径我认了，正文仍从 `resolvedPath` 读；
 *   - `inlineText(text, label)`：正文我自己给（CSV、数据库、内存文本都行），
 *     `label` 会进依赖表、去重集合与循环检测（留空则用 `resolvedPath` / 原文路径）；
 *   - `skip()`：这次包含什么都不做；
 *   - `error(message)`：报一条 `include-loader-error` 错误。
 */
struct IncludeResult
{
    /**
     * @brief 处理方式。
     */
    enum class Action
    {
        Default, ///< 交给库的默认逻辑。
        UseFile, ///< 从 resolvedPath 读文件。
        Inline,  ///< 用 text 里的正文。
        Skip,    ///< 什么都不做。
        Error,   ///< 报错。
    };

    Action action = Action::Default;

    /** `Inline` 时展开的正文。 */
    std::string text;

    /** `Inline` / `UseFile` 时的标识（依赖表、去重、循环检测都用它）。 */
    std::string label;

    /** `Error` 时的消息。 */
    std::string message;

    /**
     * @brief 构造"我不处理"的结果。
     * @return 结果。
     */
    static IncludeResult useDefault();

    /**
     * @brief 构造"路径我认了、正文仍从文件读"的结果。
     * @param label 可选标识（留空用 resolvedPath）。
     * @return 结果。
     */
    static IncludeResult useFile(const std::string &label = std::string());

    /**
     * @brief 构造"正文我自己给"的结果。
     * @param text 正文。
     * @param label 可选标识（留空用 resolvedPath / 原文路径）。
     * @return 结果。
     */
    static IncludeResult inlineText(const std::string &text,
                                    const std::string &label = std::string());

    /**
     * @brief 构造"跳过这次包含"的结果。
     * @return 结果。
     */
    static IncludeResult skip();

    /**
     * @brief 构造"报错"的结果。
     * @param message 错误消息。
     * @return 结果。
     */
    static IncludeResult error(const std::string &message);
};

/**
 * @brief 一段「不解析」的区域（区域内的指令不当指令）。
 *
 * 默认给的是 `.gram` 的两处：`%code … %endcode`、
 * `%class_body … %endbody`。处理别的语言时把 `opaqueRegions` 换成自己的，
 * 或者清空。
 */
struct OpaqueRegion
{
    /** 起始标记（例如 `%code`）。 */
    std::string begin;

    /** 结束标记（例如 `}}%`）。 */
    std::string end;

    /**
     * 起始标记是否必须单独占一行（允许缩进）。
     *
     * true：`%code` / `%class_body` 这种「段标记」；
     * false：可出现在行内的起始标记。
     */
    bool lineAnchored = true;
};

/**
 * @brief 预处理选项。
 */
struct Options
{
    /**
     * @brief 指令写法（按语言家族选）。
     */
    enum class Syntax
    {
        Auto,    ///< 同时认 `#` 与 `%`（默认）。
        Hash,    ///< 只认 `#`：C / C++ / Objective-C 等。
        Percent, ///< 只认 `%`：yacc / `.gram` / metro_editor 等。
        Custom,  ///< 用 `directivePrefixes` 里给的前缀集合。
    };

    /** 指令写法；决定哪些前缀会被当成指令行。 */
    Syntax syntax = Syntax::Auto;

    /** `Syntax::Custom` 使用的前缀字符集合（默认给了两个都常见的）。 */
    std::string directivePrefixes = "#%";

    /** 预定义符号（`-D NAME[=VALUE]`）。 */
    std::vector<Define> defines;

    /** `%include` 的额外搜索目录（先查当前文件所在目录，再按顺序查这里）。 */
    std::vector<std::string> includeDirs;

    /**
     * 自定义包含加载器：`%include` 与 `%import` 都会先问它。
     *
     * 留空时走默认逻辑（按 `includeDirs` 找文件、读成 UTF-8 文本）。设置了以后
     * 可以把 CSV / JSON / 数据库之类的数据源接进来：
     *
     * ```cpp
     * options.includeLoader = [](const preprocess::IncludeRequest &request) {
     *     // 1. 只接管 CSV，其它交回默认逻辑
     *     if (request.path.size() < 4 ||
     *         request.path.compare(request.path.size() - 4, 4, ".csv") != 0) {
     *         return preprocess::IncludeResult::useDefault();
     *     }
     *     // 2. 读 CSV 并拼成文法能吃的文本（自己实现）
     *     const std::string text = CsvToText(request.resolvedPath);
     *     // 3. 用文件路径当标识：依赖表、去重、循环检测都认它
     *     return preprocess::IncludeResult::inlineText(text, request.resolvedPath);
     * };
     * ```
     *
     * `request.imported` 区分 `%import` 与 `%include`：想让它俩行为不同，
     * 在回调里判断这个字段即可。
     */
    std::function<IncludeResult(const IncludeRequest &)> includeLoader;

    /** 符号名是否大小写不敏感（metro_editor 是不敏感的，默认按 C 习惯区分）。 */
    bool caseInsensitive = false;

    /** 同一个文件是否默认只展开一次（metro_editor 的行为）。 */
    bool includeOnceByDefault = true;

    /** 指令行本身是否留在输出里（默认丢掉；打开便于肉眼比对）。 */
    bool keepDirectiveLines = false;

    /**
     * 丢掉的行是否用空行占位（**默认打开**）。
     *
     * 打开：指令行、条件为假的行都输出一个空行，输出行号与输入一一对应
     * （`%include` 展开处除外，那里靠 `lineMap` / `#line` 追踪）。
     * 关掉：整行删掉，输出更紧凑。
     */
    bool preserveLines = true;

    /** 是否在输出里插入 `#line 行号 "文件"` 标记（换文件时插一条）。 */
    bool emitLineMarkers = false;

    /** `#line` 标记的前缀文本。 */
    std::string lineMarkerPrefix = "#line";

    /** 最大嵌套展开深度，超过就报错（防循环包含）。 */
    std::size_t maxIncludeDepth = 32;

    /**
     * 行注释起始串列表（默认 `//`）。
     *
     * 处理别的语言时按需要加：SQL / Lua 的 `--`、Python / shell 的 `#`、
     * ini 的 `;` 都可以往这里塞；空列表表示不识别行注释。
     */
    std::vector<std::string> lineComments = {"//"};

    /**
     * 块注释「起始 / 结束」串对（默认 `/\* … *\/`）。
     *
     * 处理别的语言时可以加 `(* … *)`、`--[[ … ]]` 之类；空列表表示不识别块注释。
     */
    std::vector<std::pair<std::string, std::string>> blockComments = {{"/*", "*/"}};

    /** 字符串定界符集合（`"'` 表示单双引号都识别；空串表示不识别）。 */
    std::string stringDelimiters = "\"'";

    /**
     * 不解析的区域（区域内的指令忽略）。
     *
     * 默认是空的——这是语言无关的默认值；处理 `.gram` 时用
     * `Preprocessor::forGrammar()` 会带上 `%code … %endcode`、
     * `%class_body … %endbody` 两处。
     */
    std::vector<OpaqueRegion> opaqueRegions;

    /**
     * 不认识的指令怎么处理：
     *
     *   - `Error`（默认）：报一条 `unknown-directive` 错误，指令行丢掉；
     *   - `Keep`：原样留在输出里——处理 `.gram` 这种「`%token` / `%define`
     *     有自己的含义」的文本时用这个；
     *   - `Ignore`：静默丢掉。
     */
    enum class UnknownDirective
    {
        Error,
        Keep,
        Ignore,
    };

    /** 不认识指令的策略（见上一个字段的说明）。 */
    UnknownDirective unknownDirective = UnknownDirective::Error;

    /**
     * 是否接受 metro_editor 的 `%define` / `%undefine` 写法。
     *
     * 默认关闭：中性写法是 `%symbol` / `%unsymbol`，这样处理 `.gram` 时不会把
     * 文法自己的 `%define NAME "正则"` 吃掉。注意 `#define` / `#undef` 始终可用
     * （C 风格），不受这个开关影响；metro_editor 那种 `%define` 可以打开它。
     */
    bool percentDefineIsSymbol = false;

    /**
     * @brief 实际生效的指令前缀集合。
     * @return 前缀字符集合。
     */
    std::string prefixes() const;
};

/**
 * @brief 一条诊断。
 */
struct Diagnostic
{
    /**
     * @brief 级别。
     */
    enum class Severity
    {
        Error,   ///< 错误：调用方应当视为预处理失败。
        Warning, ///< 警告：不阻断流程。
        Note,    ///< 说明：用来补充上下文（例如包含链）。
    };

    Severity severity = Severity::Error;

    /** 稳定的诊断短码，便于测试与抑制（例如 `include-not-found`）。 */
    std::string code;

    /** 出问题的文件（`runText` 时是调用方给的标签）。 */
    std::string file;

    /** 行号（1 基；0 表示与具体行无关）。 */
    std::size_t line = 0;

    /** 列号（1 基；0 表示与具体列无关）。 */
    std::size_t column = 0;

    /** 给人看的消息。 */
    std::string message;

    /**
     * @brief 拼成 `文件:行:列: 级别: 消息 [短码]` 的形式。
     * @return 一行文本。
     */
    std::string toString() const;
};

/**
 * @brief 输出里某一行的来源。
 */
struct LineOrigin
{
    /** 来源文件（绝对路径或 `runText` 的标签）。 */
    std::string file;

    /** 来源行号（1 基）。 */
    std::size_t line = 0;
};

/**
 * @brief 预处理结果。
 */
struct Result
{
    /** 处理后的正文。 */
    std::string text;

    /**
     * @brief 行号映射：`lineMap[i - 1]` 是输出第 i 行的来源。
     *
     * 空表示「没插过 `#line` 标记，也没有 include」——此时行号与主文件一致。
     */
    std::vector<LineOrigin> lineMap;

    /** 按首次出现顺序排列的依赖文件（含主文件，绝对路径）。 */
    std::vector<std::string> dependencies;

    /** 诊断列表（按发现顺序）。 */
    std::vector<Diagnostic> diagnostics;

    /** 读入的总行数（含所有被展开的文件）。 */
    std::size_t linesIn = 0;

    /** 写出的总行数。 */
    std::size_t linesOut = 0;

    /** 展开过的 `%include` 次数（被去重跳过的也算，便于观察）。 */
    std::size_t includeCount = 0;

    /**
     * @brief 是否没有错误级诊断。
     * @return 没有错误返回 true。
     */
    bool ok() const;

    /**
     * @brief 指定级别的诊断条数。
     * @param severity 级别。
     * @return 条数。
     */
    std::size_t count(Diagnostic::Severity severity) const;
};

/**
 * @brief 预处理器。
 *
 * 一个实例可以反复用；每次 `runFile` / `runText` 都是独立的一次处理，
 * 不共享符号表或包含状态。
 */
class Preprocessor
{
public:
    /**
     * @brief 构造预处理器。
     * @param options 选项。
     */
    explicit Preprocessor(Options options = Options());

    /**
     * @brief 处理一个文件。
     * @param path 主文件路径（允许相对路径）。
     * @param out 结果（成功与失败都会填诊断，失败时正文可能不完整）。
     * @return 没有错误诊断返回 true。
     */
    bool runFile(const std::string &path, Result &out);

    /**
     * @brief 处理一段文本。
     * @param text 文本内容。
     * @param label 诊断里显示的文件名（例如 `<memory>`）。
     * @param baseDir `%include` 相对路径的基准目录。
     * @param out 结果。
     * @return 没有错误诊断返回 true。
     */
    bool runText(const std::string &text, const std::string &label,
                 const std::string &baseDir, Result &out);

    /**
     * @brief 求值一个条件表达式。
     *
     * 语法（优先级从低到高）：
     *
     *   或  := 与 ('||' 与)*
     *   与  := 非 ('&&' 非)*
     *   非  := '!' 非 | 比较
     *   比较 := 原子 (('==' | '!=') 字面量)?
     *   原子 := NAME | 'defined' '(' NAME ')' | '(' 或 ')'
     *
     * 未定义的符号为假；定义了但没值的符号为真；有值的按
     * `0/false/no/off`（大小写不敏感）与空串为假、其余为真。
     *
     * @param expression 表达式原文。
     * @param symbols 符号表。
     * @param value 求值结果（成功时写入）。
     * @param diagnostic 失败时写入诊断（列号相对表达式；允许传 nullptr）。
     * @return 表达式合法返回 true。
     */
    bool evaluate(const std::string &expression,
                  const std::map<std::string, std::string> &symbols,
                  bool &value, Diagnostic *diagnostic = nullptr) const;

    /**
     * @brief 符号名是否合法（`[A-Za-z_][A-Za-z0-9_]*`）。
     * @param name 名字。
     * @return 合法返回 true。
     */
    static bool isValidName(const std::string &name);

    /**
     * @brief 把 `NAME` / `NAME=VALUE` 原文拆成一个定义。
     * @param argument 参数原文。
     * @param out 解析结果（成功时写入）。
     * @param diagnostic 失败时写入诊断（允许传 nullptr）。
     * @return 解析成功返回 true。
     */
    static bool parseDefine(const std::string &argument, Define &out,
                            Diagnostic *diagnostic = nullptr);

    /**
     * @brief 把 `NAME=VALUE` 的 VALUE 解释成真假。
     * @param value 值的原文。
     * @return 真 / 假。
     */
    static bool parseTruthValue(const std::string &value);

    /**
     * @brief 处理 `.gram` 文本的推荐选项。
     *
     * 指令前缀只留 `%`（`.gram` 的语义动作里可能出现 `#`）；`%token` /
     * `%define` 等不属于预处理器的指令原样保留；顺带带上 `.gram` 的三处不透明区域。
     *
     * @return 选项。
     */
    static Options forGrammar();

    /**
     * @brief 处理 C / C++ 文本的推荐选项。
     *
     * 只认 `#` 前缀，注释按 `//` 与 `/\* … *\/`，不认识的指令按错误处理。
     *
     * @return 选项。
     */
    static Options forC();

    /**
     * @brief 处理 metro_editor 那类 `%define` / `%if` / `%include` 文本的推荐选项。
     *
     * 只认 `%` 前缀，并把 `%define` / `%undefine` 也当作符号定义。
     *
     * @return 选项。
     */
    static Options forMetro();

private:
    Options options_;
};

} // namespace preprocess

#endif // PARSE_ENGINE_PREPROCESS_HPP
