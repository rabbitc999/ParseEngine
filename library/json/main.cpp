/**
 * @file main.cpp
 * @brief json 命令行工具：格式化 / 校验 / 取值 / 列键 / 统计。
 *
 * 用法见 `--help`；退出码：0 成功，1 解析或取值失败，2 用法错误。
 */

#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{

/**
 * @brief 打印用法。
 * @param program 程序名。
 */
void printUsage(const std::string &program)
{
    std::cout
        << "用法：" << program << " <命令> [文件] [选项]\n"
        << "\n"
        << "命令：\n"
        << "  fmt              格式化输出（--indent N 缩进，--compact 单行）\n"
        << "  check            只校验，打印诊断\n"
        << "  get <路径>       按路径取值，例如 user.tags[1].name\n"
        << "  keys [路径]      列出对象成员名（一行一个）\n"
        << "  stats            统计各类型节点数与最大深度\n"
        << "\n"
        << "选项：\n"
        << "  --indent N       缩进空格数（缺省 2）\n"
        << "  --compact        单行紧凑输出\n"
        << "  --raw            get 时字符串不加引号\n"
        << "  --out FILE       把命令输出写到文件（缺省标准输出）\n"
        << "  --comments       允许 // 与块注释\n"
        << "  --trailing-commas 允许末尾逗号\n"
        << "  -h, --help       显示这份帮助\n"
        << "\n"
        << "文件缺省从标准输入读。\n";
}

/**
 * @brief 读一个文件或标准输入。
 * @param path 文件路径；空表示标准输入。
 * @param text 结果。
 * @return 成功返回 true。
 */
bool readInput(const std::string &path, std::string &text)
{
    if (path.empty())
    {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        text = buffer.str();
        return true;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        std::cerr << "打不开文件：" << path << "\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text = buffer.str();
    return true;
}

/**
 * @brief 统计一个值的类型分布与最大深度（顺带演示迭代器的用法）。
 * @param value 值。
 * @param counters 类型计数。
 * @param depth 当前深度。
 * @param maxDepth 最大深度。
 */
void collectStats(const json::Value &value, std::map<std::string, std::size_t> &counters,
                  std::size_t depth, std::size_t &maxDepth)
{
    maxDepth = std::max(maxDepth, depth);
    ++counters[value.typeName()];

    // 1. 数组：用 STL 算法数一下"有多少个是容器"。
    if (const json::Array *items = value.asArray())
    {
        counters["array.items"] += items->size();
        const auto nested = std::count_if(items->begin(), items->end(),
                                          [](const json::Value &item) {
                                              return item.isArray() || item.isObject();
                                          });
        counters["array.nested"] += static_cast<std::size_t>(nested);
        for (const json::Value &item : *items) collectStats(item, counters, depth + 1u, maxDepth);
        return;
    }

    // 2. 对象：结构化绑定遍历。
    if (const json::Object *object = value.asObject())
    {
        counters["object.members"] += object->size();
        for (const auto &[key, item] : *object)
        {
            (void)key;
            collectStats(item, counters, depth + 1u, maxDepth);
        }
        return;
    }
}

} // namespace

/**
 * @brief 程序入口。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 退出码。
 */
int main(int argc, char **argv)
{
    const std::string program = (argc > 0) ? argv[0] : "json";
    if (argc < 2)
    {
        printUsage(program);
        return 2;
    }

    // 1. 命令与参数。
    const std::string command = argv[1];
    std::string path;
    std::string file;
    std::string outputFile;
    int indent = 2;
    bool compact = false;
    bool raw = false;
    std::vector<std::string> positionals;
    json::ParseOptions options;

    for (int index = 2; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help")
        {
            printUsage(program);
            return 0;
        }
        if (argument == "--indent")
        {
            if (index + 1 >= argc) { std::cerr << "--indent 缺少取值\n"; return 2; }
            indent = std::max(0, std::atoi(argv[++index]));
            continue;
        }
        if (argument == "--compact") { compact = true; continue; }
        if (argument == "--raw") { raw = true; continue; }
        if (argument == "--out")
        {
            if (index + 1 >= argc) { std::cerr << "--out 缺少取值\n"; return 2; }
            outputFile = argv[++index];
            continue;
        }
        if (argument == "--comments") { options.allowComments = true; continue; }
        if (argument == "--trailing-commas") { options.allowTrailingCommas = true; continue; }
        if (!argument.empty() && argument[0] == '-')
        {
            std::cerr << "不认识的选项：" << argument << "\n";
            return 2;
        }
        positionals.push_back(argument);
    }

    // 2. 位置参数按命令解释：
    //      fmt / check / stats   [文件]
    //      get <路径> [文件]
    //      keys [路径] [文件]    （只给一个时按文件算，要按路径查就写两个）
    if (command == "get")
    {
        if (positionals.empty())
        {
            std::cerr << "get 需要路径，例如 get user.tags[0] config.json\n";
            return 2;
        }
        path = positionals[0];
        if (positionals.size() > 1u) file = positionals[1];
    }
    else if (command == "keys")
    {
        if (positionals.size() == 1u) file = positionals[0];
        else if (positionals.size() >= 2u)
        {
            path = positionals[0];
            file = positionals[1];
        }
    }
    else if (!positionals.empty())
    {
        file = positionals[0];
    }
    if (positionals.size() > 2u)
    {
        std::cerr << "位置参数太多了：" << positionals[2] << "\n";
        return 2;
    }

    // 3. 读输入并解析。
    std::string text;
    if (!readInput(file, text)) return 2;

    const json::ParseResult doc = json::parse(text, options);
    if (!doc.ok())
    {
        for (const json::Diagnostic &diagnostic : doc.diagnostics)
        {
            std::cerr << (file.empty() ? "<stdin>" : file) << ":" << diagnostic.toString() << "\n";
        }
        return 1;
    }
    const json::Value &root = *doc.value;

    // 4. 输出目标：缺省标准输出，`--out` 时写文件。
    std::ofstream fileStream;
    if (!outputFile.empty())
    {
        fileStream.open(outputFile, std::ios::binary);
        if (!fileStream)
        {
            std::cerr << "写不了输出文件：" << outputFile << "\n";
            return 2;
        }
    }
    std::ostream &out = outputFile.empty() ? std::cout : fileStream;

    // 5. 分派命令。
    if (command == "fmt")
    {
        out << root.dump(compact ? -1 : indent) << "\n";
        return 0;
    }
    if (command == "check")
    {
        out << "JSON 有效：" << root.typeName() << "，顶层 "
            << (root.isArray() || root.isObject() ? root.size() : 0u) << " 项\n";
        return 0;
    }
    if (command == "get")
    {
        const json::Value *found = root.atPath(path);
        if (found == nullptr)
        {
            std::cerr << "路径不存在：" << path << "\n";
            return 1;
        }
        if (raw && found->isString()) out << found->asString().value() << "\n";
        else out << found->dump(indent) << "\n";
        return 0;
    }
    if (command == "keys")
    {
        const json::Value *target = path.empty() ? &root : root.atPath(path);
        if (target == nullptr)
        {
            std::cerr << "路径不存在：" << path << "\n";
            return 1;
        }
        if (target->isObject())
        {
            for (const auto &[key, item] : *target->asObject())
            {
                (void)item;
                out << key << "\n";
            }
            return 0;
        }
        if (target->isArray())
        {
            for (std::size_t index = 0; index < target->size(); ++index)
            {
                out << "[" << index << "]\n";
            }
            return 0;
        }
        std::cerr << "这个值既不是对象也不是数组：" << target->typeName() << "\n";
        return 1;
    }
    if (command == "stats")
    {
        std::map<std::string, std::size_t> counters;
        std::size_t maxDepth = 0;
        collectStats(root, counters, 1u, maxDepth);
        for (const auto &[name, count] : counters)
        {
            out << name << ": " << count << "\n";
        }
        out << "maxdepth: " << maxDepth << "\n";
        return 0;
    }

    std::cerr << "不认识的命令：" << command << "\n";
    printUsage(program);
    return 2;
}
