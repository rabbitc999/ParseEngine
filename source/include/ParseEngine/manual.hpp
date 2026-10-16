/**
 * @file manual.hpp
 * @brief `ParseEngine man` 子命令：在终端里离线查询内嵌手册。
 *
 * 手册数据在构建期内联进可执行文件（见 tools/embed_manual.py 与
 * ParseEngine/manual_data.hpp），运行时不需要 doc/ 目录。
 */

#ifndef PARSEENGINE_MANUAL_HPP
#define PARSEENGINE_MANUAL_HPP

namespace ParseEngine
{
namespace Manual
{

/**
 * @brief 跑 `man` 子命令（`argv[1]` 已经是 `man`）。
 * @param argc 参数个数。
 * @param argv 参数数组；`argv[0]` 是程序名。
 * @return 退出码：0 成功，1 未找到目标，2 参数错误。
 */
int runCommandLine(int argc, char **argv);

} // namespace Manual
} // namespace ParseEngine

#endif // PARSEENGINE_MANUAL_HPP
