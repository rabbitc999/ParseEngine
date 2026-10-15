#pragma once

#include "transfer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace transfer {

/** Hydra86 嵌入执行结果；未链接 Hydra86 时 available 为 false。 */
struct Hydra86Execution {
    bool available = false;
    bool ok = false;
    std::uint32_t exitCode = 0;
    std::uint64_t cycles = 0;
    std::string output;
    std::string psf;
    std::string error;
};

/** Hydra86 可执行镜像；未链接 Hydra86 时 available 为 false。 */
struct Hydra86Binary {
    bool available = false;
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
};

/** 把 transfer 四元式降低成 Hydra86 原生汇编。 */
std::string generateHydra86(const Program& program);

/** 使用 Hydra86 官方汇编器把原生汇编编译为 v2 变长 `.bin` 镜像。 */
Hydra86Binary compileHydra86(const std::string& assembly);

/** 当前构建是否链接了 Hydra86 的稳定嵌入 API。 */
bool hydra86Embedded();

/** 使用 Hydra86::runAssembly 装配并执行；未嵌入时返回明确诊断。 */
Hydra86Execution runHydra86(const std::string& assembly);

/** 使用 Hydra86::runImage 执行已编译的 `.bin` 镜像。 */
Hydra86Execution runHydra86Binary(const std::vector<std::uint8_t>& image);

} // namespace transfer
