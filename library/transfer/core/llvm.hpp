#pragma once

#include "transfer.hpp"

#include <string>

namespace transfer {

/** 把 transfer 四元式降低为可交给 Clang 或 lli 的 LLVM IR 文本。 */
std::string generateLlvm(const Program& program);

} // namespace transfer
