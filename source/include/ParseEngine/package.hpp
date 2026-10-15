/** @file package.hpp
 * @brief 独立 Python 发布脚本的命令行桥接，不实现 ZIP 压缩。
 */
#ifndef PARSEENGINE_PACKAGE_HPP
#define PARSEENGINE_PACKAGE_HPP
#include <string>
namespace ParseEngine {
/** @brief 运行 Python 打包脚本，参数直接传递而不经过 shell。
 * @param program 当前程序路径。 @param output 可选 ZIP 路径。 @return 子进程退出码。
 */
int packageExecutable(const char *program,const std::string &output);
}
#endif
