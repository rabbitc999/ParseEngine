#pragma once

#include <string>

namespace transfer {

/** 从本项目生成且保留 t1t 载荷的 LLVM IR 导出 Hydra86 原生汇编。 */
std::string generateHydra86FromLlvm(const std::string& llvmIr);

} // namespace transfer
