/** @file code_gen_java.hpp @brief Java DD / Earley / CYK 代码生成后端。 */
#ifndef PARSE_ENGINE_CODE_GEN_JAVA_HPP
#define PARSE_ENGINE_CODE_GEN_JAVA_HPP

#include <map>
#include <string>

#include "ParseEngine/analysis.hpp"
#include "ParseEngine/error.hpp"
#include "ParseEngine/project.hpp"

namespace ParseEngine
{
class CodeGenJava
{
public:
    static bool generateProject(const ProjectRequest &request,
                                const AnalysisResult &analysis,
                                std::map<std::string, std::string> &out,
                                Error &error);
};
} // namespace ParseEngine
#endif
