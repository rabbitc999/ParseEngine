/* ============================================================
 * 示例库文件：custom 运行时的语义值类型。
 *
 * 由 custom_tree.gram 里的 `%copy file lib/parser_value.h` 拷进生成目录，
 * 生成工程会把它算进 Makefile 的 HDRS。
 * ============================================================ */

#ifndef EXAMPLE_PARSER_VALUE_H
#define EXAMPLE_PARSER_VALUE_H

/* custom 运行时的语义值类型；%tree 文法不写语义动作，用标量即可。 */
typedef int ParserValue_T;

#endif /* EXAMPLE_PARSER_VALUE_H */
