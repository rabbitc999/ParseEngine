/**
 * @file manual_data.hpp
 * @brief 内联在可执行文件里的手册数据（由 tools/embed_manual.py 生成）。
 *
 * `ParseEngine man` 子命令靠这份数据在终端离线查询：文档页正文（中英双语）、
 * 文法指令表、生成模式表、`%lang` 目标语言与示例清单。正文体积大，构建期用
 * library/lztext 压缩后内联，运行时 `text()` 解压一次再按偏移切片返回；
 * 运行时不读仓库里的任何文件。
 *
 * 数据来源：`doc/`、`doc/en/` 下的 Markdown 与 `example/manifest.tsv`。
 */

#ifndef PARSEENGINE_MANUAL_DATA_HPP
#define PARSEENGINE_MANUAL_DATA_HPP

#include <cstddef>
#include <string>

namespace ParseEngine
{
namespace Manual
{

/// 一份文档页的元信息（正文另存，见 `sliceAt`）。
struct ManualPage
{
    const char *id;       ///< 短编号（`01` … `16`）；无编号或与别的页同号时是空串
    const char *stem;     ///< 文件名去扩展名，如 `01-overview`
    const char *file;     ///< 文件名，如 `01-overview.md`
    const char *title;    ///< 中文正文里的一级标题
    const char *titleEn;  ///< 英文正文里的一级标题；没有英文版时是空串
    const char *summary;  ///< 正文首段的一句话摘要（可能为空）
    bool hasEnglish;      ///< `doc/en/<文件>` 是否存在
};

/// 一条「名字 / 用法 / 说明」条目：指令、模式、目标语言、示例共用这个形状。
struct ManualEntry
{
    const char *name;      ///< 名称，如 `%token`、`lalr`、`python`
    const char *usage;     ///< 用法或登记值（可能为空）
    const char *summary;   ///< 一句中文说明（可能为空）
    const char *summaryEn; ///< 对应的英文说明；没有英文来源时是空串
};

/**
 * @brief 取解压后的全部文档正文（中文版与英文版按页序拼接）。
 * @return 进程内唯一的只读字符串；解压失败时是空串。
 */
const std::string &text();

/// 文档页数量。
std::size_t pageCount();
/// 第 index 页的元信息（index 必须小于 pageCount()）。
const ManualPage &pageAt(std::size_t index);

/// 正文切片数量。
std::size_t sliceCount();
/// 第 index 段切片的正文；下标越界时是空串。
std::string sliceAt(std::size_t index);
/// 第 index 段切片的键：`page/<文件>`（中文）或 `page-en/<文件>`（英文）。
const char *sliceKeyAt(std::size_t index);

/// 文法指令数量（`%token` / `%mode` …）。
std::size_t directiveCount();
/// 第 index 条指令。
const ManualEntry &directiveAt(std::size_t index);

/// 生成模式数量（固定十六种，顺序与文档矩阵表一致）。
std::size_t modeCount();
/// 第 index 个生成模式。
const ManualEntry &modeAt(std::size_t index);

/// 目标语言数量（八种代码后端 + `gram` 导出档）。
std::size_t languageCount();
/// 第 index 个目标语言。
const ManualEntry &languageAt(std::size_t index);

/// 示例文法数量。
std::size_t exampleCount();
/// 第 index 个示例。
const ManualEntry &exampleAt(std::size_t index);

} // namespace Manual
} // namespace ParseEngine

#endif // PARSEENGINE_MANUAL_DATA_HPP
