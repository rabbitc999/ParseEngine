/**
 * @file model_demo.cpp
 * @brief 演示"TOML ↔ 模型类"的投射：encode / decode / 诊断 / STL 算法。
 *
 * 运行：
 *   model-demo examples/config.toml
 */

#include "../bind.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

/** 一个词法规则。 */
struct Token
{
    std::string name;
    int priority = 0;
    std::optional<std::string> pattern;
};

/** 编译限制。 */
struct Limits
{
    int maxDepth = 0;
    int maxErrors = 0;
};

/** 顶层配置。 */
struct Config
{
    std::string name;
    int version = 0;
    bool debug = false;
    std::vector<std::string> targets;
    double ratio = 0.0;
    toml::DateTime released;
    Limits limits;
    std::vector<Token> tokens;
    std::map<std::string, std::string> labels;
};

// `TOML_MODEL` 是显式特化，只能写在全局作用域（类型可以放在命名空间里）。
TOML_MODEL(Token,
           toml::field("name", &Token::name),
           toml::field("priority", &Token::priority),
           toml::field("pattern", &Token::pattern));

TOML_MODEL(Limits,
           toml::field("maxDepth", &Limits::maxDepth),
           toml::field("maxErrors", &Limits::maxErrors));

TOML_MODEL(Config,
           toml::field("name", &Config::name),
           toml::field("version", &Config::version),
           toml::field("debug", &Config::debug),
           toml::field("targets", &Config::targets),
           toml::field("ratio", &Config::ratio),
           toml::field("released", &Config::released),
           toml::field("limits", &Config::limits),
           toml::field("tokens", &Config::tokens),
           toml::field("labels", &Config::labels));

/**
 * @brief 读文件。
 * @param path 路径。
 * @return 内容；打不开返回空串。
 */
std::string readFile(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/**
 * @brief 程序入口。
 * @param argc 参数个数。
 * @param argv 参数数组。
 * @return 退出码。
 */
int main(int argc, char **argv)
{
    const std::string path = (argc > 1) ? argv[1] : "examples/config.toml";

    // 1. 文本 → TOML。
    const toml::ParseResult doc = toml::parse(readFile(path));
    if (!doc.ok())
    {
        std::cerr << "解析失败：" << doc.diagnostics.front().toString() << "\n";
        return 1;
    }

    // 2. TOML → 模型。
    const toml::DecodeResult<Config> config = toml::decode<Config>(*doc.value);
    if (!config.ok())
    {
        std::cerr << "投射失败：\n";
        for (const toml::Diagnostic &item : config.diagnostics)
        {
            std::cerr << "  " << item.toString() << "\n";
        }
        return 1;
    }

    std::cout << "配置：" << config.value->name << " v" << config.value->version << "\n";
    std::cout << "目标：" << config.value->targets.size() << " 个，限制 maxDepth="
              << config.value->limits.maxDepth << "\n";
    std::cout << "词法：" << config.value->tokens.size() << " 条，最高优先级="
              << std::max_element(config.value->tokens.begin(), config.value->tokens.end(),
                                  [](const Token &left, const Token &right) {
                                      return left.priority < right.priority;
                                  })
                     ->priority
              << "\n";
    std::cout << "发布：" << config.value->released.toString() << "\n";
    for (const auto &[key, text] : config.value->labels)
    {
        std::cout << "标签 " << key << " = " << text << "\n";
    }

    // 3. 模型 → TOML。
    std::cout << "\n重新编码：\n" << toml::encode(*config.value).dump();
    return 0;
}
