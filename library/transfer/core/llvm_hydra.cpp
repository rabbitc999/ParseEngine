#include "llvm_hydra.hpp"

#include "hydra86.hpp"
#include "transfer.hpp"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

namespace transfer {
namespace {

const char* const kMarker = "; t1t-hydra-quads-base64: ";
const char* const kHashMarker = "; t1t-hydra-ir-fnv1a64: ";

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char value : text) {
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int base64Value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

std::string decodeBase64(const std::string& encoded) {
    std::string result;
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (char value : encoded) {
        if (value == '=') break;
        if (std::isspace(static_cast<unsigned char>(value))) continue;
        const int digit = base64Value(value);
        if (digit < 0) throw std::runtime_error("LLVM IR 中的 Hydra86 载荷不是合法 Base64");
        accumulator = (accumulator << 6u) | static_cast<unsigned>(digit);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            result.push_back(static_cast<char>((accumulator >> bits) & 0xffu));
        }
    }
    return result;
}

} // namespace

std::string generateHydra86FromLlvm(const std::string& llvmIr) {
    const std::size_t marker = llvmIr.find(kMarker);
    if (marker == std::string::npos)
        throw std::runtime_error(
            "LLVM IR 没有 t1t Hydra86 载荷；仅支持 transfer --target=llvm 生成且未删除载荷的 .ll");
    const std::size_t begin = marker + std::char_traits<char>::length(kMarker);
    const std::size_t end = llvmIr.find_first_of("\r\n", begin);
    const std::size_t hashMarker = llvmIr.find(kHashMarker, end);
    if (hashMarker == std::string::npos)
        throw std::runtime_error("LLVM IR 缺少 t1t Hydra86 完整性校验");
    const std::size_t hashBegin = hashMarker + std::char_traits<char>::length(kHashMarker);
    const std::size_t hashEnd = llvmIr.find_first_of("\r\n", hashBegin);
    if (hashEnd == std::string::npos)
        throw std::runtime_error("LLVM IR 中的 t1t Hydra86 完整性校验没有正文");
    std::size_t bodyBegin = hashEnd;
    if (bodyBegin < llvmIr.size() && llvmIr[bodyBegin] == '\r') ++bodyBegin;
    if (bodyBegin < llvmIr.size() && llvmIr[bodyBegin] == '\n') ++bodyBegin;
    std::uint64_t expectedHash = 0;
    try {
        expectedHash = static_cast<std::uint64_t>(std::stoull(llvmIr.substr(hashBegin,
                                                                           hashEnd - hashBegin)));
    } catch (const std::exception&) {
        throw std::runtime_error("LLVM IR 中的 t1t Hydra86 完整性校验无效");
    }
    if (fnv1a64(llvmIr.substr(bodyBegin)) != expectedHash)
        throw std::runtime_error(
            "LLVM IR 正文已被修改；Hydra86 载荷只适用于未经改写的 transfer 产物");
    const std::string quads = decodeBase64(llvmIr.substr(begin, end - begin));
    std::istringstream input(quads);
    Program program = read(input);
    if (program.empty()) throw std::runtime_error("LLVM IR 中的 t1t Hydra86 载荷为空");
    return generateHydra86(program);
}

} // namespace transfer
