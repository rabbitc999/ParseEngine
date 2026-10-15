/**
 * @file code_gen_c99.hpp
 * @brief C99 后端：parser.c / parser.h / parser_token.* / parser_table.c / main.c。
 *
 * 与 `code_gen.hpp`（共用工具）分开，和 `CodeGenCpp17` / `CodeGenPython` /
 * `CodeGenCsharp` 一个路数。共用部分（`CodeGen::actionCases` /
 * `destructorSymbolGroups` / `extractSection` / `resolveLineDirectives`）由
 * `code_gen_c99.cpp` include `code_gen.hpp` 后调用。
 */

#ifndef SCRIPT_ENGINE_CODE_GEN_C99_HPP
#define SCRIPT_ENGINE_CODE_GEN_C99_HPP

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/grammar_types.hpp"

namespace ParseEngine
{

/**
 * @brief C99 产物生成器（`--lang c`）。
 */
class CodeGenC99
{
public:
    /**
     * @brief 生成 custom 模式的 parser.c。
     * @param grammar 文法。
     * @param tokens 终结符列表。
     * @param tokenDefs 固定字面量 Token。
     * @param mode 生成模式。
     * @param tree 是否 %tree（custom 下只建只存 Token 信息的语法树）。
     * @param priority 优先级配置；可为 nullptr。
     * @param lexSpec 词法扩展配置。
     * @param spaceChars %space 声明的跳过字符集；nullptr 表示没写（缺省全部空白）。
     * @param userCode %code 内容。
     * @param unionBody %union 的花括号内部原文；没写 %union 时为空。
     * @param valueType %value_type 的类型原文；没写时为空。
     * @param symbolMember 符号到 %union 成员名的映射；没写 %union 时传 nullptr。
     * @param destructors %union 成员到 %destructor 清理片段的映射；没写
     *        %destructor 时传 nullptr。
     * @param syncTokens %sync 声明的同步 Token；没写 %sync 时传 nullptr。
     * @param precomputed 已算好的分析结果；为空时本函数自行分析。
     * @param out 输出 parser.c 文本（成功时写入）。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；文法或语义动作非法时返回 false。
     */
    static bool customParserC(
        const Grammar &grammar, const std::vector<std::string> &tokens,
        const std::map<std::string, std::vector<std::string>> &tokenDefs,
        const std::string &mode, bool tree, const PriorityConfig *priority,
        const LexSpec &lexSpec, const std::string *spaceChars, bool teach,
        const std::string &userCode, const std::string &unionBody,
        const std::string &valueType,
        const std::map<std::string, std::string> *symbolMember,
        const std::map<std::string, std::string> *destructors,
        const std::vector<std::string> *syncTokens,
        const AnalysisResult *precomputed, std::string &out, Error &error);

    /**
     * @brief 生成 custom 模式的 parser.h。
     * @param defines 宏定义段。
     * @param errorType 错误类型段。
     * @param userCode %code 内容。
     * @param unionBody %union 的花括号内部原文；没写 %union 时为空。
     * @param valueType %value_type 的类型原文；没写时为空。
     * @param tree 是否 %tree。
     * @param modeLabel 生成模式标签（填模板里的 `{{ mode_name }}`）。
     * @return parser.h 文本。
     */
    static std::string customParserH(const std::string &defines,
                                     const std::string &errorType,
                                     const std::string &userCode,
                                     const std::string &unionBody,
                                     const std::string &valueType, bool tree,
                                     const std::string &modeLabel);

    /**
     * @brief 生成 custom 模式的 parser_token.h。
     *
     * 词法产物也走这份模板：那边历史上不填 `{{ mode_name }}`（产物里留的是
     * 字面量 `__MODE_NAME__`），调用方传那个字面量即可保持逐字节不变。
     * @param tokenTypes Token 类型枚举段。
     * @param lexDecls 词法声明段。
     * @param modeLabel 生成模式标签。
     * @return parser_token.h 文本。
     */
    static std::string customParserTokenH(const std::string &tokenTypes,
                                          const std::string &lexDecls,
                                          const std::string &modeLabel);

    /**
     * @brief 生成词法模式（`%mode lex`）的 lexer.h。
     *
     * 只有词法相关的声明：Token 类型 / ParserToken_T（在 parser_token.h 里）+
     * 词法器入口 Parser_LexInit / Parser_NextToken + 错误表访问。
     * @param defines `PARSER_*` 宏段（与解析产物同一套）。
     * @param errorType `ParserError_T` 段。
     * @param userCode `%code` 内容（判断要不要包含 parser_user.h）。
     * @param unionBody `%union` 正文；没有时为空。
     * @param valueType `%value_type`；没有时为空。
     * @return lexer.h 文本。
     */
    static std::string lexerOnlyH(const std::string &defines,
                                  const std::string &errorType,
                                  const std::string &userCode,
                                  const std::string &unionBody,
                                  const std::string &valueType);

    /**
     * @brief 生成词法模式（`%mode lex`）的 lexer.c。
     *
     * 内容是错误表 + 词法器 + Parser_LexInit / Parser_NextToken，没有分析栈、
     * 分析表或解析循环。
     * @param tokenDefs 固定字面量 Token。
     * @param lexSpec 词法扩展配置（`%token regex` / `%identifier`）。
     * @param spaceChars `%space` 声明的跳过字符集；nullptr 表示缺省。
     * @param userCode `%code` 内容。
     * @param unionBody `%union` 正文；没有时为空。
     * @param valueType `%value_type`；没有时为空。
     * @param symbolMember 符号到 `%union` 成员的映射；没写 `%union` 时 nullptr。
     * @return lexer.c 文本。
     */
    static std::string lexerOnlyC(
        const std::map<std::string, std::vector<std::string>> &tokenDefs,
        const LexSpec &lexSpec, const std::string *spaceChars,
        const std::string &userCode, const std::string &unionBody,
        const std::string &valueType,
        const std::map<std::string, std::string> *symbolMember);

    /**
     * @brief 生成 custom 模式的 parser_token.c。
     * @param tokenNames Token 名表。
     * @param lexDecls 词法声明段。
     * @param lexTables 词法表段。
     * @param modeLabel 生成模式标签（词法产物同 customParserTokenH 那套历史口径）。
     * @return parser_token.c 文本。
     */
    static std::string customParserTokenC(const std::string &tokenNames,
                                          const std::string &lexDecls,
                                          const std::string &lexTables,
                                          const std::string &modeLabel);

    /**
     * @brief 生成 custom 模式的 main.c。
     * @param tree 是否 %tree。
     * @param commits 文法里是否写了 %commit / %trivia（演示打印容器与位置）。
     * @param modeLabel 生成模式标签。
     * @return main.c 文本。
     */
    static std::string customMainC(bool tree, bool commits,
                                   const std::string &modeLabel);

    /**
     * @brief 用文法里 %main 段的内容生成 main.c（外壳仍是 main_custom.c 模板）。
     * @param body %main 段正文（原样贴，包含行尾）。
     * @param modeLabel 生成模式标签。
     * @return main.c 文本。
     */
    static std::string customMainCWithBody(const std::string &body,
                                           const std::string &modeLabel);

    /**
     * @brief 生成 parser_user.h（用户代码原样，%union 的类型定义追加在后面）。
     * @param userCode %code 内容。
     * @param unionBody %union 的花括号内部原文；没写 %union 时为空。
     * @param valueType %value_type 的类型原文；没写时为空。
     * @return 文件内容；用户代码 / %union / %value_type 都没有时返回空 optional。
     */
    static std::optional<std::string> customUserH(
        const std::string &userCode, const std::string &unionBody,
        const std::string &valueType);

    /**
     * @brief parser.c 里对 parser_user.h 的包含语句。
     * @param userCode %code 内容。
     * @param unionBody %union 的花括号内部原文；没写 %union 时为空。
     * @param valueType %value_type 的类型原文；没写时为空。
     * @return 包含语句；用户代码 / %union / %value_type 都没有时返回空串。
     */
    static std::string customUserInclude(const std::string &userCode,
                                         const std::string &unionBody,
                                         const std::string &valueType);

    /**
     * @brief parser.c 对 parser_table.c 里分析表的外部声明。
     * @param mode 生成模式。
     * @param analysis 分析结果。
     * @return 声明文本。
     */
    static std::string tableDeclsC(const std::string &mode,
                                   const AnalysisResult &analysis);

    /**
     * @brief 生成 %token regex / %identifier 的 DFA 表与识别接口。
     * @param tokenDefs 固定字面量 Token。
     * @param regexDefs 正则 Token（有序）。
     * @param identifierPattern 标识符正则；无值时表示未声明。
     * @param commits %commit / %trivia 声明（声明序）；没有时为空。
     * @param tokenLexOrder Token 名 -> 词法声明序（%commit 同长度比较用）。
     * @param outDecls 输出声明段。
     * @param outTables 输出表定义段。
     * @param error 出错时写入错误说明。
     * @return 成功返回 true；正则语法有误返回 false。
     */
    static bool lexDfa(
        const std::map<std::string, std::vector<std::string>> &tokenDefs,
        const std::map<std::string, std::string> &regexDefs,
        const std::optional<std::string> &identifierPattern,
        const std::vector<CommitSpec> &commits,
        const std::map<std::string, int> &tokenLexOrder,
        std::string &outDecls, std::string &outTables, Error &error);

    /**
     * @brief 生成 SLR / LALR 的 ACTION / GOTO 与产生式元数据表。
     * @param analysis 分析结果。
     * @return C 代码段。
     */
    static std::string lrTables(const AnalysisResult &analysis);

    /**
     * @brief 生成 LL(1) 的预测分析表与左递归空尾标记表。
     * @param analysis 分析结果。
     * @param terminalCount 终结符个数。
     * @param nontermCount 非终结符个数。
     * @return C 代码段。
     */
    static std::string llTables(const AnalysisResult &analysis,
                                int terminalCount, int nontermCount);

    /**
     * @brief Token 名称表内容（与枚举同序，末尾补 INVALID）。
     * @param terminals 终结符列表。
     * @return C 初始化列表文本。
     */
    static std::string tokenNames(const std::vector<std::string> &terminals);
};

} // namespace ParseEngine

#endif // SCRIPT_ENGINE_CODE_GEN_C99_HPP
