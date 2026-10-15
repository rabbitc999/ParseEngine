/**
 * @file stackvm.cpp
 * @brief stackvm.hpp 的实现：四元式解析、组装、栈式虚拟机执行。
 */

#include "stackvm.hpp"
#include "stackvm_crypto.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <map>
#include <memory>
#include <set>
#include <sstream>

/* 大整数（`bint`）是**附加功能**：`STACKVM_WITH_BIGINT`（见 stackvm.hpp）为 1 才编进来。
 * 编进来时用仓库里的 library/big_number（`bigint::BigInt`）：加 / 减 / 乘 / 除 / 取余
 * 与十进制串互转都在那边，本库只负责"把它接到虚拟机的值语义上"（见运行期对象表）。 */
#if STACKVM_WITH_BIGINT
#include "bigint.hpp"
#endif
#if STACKVM_WITH_BIGFLOAT
#include "bigfloat.hpp"
#endif

namespace stackvm
{

bool HeapAllocator::readInt(std::size_t address, std::int64_t &out) const
{
    return readBytes(address, reinterpret_cast<char *>(&out), sizeof(out));
}

bool HeapAllocator::writeInt(std::size_t address, std::int64_t value)
{
    return writeBytes(address, reinterpret_cast<const char *>(&value), sizeof(value));
}

bool HeapAllocator::readDouble(std::size_t address, double &out) const
{
    return readBytes(address, reinterpret_cast<char *>(&out), sizeof(out));
}

bool HeapAllocator::writeDouble(std::size_t address, double value)
{
    return writeBytes(address, reinterpret_cast<const char *>(&value), sizeof(value));
}

NativeValue NativeValue::fromInt(std::int64_t value)
{
    NativeValue result;
    result.integer = value;
    return result;
}

NativeValue NativeValue::fromFloat(double value)
{
    NativeValue result;
    result.kind = Kind::Floating;
    result.floating = value;
    return result;
}

NativeValue NativeValue::fromText(std::string value)
{
    NativeValue result;
    result.kind = Kind::Text;
    result.text = std::move(value);
    return result;
}

std::int64_t NativeValue::asInt() const
{
    if (isText()) return 0;
    return isFloat() ? static_cast<std::int64_t>(floating) : integer;
}

double NativeValue::asFloat() const
{
    if (isText()) return 0.0;
    return isFloat() ? floating : static_cast<double>(integer);
}

bool NativeRegistry::add(const std::string &name, std::size_t arity, NativeCallback callback,
                         std::string *error)
{
    if (name.empty())
    {
        if (error != nullptr) *error = "原生函数名不能为空";
        return false;
    }
    if (!callback)
    {
        if (error != nullptr) *error = "原生函数 " + name + " 没有 C++ 回调";
        return false;
    }
    if (entries_.count(name) != 0u)
    {
        if (error != nullptr) *error = "原生函数重复注册：" + name;
        return false;
    }
    entries_.emplace(name, Entry{arity, std::move(callback)});
    return true;
}

bool NativeRegistry::remove(const std::string &name) { return entries_.erase(name) != 0u; }

const NativeRegistry::Entry *NativeRegistry::find(const std::string &name) const
{
    const auto found = entries_.find(name);
    return found == entries_.end() ? nullptr : &found->second;
}

std::string DataArea::readCString(std::size_t address, std::size_t limit) const
{
    if (address >= binary.size()) return std::string();
    std::string out;
    for (std::size_t index = address; index < binary.size() && out.size() < limit; ++index)
    {
        const char ch = static_cast<char>(binary[index]);
        if (ch == '\0') break;
        out.push_back(ch);
    }
    return out;
}

bool DataArea::load(const std::string &binaryPath, const std::string &layoutPath,
                    std::vector<std::string> *diagnostics)
{
    // 1. 读二进制。
    std::ifstream binaryStream(binaryPath, std::ios::binary);
    if (!binaryStream)
    {
        if (diagnostics) diagnostics->push_back("打不开数据区文件：" + binaryPath);
        return false;
    }
    binary.assign(std::istreambuf_iterator<char>(binaryStream),
                  std::istreambuf_iterator<char>());

    // 2. 读布局：一行 `符号 offset=0 size=8 type=…`。
    symbols.clear();
    std::ifstream layoutStream(layoutPath, std::ios::binary);
    if (!layoutStream)
    {
        if (diagnostics) diagnostics->push_back("打不开布局文件：" + layoutPath);
        return false;
    }
    std::string line;
    while (std::getline(layoutStream, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t space = line.find(' ');
        const std::size_t offsetAt = line.find("offset=");
        if (space == std::string::npos || offsetAt == std::string::npos) continue;
        const std::string name = line.substr(0u, space);
        const std::size_t end = line.find(' ', offsetAt);
        const std::string text = line.substr(offsetAt + 7u, end - offsetAt - 7u);
        symbols[name] = static_cast<std::size_t>(std::strtoull(text.c_str(), nullptr, 10));
        // 布局行里还有 `size=N`（这个符号占多少字节）：下标越界检查靠它算容量。
        const std::size_t sizeAt = line.find("size=");
        if (sizeAt != std::string::npos)
        {
            const std::size_t sizeEnd = line.find(' ', sizeAt);
            sizes[name] = static_cast<std::size_t>(std::strtoull(
                line.substr(sizeAt + 5u, sizeEnd - sizeAt - 5u).c_str(), nullptr, 10));
        }
        // 布局行里还有 `stride=N`（数据区槽位步长，simple_ast 固定写 8）；
        // 寻址用的是四元式里的步长，这里只读进来备查。
        const std::size_t strideAt = line.find("stride=");
        if (strideAt != std::string::npos)
        {
            const std::size_t strideEnd = line.find(' ', strideAt);
            strides[name] = static_cast<std::size_t>(std::strtoull(
                line.substr(strideAt + 7u, strideEnd - strideAt - 7u).c_str(), nullptr, 10));
        }
    }
    return true;
}

bool DataArea::readInt(std::size_t address, std::int64_t &out) const
{
    if (address + 8u > binary.size()) return false;
    std::uint64_t bits = 0;
    for (int index = 7; index >= 0; --index)
    {
        bits = (bits << 8u) |
               static_cast<unsigned char>(binary[address + static_cast<std::size_t>(index)]);
    }
    out = static_cast<std::int64_t>(bits);
    return true;
}

bool DataArea::writeInt(std::size_t address, std::int64_t value)
{
    if (address + 8u > binary.size()) return false;
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (int index = 0; index < 8; ++index)
    {
        binary[address + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((bits >> (index * 8)) & 0xFFu);
    }
    return true;
}

bool DataArea::readDouble(std::size_t address, double &out) const
{
    if (address + 8u > binary.size()) return false;
    std::uint64_t bits = 0;
    for (int index = 7; index >= 0; --index)
    {
        bits = (bits << 8u) |
               static_cast<unsigned char>(binary[address + static_cast<std::size_t>(index)]);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    out = value;
    return true;
}

bool DataArea::writeDouble(std::size_t address, double value)
{
    if (address + 8u > binary.size()) return false;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int index = 0; index < 8; ++index)
    {
        binary[address + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((bits >> (index * 8)) & 0xFFu);
    }
    return true;
}

bool DataArea::readBytes(std::size_t address, char *out, std::size_t count) const
{
    if (address + count > binary.size()) return false;
    if (count != 0u) std::memcpy(out, binary.data() + address, count);
    return true;
}

bool DataArea::writeBytes(std::size_t address, const char *in, std::size_t count)
{
    if (address + count > binary.size()) return false;
    if (count != 0u) std::memcpy(binary.data() + address, in, count);
    return true;
}

namespace
{

constexpr std::uint8_t kProgramMagic[] = {'P', 'E', 'S', 'V', 'M', 'B', '\r', '\n'};
/** 当前写出的版本。 */
constexpr std::uint16_t kProgramFormatVersion = 4u;
/** 从这一版起 payload 末尾有"导出标签"节。 */
constexpr std::uint16_t kProgramExportsFormatVersion = 3u;
/** 从这一版起 payload 末尾（导出标签之后）还有"调试信息"节。 */
constexpr std::uint16_t kProgramDebugFormatVersion = 4u;
constexpr std::uint16_t kProgramCompactFormatVersion = 2u;
constexpr std::uint16_t kProgramLegacyFormatVersion = 1u;
constexpr std::size_t kProgramHeaderSize = 24u;
constexpr std::size_t kMaximumBinarySize = 512u * 1024u * 1024u;
constexpr std::uint32_t kMaximumItemCount = 10u * 1000u * 1000u;
constexpr std::uint32_t kMaximumStringSize = 64u * 1024u * 1024u;

void binaryDiagnostic(std::vector<std::string> *diagnostics, const std::string &message)
{
    if (diagnostics != nullptr) diagnostics->push_back(message);
}

std::uint32_t binaryChecksum(const std::uint8_t *data, std::size_t size)
{
    std::uint32_t hash = 2166136261u;
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= data[index];
        hash *= 16777619u;
    }
    return hash;
}

struct BinaryWriter
{
    std::vector<std::uint8_t> bytes;
    std::string error;

    void raw(const std::uint8_t *data, std::size_t size)
    {
        bytes.insert(bytes.end(), data, data + size);
    }

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value)
    {
        for (unsigned shift = 0u; shift < 16u; shift += 8u)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }

    void u32(std::uint32_t value)
    {
        for (unsigned shift = 0u; shift < 32u; shift += 8u)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }

    void u64(std::uint64_t value)
    {
        for (unsigned shift = 0u; shift < 64u; shift += 8u)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }

    /** ULEB128：小整数只占一个字节。 */
    void varUInt(std::uint64_t value)
    {
        do
        {
            std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fu);
            value >>= 7u;
            if (value != 0u) byte |= 0x80u;
            u8(byte);
        } while (value != 0u);
    }

    /** ZigZag + ULEB128：正负小整数都紧凑。 */
    void varInt(std::int64_t value)
    {
        const std::uint64_t zigzag = (static_cast<std::uint64_t>(value) << 1u) ^
                                     (0u - static_cast<std::uint64_t>(value < 0));
        varUInt(zigzag);
    }

    void varText(const std::string &value)
    {
        if (value.size() > kMaximumStringSize)
        {
            error = "字符串超过二进制格式的 64 MiB 上限";
            return;
        }
        varUInt(value.size());
        raw(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    }

    void text(const std::string &value)
    {
        if (value.size() > kMaximumStringSize)
        {
            error = "字符串超过二进制格式的 64 MiB 上限";
            return;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    }

    bool count(std::size_t value, const char *what)
    {
        if (value > kMaximumItemCount)
        {
            error = std::string(what) + "超过二进制格式的数量上限";
            return false;
        }
        u32(static_cast<std::uint32_t>(value));
        return true;
    }
};

struct BinaryReader
{
    const std::vector<std::uint8_t> &bytes;
    std::size_t position = 0u;
    std::string error;

    explicit BinaryReader(const std::vector<std::uint8_t> &input) : bytes(input) {}

    bool take(std::size_t size, const std::uint8_t **out)
    {
        if (size > bytes.size() - std::min(position, bytes.size()))
        {
            error = "二进制程序被截断";
            return false;
        }
        *out = bytes.data() + position;
        position += size;
        return true;
    }

    bool u8(std::uint8_t *out)
    {
        const std::uint8_t *data = nullptr;
        if (!take(1u, &data)) return false;
        *out = data[0];
        return true;
    }

    bool u16(std::uint16_t *out)
    {
        const std::uint8_t *data = nullptr;
        if (!take(2u, &data)) return false;
        *out = static_cast<std::uint16_t>(data[0]) |
               static_cast<std::uint16_t>(data[1] << 8u);
        return true;
    }

    bool u32(std::uint32_t *out)
    {
        const std::uint8_t *data = nullptr;
        if (!take(4u, &data)) return false;
        *out = 0u;
        for (unsigned index = 0u; index < 4u; ++index)
            *out |= static_cast<std::uint32_t>(data[index]) << (index * 8u);
        return true;
    }

    bool u64(std::uint64_t *out)
    {
        const std::uint8_t *data = nullptr;
        if (!take(8u, &data)) return false;
        *out = 0u;
        for (unsigned index = 0u; index < 8u; ++index)
            *out |= static_cast<std::uint64_t>(data[index]) << (index * 8u);
        return true;
    }

    bool varUInt(std::uint64_t *out)
    {
        *out = 0u;
        for (unsigned shift = 0u; shift < 64u; shift += 7u)
        {
            std::uint8_t byte = 0u;
            if (!u8(&byte)) return false;
            if (shift == 63u && (byte & 0xfeu) != 0u)
            {
                error = "二进制程序含溢出的变长整数";
                return false;
            }
            *out |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0u) return true;
        }
        error = "二进制程序含过长的变长整数";
        return false;
    }

    bool varInt(std::int64_t *out)
    {
        std::uint64_t zigzag = 0u;
        if (!varUInt(&zigzag)) return false;
        *out = static_cast<std::int64_t>((zigzag >> 1u) ^
                                        (0u - static_cast<std::uint64_t>(zigzag & 1u)));
        return true;
    }

    bool varText(std::string *out)
    {
        std::uint64_t size = 0u;
        if (!varUInt(&size)) return false;
        if (size > kMaximumStringSize)
        {
            error = "二进制程序里的字符串超过 64 MiB 上限";
            return false;
        }
        const std::uint8_t *data = nullptr;
        if (!take(static_cast<std::size_t>(size), &data)) return false;
        out->assign(reinterpret_cast<const char *>(data), static_cast<std::size_t>(size));
        return true;
    }

    bool text(std::string *out)
    {
        std::uint32_t size = 0u;
        if (!u32(&size)) return false;
        if (size > kMaximumStringSize)
        {
            error = "二进制程序里的字符串超过 64 MiB 上限";
            return false;
        }
        const std::uint8_t *data = nullptr;
        if (!take(size, &data)) return false;
        out->assign(reinterpret_cast<const char *>(data), size);
        return true;
    }

    bool count(std::uint32_t *out, const char *what)
    {
        if (!u32(out)) return false;
        if (*out > kMaximumItemCount)
        {
            error = std::string(what) + "超过二进制格式的数量上限";
            return false;
        }
        return true;
    }
};

bool readBinaryFile(const std::string &path, std::vector<std::uint8_t> *bytes,
                    std::vector<std::string> *diagnostics)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        binaryDiagnostic(diagnostics, "打不开二进制程序：" + path);
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > kMaximumBinarySize)
    {
        binaryDiagnostic(diagnostics, "二进制程序过大（上限 512 MiB）：" + path);
        return false;
    }
    stream.seekg(0, std::ios::beg);
    bytes->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.eof() && stream.fail())
    {
        binaryDiagnostic(diagnostics, "读取二进制程序失败：" + path);
        return false;
    }
    return true;
}

} // namespace

bool isProgramBinary(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::uint8_t magic[sizeof(kProgramMagic)] = {};
    stream.read(reinterpret_cast<char *>(magic), static_cast<std::streamsize>(sizeof(magic)));
    return stream.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::equal(std::begin(magic), std::end(magic), std::begin(kProgramMagic));
}

bool saveProgramBinary(const Program &program, const std::string &path,
                       std::vector<std::string> *diagnostics, bool encrypt)
{
    if (!program.ok())
    {
        binaryDiagnostic(diagnostics, "不能序列化带组装诊断的程序");
        return false;
    }
    BinaryWriter payload;
    payload.text(program.name);
    if (!payload.count(program.code.size(), "指令数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (std::size_t instructionIndex = 0u; instructionIndex < program.code.size(); ++instructionIndex)
    {
        const Instruction &instruction = program.code[instructionIndex];
        if (static_cast<std::uint16_t>(instruction.op) >
                static_cast<std::uint16_t>(Op::StoreAt) ||
            static_cast<std::uint8_t>(instruction.format) >
                static_cast<std::uint8_t>(PrintFormat::Text))
        {
            binaryDiagnostic(diagnostics, "程序含无法序列化的指令枚举值");
            return false;
        }
        std::uint64_t doubleBits = 0u;
        std::memcpy(&doubleBits, &instruction.doubleValue, sizeof(doubleBits));
        std::uint8_t operands = 0u;
        if (instruction.format != PrintFormat::Signed) operands |= 1u << 0u;
        if (instruction.unsignedOp || instruction.isFloat) operands |= 1u << 1u;
        if (instruction.intValue != 0) operands |= 1u << 2u;
        if (doubleBits != 0u) operands |= 1u << 3u;
        if (instruction.quadIndex != instructionIndex) operands |= 1u << 4u;
        if (!instruction.name.empty()) operands |= 1u << 5u;
        if (!instruction.name2.empty()) operands |= 1u << 6u;
        if (!instruction.textValue.empty()) operands |= 1u << 7u;

        payload.u8(static_cast<std::uint8_t>(instruction.op));
        payload.u8(operands);
        if ((operands & (1u << 0u)) != 0u)
            payload.u8(static_cast<std::uint8_t>(instruction.format));
        if ((operands & (1u << 1u)) != 0u)
            payload.u8(static_cast<std::uint8_t>((instruction.unsignedOp ? 1u : 0u) |
                                                 (instruction.isFloat ? 2u : 0u)));
        if ((operands & (1u << 2u)) != 0u) payload.varInt(instruction.intValue);
        if ((operands & (1u << 3u)) != 0u) payload.u64(doubleBits);
        if ((operands & (1u << 4u)) != 0u) payload.varUInt(instruction.quadIndex);
        if ((operands & (1u << 5u)) != 0u) payload.varText(instruction.name);
        if ((operands & (1u << 6u)) != 0u) payload.varText(instruction.name2);
        if ((operands & (1u << 7u)) != 0u) payload.varText(instruction.textValue);
    }

    if (!payload.count(program.labels.size(), "标签数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (const auto &[name, index] : program.labels)
    {
        if (index > program.code.size())
        {
            binaryDiagnostic(diagnostics, "程序含越过指令末尾的标签：" + name);
            return false;
        }
        payload.text(name);
        payload.u64(static_cast<std::uint64_t>(index));
    }

    if (!payload.count(program.types.size(), "类型数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (const auto &[variable, type] : program.types)
    {
        payload.text(variable);
        payload.text(type.name);
        payload.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(type.bits)));
        payload.u8(static_cast<std::uint8_t>((type.isUnsigned ? 1u : 0u) |
                                             (type.isFloat ? 2u : 0u)));
        payload.u64(static_cast<std::uint64_t>(type.size));
    }

    if (!payload.count(program.parameters.size(), "形参表数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (const auto &[label, parameters] : program.parameters)
    {
        payload.text(label);
        if (!payload.count(parameters.size(), "形参数"))
        {
            binaryDiagnostic(diagnostics, payload.error);
            return false;
        }
        for (const std::string &parameter : parameters) payload.text(parameter);
    }
    if (!payload.count(program.exports.size(), "导出标签数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (const std::string &label : program.exports)
    {
        if (program.labels.find(label) == program.labels.end())
        {
            binaryDiagnostic(diagnostics, "导出项不是程序标签：" + label);
            return false;
        }
        payload.text(label);
    }
    /* 调试信息节（v4）：指令下标 → 源码位置。没有调试信息也写 0 条，节始终在场。 */
    if (!payload.count(program.debug.entries.size(), "调试信息条数"))
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    for (const auto &entry : program.debug.entries)
    {
        payload.varUInt(static_cast<std::uint64_t>(entry.first));
        payload.varText(entry.second.function);
        payload.varText(entry.second.file);
        payload.varUInt(entry.second.line > 0 ? static_cast<std::uint64_t>(entry.second.line) : 0u);
        payload.varUInt(static_cast<std::uint64_t>(entry.second.quadLine));
    }
    if (!payload.error.empty())
    {
        binaryDiagnostic(diagnostics, payload.error);
        return false;
    }
    /* 可选加密：头部 24 字节保持明文，载荷区换成 [12 字节 nonce][密文]，密文的明文是
     * `8 字节标记 + 原 payload`——标记用来把"密钥不对"和"格式坏了"分开报。
     * 校验和算在**存储形态**上，所以"文件损坏"在解密之前就能判定。 */
    std::uint16_t flags = 0u;
    std::vector<std::uint8_t> stored;
    if (encrypt)
    {
        if (!kPayloadCryptoCompiled)
        {
            binaryDiagnostic(diagnostics,
                             "这个构建没有编进载荷加密（STACKVM_NO_PAYLOAD_CRYPTO）：写不了加密程序");
            return false;
        }
        flags = kProgramFlagEncrypted;
        std::vector<std::uint8_t> plaintext;
        plaintext.insert(plaintext.end(), kPayloadMarker, kPayloadMarker + kPayloadMarkerSize);
        plaintext.insert(plaintext.end(), payload.bytes.begin(), payload.bytes.end());
        std::uint8_t key[32];
        cryptoPayloadKey(key);
        std::uint8_t nonce[kPayloadNonceSize];
        cryptoPayloadNonce(plaintext.data(), plaintext.size(), nonce);
        stored.insert(stored.end(), nonce, nonce + kPayloadNonceSize);
        const std::size_t offset = stored.size();
        stored.insert(stored.end(), plaintext.begin(), plaintext.end());
        cryptoChaCha20Xor(key, nonce, 0u, stored.data() + offset, plaintext.size());
    }
    else
    {
        stored = payload.bytes;
    }
    if (stored.size() > kMaximumBinarySize - kProgramHeaderSize)
    {
        binaryDiagnostic(diagnostics, "二进制程序过大（上限 512 MiB）");
        return false;
    }

    BinaryWriter file;
    file.raw(kProgramMagic, sizeof(kProgramMagic));
    file.u16(kProgramFormatVersion);
    file.u16(flags);
    file.u64(static_cast<std::uint64_t>(stored.size()));
    file.u32(binaryChecksum(stored.data(), stored.size()));
    file.raw(stored.data(), stored.size());

    std::ofstream stream(path, std::ios::binary);
    if (!stream)
    {
        binaryDiagnostic(diagnostics, "写不了二进制程序：" + path);
        return false;
    }
    stream.write(reinterpret_cast<const char *>(file.bytes.data()),
                 static_cast<std::streamsize>(file.bytes.size()));
    if (!stream)
    {
        binaryDiagnostic(diagnostics, "写二进制程序失败：" + path);
        return false;
    }
    return true;
}

bool loadProgramBinary(const std::string &path, Program &program,
                       std::vector<std::string> *diagnostics)
{
    std::vector<std::uint8_t> fileBytes;
    if (!readBinaryFile(path, &fileBytes, diagnostics)) return false;
    if (fileBytes.size() < kProgramHeaderSize ||
        !std::equal(std::begin(kProgramMagic), std::end(kProgramMagic), fileBytes.begin()))
    {
        binaryDiagnostic(diagnostics, "不是 stackvm 二进制程序：" + path);
        return false;
    }

    BinaryReader header{fileBytes};
    const std::uint8_t *ignoredMagic = nullptr;
    std::uint16_t version = 0u;
    std::uint16_t flags = 0u;
    std::uint64_t payloadSize = 0u;
    std::uint32_t checksum = 0u;
    if (!header.take(sizeof(kProgramMagic), &ignoredMagic) || !header.u16(&version) ||
        !header.u16(&flags) || !header.u64(&payloadSize) || !header.u32(&checksum))
    {
        binaryDiagnostic(diagnostics, header.error);
        return false;
    }
    if ((version != kProgramFormatVersion && version != kProgramCompactFormatVersion &&
         version != kProgramLegacyFormatVersion) ||
        (flags & ~static_cast<std::uint16_t>(kProgramFlagEncrypted)) != 0u)
    {
        binaryDiagnostic(diagnostics, "不支持的 stackvm 二进制版本或标志");
        return false;
    }
    if (payloadSize != fileBytes.size() - kProgramHeaderSize)
    {
        binaryDiagnostic(diagnostics, "二进制程序长度字段不匹配");
        return false;
    }
    const std::uint8_t *payloadBegin = fileBytes.data() + kProgramHeaderSize;
    if (binaryChecksum(payloadBegin, static_cast<std::size_t>(payloadSize)) != checksum)
    {
        binaryDiagnostic(diagnostics, "二进制程序校验和不匹配");
        return false;
    }

    const std::uint8_t *payloadEnd = fileBytes.data() + fileBytes.size();
    std::vector<std::uint8_t> decrypted;
    if ((flags & kProgramFlagEncrypted) != 0u)
    {
        if (!kPayloadCryptoCompiled)
        {
            binaryDiagnostic(diagnostics,
                             "这个构建没有编进载荷解密（STACKVM_NO_PAYLOAD_CRYPTO）："
                             "读不了加密的程序");
            return false;
        }
        if (payloadSize < kPayloadNonceSize + kPayloadMarkerSize)
        {
            binaryDiagnostic(diagnostics, "加密载荷短于 nonce + 标记");
            return false;
        }
        decrypted.assign(payloadBegin, payloadEnd);
        std::uint8_t key[32];
        cryptoPayloadKey(key);
        cryptoChaCha20Xor(key, decrypted.data(), 0u, decrypted.data() + kPayloadNonceSize,
                          static_cast<std::size_t>(payloadSize) - kPayloadNonceSize);
        if (std::memcmp(decrypted.data() + kPayloadNonceSize, kPayloadMarker,
                        kPayloadMarkerSize) != 0)
        {
            binaryDiagnostic(diagnostics,
                             "载荷解密失败：密钥与写它的时候不一致（见 STACKVM_PAYLOAD_KEY）");
            return false;
        }
        payloadBegin = decrypted.data() + kPayloadNonceSize + kPayloadMarkerSize;
        payloadEnd = decrypted.data() + decrypted.size();
    }
    std::vector<std::uint8_t> payloadBytes(payloadBegin, payloadEnd);
    BinaryReader reader{payloadBytes};
    Program decoded;
    if (!reader.text(&decoded.name))
    {
        binaryDiagnostic(diagnostics, reader.error);
        return false;
    }

    std::uint32_t count = 0u;
    if (!reader.count(&count, "指令数"))
    {
        binaryDiagnostic(diagnostics, reader.error);
        return false;
    }
    decoded.code.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index)
    {
        Instruction instruction;
        std::uint16_t op = 0u;
        std::uint8_t format = 0u;
        std::uint8_t instructionFlags = 0u;
        std::uint64_t doubleBits = 0u;
        std::uint64_t quadIndex = index;
        std::int64_t intValue = 0;
        if (version == kProgramLegacyFormatVersion)
        {
            std::uint64_t legacyIntBits = 0u;
            if (!reader.u16(&op) || !reader.u8(&format) || !reader.u8(&instructionFlags) ||
                !reader.u64(&legacyIntBits) || !reader.u64(&doubleBits) || !reader.u64(&quadIndex) ||
                !reader.text(&instruction.name) || !reader.text(&instruction.name2) ||
                !reader.text(&instruction.textValue))
            {
                binaryDiagnostic(diagnostics, reader.error);
                return false;
            }
            std::memcpy(&intValue, &legacyIntBits, sizeof(intValue));
        }
        else
        {
            std::uint8_t byteOp = 0u;
            std::uint8_t operands = 0u;
            if (!reader.u8(&byteOp) || !reader.u8(&operands))
            {
                binaryDiagnostic(diagnostics, reader.error);
                return false;
            }
            op = byteOp;
            std::uint64_t varQuadIndex = index;
            if (((operands & (1u << 0u)) != 0u && !reader.u8(&format)) ||
                ((operands & (1u << 1u)) != 0u && !reader.u8(&instructionFlags)) ||
                ((operands & (1u << 2u)) != 0u && !reader.varInt(&intValue)) ||
                ((operands & (1u << 3u)) != 0u && !reader.u64(&doubleBits)) ||
                ((operands & (1u << 4u)) != 0u && !reader.varUInt(&varQuadIndex)) ||
                ((operands & (1u << 5u)) != 0u && !reader.varText(&instruction.name)) ||
                ((operands & (1u << 6u)) != 0u && !reader.varText(&instruction.name2)) ||
                ((operands & (1u << 7u)) != 0u && !reader.varText(&instruction.textValue)))
            {
                binaryDiagnostic(diagnostics, reader.error);
                return false;
            }
            quadIndex = varQuadIndex;
        }
        if (op > static_cast<std::uint16_t>(Op::StoreAt) ||
            format > static_cast<std::uint8_t>(PrintFormat::Text) ||
            (instructionFlags & ~3u) != 0u || quadIndex > kMaximumItemCount)
        {
            binaryDiagnostic(diagnostics, "二进制程序含非法指令字段");
            return false;
        }
        instruction.op = static_cast<Op>(op);
        instruction.format = static_cast<PrintFormat>(format);
        instruction.unsignedOp = (instructionFlags & 1u) != 0u;
        instruction.isFloat = (instructionFlags & 2u) != 0u;
        instruction.intValue = intValue;
        std::memcpy(&instruction.doubleValue, &doubleBits, sizeof(doubleBits));
        instruction.quadIndex = static_cast<std::size_t>(quadIndex);
        decoded.code.push_back(std::move(instruction));
    }

    if (!reader.count(&count, "标签数"))
    {
        binaryDiagnostic(diagnostics, reader.error);
        return false;
    }
    for (std::uint32_t index = 0u; index < count; ++index)
    {
        std::string name;
        std::uint64_t target = 0u;
        if (!reader.text(&name) || !reader.u64(&target) || target > decoded.code.size() ||
            !decoded.labels.emplace(std::move(name), static_cast<std::size_t>(target)).second)
        {
            binaryDiagnostic(diagnostics, reader.error.empty() ? "二进制程序含非法或重复标签"
                                                                : reader.error);
            return false;
        }
    }

    if (!reader.count(&count, "类型数"))
    {
        binaryDiagnostic(diagnostics, reader.error);
        return false;
    }
    for (std::uint32_t index = 0u; index < count; ++index)
    {
        std::string variable;
        TypeInfo type;
        std::uint32_t bits = 0u;
        std::uint8_t typeFlags = 0u;
        std::uint64_t size = 0u;
        if (!reader.text(&variable) || !reader.text(&type.name) || !reader.u32(&bits) ||
            !reader.u8(&typeFlags) || !reader.u64(&size) || (typeFlags & ~3u) != 0u ||
            size > kMaximumBinarySize)
        {
            binaryDiagnostic(diagnostics, reader.error.empty() ? "二进制程序含非法类型字段"
                                                                : reader.error);
            return false;
        }
        type.bits = static_cast<std::int32_t>(bits);
        type.isUnsigned = (typeFlags & 1u) != 0u;
        type.isFloat = (typeFlags & 2u) != 0u;
        type.size = static_cast<std::size_t>(size);
        if (!decoded.types.emplace(std::move(variable), std::move(type)).second)
        {
            binaryDiagnostic(diagnostics, "二进制程序含重复类型项");
            return false;
        }
    }

    if (!reader.count(&count, "形参表数"))
    {
        binaryDiagnostic(diagnostics, reader.error);
        return false;
    }
    for (std::uint32_t index = 0u; index < count; ++index)
    {
        std::string label;
        std::uint32_t parameterCount = 0u;
        if (!reader.text(&label) || !reader.count(&parameterCount, "形参数"))
        {
            binaryDiagnostic(diagnostics, reader.error);
            return false;
        }
        std::vector<std::string> parameters;
        parameters.reserve(parameterCount);
        for (std::uint32_t parameter = 0u; parameter < parameterCount; ++parameter)
        {
            std::string name;
            if (!reader.text(&name))
            {
                binaryDiagnostic(diagnostics, reader.error);
                return false;
            }
            parameters.push_back(std::move(name));
        }
        if (!decoded.parameters.emplace(std::move(label), std::move(parameters)).second)
        {
            binaryDiagnostic(diagnostics, "二进制程序含重复形参表");
            return false;
        }
    }
    if (version >= kProgramExportsFormatVersion)
    {
        if (!reader.count(&count, "导出标签数"))
        {
            binaryDiagnostic(diagnostics, reader.error);
            return false;
        }
        for (std::uint32_t index = 0u; index < count; ++index)
        {
            std::string label;
            if (!reader.text(&label) || decoded.labels.find(label) == decoded.labels.end() ||
                !decoded.exports.insert(std::move(label)).second)
            {
                binaryDiagnostic(diagnostics, reader.error.empty() ? "二进制程序含非法或重复导出项"
                                                                    : reader.error);
                return false;
            }
        }
    }
    else
    {
        for (const auto &[label, ignored] : decoded.labels)
        {
            (void)ignored;
            decoded.exports.insert(label);
        }
    }
    /* 调试信息节（v4 起才有）。旧版本没有这一段，读到时直接跳过。 */
    if (version >= kProgramDebugFormatVersion)
    {
        if (!reader.count(&count, "调试信息条数"))
        {
            binaryDiagnostic(diagnostics, reader.error);
            return false;
        }
        std::size_t previous = 0u;
        bool first = true;
        for (std::uint32_t index = 0u; index < count; ++index)
        {
            std::uint64_t instruction = 0u;
            std::uint64_t line = 0u;
            std::uint64_t quadLine = 0u;
            SourceLocation location;
            if (!reader.varUInt(&instruction) || !reader.varText(&location.function) ||
                !reader.varText(&location.file) || !reader.varUInt(&line) ||
                !reader.varUInt(&quadLine))
            {
                binaryDiagnostic(diagnostics, reader.error);
                return false;
            }
            if (instruction >= decoded.code.size() || (!first && instruction < previous) ||
                line > kMaximumItemCount || quadLine > kMaximumItemCount)
            {
                binaryDiagnostic(diagnostics, "二进制程序含非法调试信息");
                return false;
            }
            location.line = static_cast<std::int32_t>(line);
            location.quadLine = static_cast<std::size_t>(quadLine);
            decoded.debug.entries.emplace_back(static_cast<std::size_t>(instruction),
                                               std::move(location));
            previous = static_cast<std::size_t>(instruction);
            first = false;
        }
    }
    if (reader.position != payloadBytes.size())
    {
        binaryDiagnostic(diagnostics, "二进制程序尾部有未识别数据");
        return false;
    }
    program = std::move(decoded);
    return true;
}

namespace
{

/**
 * @brief 去掉首尾空白。
 * @param text 原文。
 * @return 去空白后的文本。
 */
std::string trim(const std::string &text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1u);
}

/**
 * @brief 去掉首尾引号。
 * @param text 原文。
 * @return 去引号后的文本。
 */
std::string unquote(const std::string &text)
{
    if (text.size() >= 2u && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front())
    {
        return text.substr(1u, text.size() - 2u);
    }
    return text;
}

/**
 * @brief 按逗号切分四元式参数：引号里的逗号不算分隔符。
 * @param body 括号里的内容。
 * @return 参数列表（已去空白）。
 */
std::vector<std::string> splitArguments(const std::string &body)
{
    std::vector<std::string> parts;
    std::string current;
    char quote = '\0';
    for (const char ch : body)
    {
        if (quote != '\0')
        {
            current += ch;
            if (ch == quote) quote = '\0';
            continue;
        }
        if (ch == '"' || ch == '\'')
        {
            quote = ch;
            current += ch;
            continue;
        }
        if (ch == ',')
        {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty() || !parts.empty()) parts.push_back(trim(current));
    return parts;
}

/**
 * @brief 把文本解析成整数 / 浮点。
 * @param text 文本。
 * @param isFloat 输出：是不是浮点。
 * @param intValue 输出：整数。
 * @param doubleValue 输出：浮点。
 * @return 是数字返回 true。
 */
bool parseNumber(const std::string &text, bool &isFloat, std::int64_t &intValue,
                 double &doubleValue)
{
    if (text.empty()) return false;

    // 1. 先按整数试（含负号）。
    std::size_t index = 0;
    if (text[index] == '+' || text[index] == '-') ++index;
    bool digits = false;
    bool isPlainInt = index < text.size();
    for (; index < text.size(); ++index)
    {
        if (text[index] >= '0' && text[index] <= '9') { digits = true; continue; }
        isPlainInt = false;
        break;
    }
    if (isPlainInt && digits)
    {
        char *end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (end != nullptr && *end == '\0')
        {
            isFloat = false;
            intValue = static_cast<std::int64_t>(value);
            return true;
        }
    }

    // 2. 再按浮点试。
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if ((end != nullptr) && (*end == '\0') && (end != text.c_str()))
    {
        isFloat = true;
        doubleValue = value;
        intValue = static_cast<std::int64_t>(value);
        return true;
    }
    return false;
}

/**
 * @brief 最短往返的浮点文本（与 library/json 同一套路）。
 * @param value 浮点。
 * @return 文本。
 */
std::string formatDouble(double value)
{
    char buffer[64];
    std::string text;
    for (int precision = 17; precision >= 1; --precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        text = buffer;
        char *end = nullptr;
        if (std::strtod(text.c_str(), &end) == value && end != nullptr && *end == '\0') break;
    }
    return text;
}

/**
 * @brief 浮点**字面量**的四元式文本：一定带小数点（`2` → `2.0`）。
 *
 * 四元式是文本形态，后端按"有没有小数点"区分整数槽位与浮点槽位；折叠出来的
 * 浮点常量必须写成 `2.0`，否则 `(:=, 2, _, t)` 之后按浮点用时会被当成整数。
 * @param value 浮点。
 * @return 文本。
 */
std::string formatFloatLiteral(double value)
{
    std::string text = formatDouble(value);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

/**
 * @brief 按位宽回绕并做符号扩展。
 * @param value 原值。
 * @param type 类型。
 * @return 归一化后的值。
 */
std::int64_t normalizeInt(std::int64_t value, const TypeInfo &type)
{
    if (type.bits <= 0 || type.bits >= 64) return value;
    const std::int64_t mask = (static_cast<std::int64_t>(1) << type.bits) - 1;
    value &= mask;
    if (!type.isUnsigned && (value & (static_cast<std::int64_t>(1) << (type.bits - 1))))
    {
        value -= (static_cast<std::int64_t>(1) << type.bits);
    }
    return value;
}

/** f32 在每次带类型的运算或转换后按 IEEE-754 单精度舍入，f64 保持 double。 */
double normalizeFloat(double value, int bits)
{
    return bits == 32 ? static_cast<double>(static_cast<float>(value)) : value;
}

/** 栈里的一个值。 */
struct Slot
{
    bool isFloat = false;   ///< 是否浮点。
    bool isText = false;    ///< 是否字符串。
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;

    /**
     * @brief 造一个整数。
     * @param value 值。
     * @return 槽位。
     */
    static Slot fromInt(std::int64_t value)
    {
        Slot slot;
        slot.integer = value;
        return slot;
    }

    /**
     * @brief 造一个浮点。
     * @param value 值。
     * @return 槽位。
     */
    static Slot fromDouble(double value)
    {
        Slot slot;
        slot.isFloat = true;
        slot.real = value;
        return slot;
    }

    /**
     * @brief 造一个字符串。
     * @param value 文本。
     * @return 槽位。
     */
    static Slot fromText(std::string value)
    {
        Slot slot;
        slot.isText = true;
        slot.text = std::move(value);
        return slot;
    }

    /** 取出数值（整数或浮点都行）。 */
    [[nodiscard]] double number() const { return isFloat ? real : static_cast<double>(integer); }
};

/**
 * @brief 虚拟机自己的堆：一块连续的字节区 + 空闲块表。
 *
 * **为什么不放在数据区里**：数据区（`.data.bin`）是编译期定好符号与槽位的静态内存，
 * 程序跑起来只有那一片；而 `new` / `malloc` 要多少字节是运行期才知道的，也不该跟
 * 静态数据抢地址。所以堆单独长，只和数据区**共用一套地址空间**：地址从 `base()`
 * 起算（紧跟在数据区后面），`(load, 地址, …)` / `(store, 地址, …)` 两边通用 ——
 * 对象既不在数据区里，也不在求值栈 / 调用帧里，什么时候还回来由程序自己 `free`。
 *
 * 分配策略：**按 1 字节单元** + 首次适配 + 相邻空闲块
 * 合并；块表不够就整块往后长。地址即"字节区偏移 + base"，所以 `free` 只认地址；
 * 读到 / 写到没分配（或已经 free 过）的堆地址会报错，而不是悄悄读到脏数据。
 */
class Heap final : public HeapAllocator
{
public:
    /**
     * @brief 一块内存。
     */
    struct Block
    {
        std::size_t address = 0; ///< 起始地址（已含 base）。
        std::size_t size = 0;    ///< 字节数。
        bool free = true;        ///< 是否空闲。
    };

    /** 默认构造后由 Machine 在每次 run() 前 reset。 */
    Heap() = default;

    /** 清空上一轮状态并设置堆基址；地址 0 保留为空指针。 */
    void reset(std::size_t base) override
    {
        base_ = base < kRuntimeCellBytes ? kRuntimeCellBytes : base;
        bytes_.clear();
        blocks_.clear();
        peak_ = 0u;
    }

    /** 堆区起始地址。 */
    [[nodiscard]] std::size_t base() const override { return base_; }

    /**
     * @brief 分配一块字节寻址的内存。
     * @param bytes 字节数。
     * @return 地址；参数为 0 或没内存了返回 0。
     */
    std::size_t allocate(std::size_t bytes) override
    {
        if (bytes == 0u) return 0u;
        const std::size_t need = bytes;

        // 已释放块保留为墓碑，直到下一次 reset / 本次 run 结束。
        // 只向后分配，旧指针就不会因地址复用而指向另一个对象。
        Block block;
        if (bytes_.size() > std::numeric_limits<std::size_t>::max() - base_ ||
            need > std::numeric_limits<std::size_t>::max() - base_ - bytes_.size() ||
            need > bytes_.max_size() - bytes_.size()) return 0u;
        block.address = base_ + bytes_.size();
        block.size = need;
        block.free = false;
        try { bytes_.resize(bytes_.size() + need, 0u); }
        catch (const std::bad_alloc &) { return 0u; }
        catch (const std::length_error &) { return 0u; }
        try { blocks_.push_back(block); }
        catch (const std::bad_alloc &) { bytes_.resize(bytes_.size() - need); return 0u; }
        catch (const std::length_error &) { bytes_.resize(bytes_.size() - need); return 0u; }
        refreshPeak();
        return block.address;
    }

    /**
     * @brief 释放一块内存（地址 0 当空指针放过；相邻空闲块合并）。
     * @param address 地址。
     * @param reason 失败原因（可选）。
     * @return 成功（或空指针）返回 true。
     */
    bool release(std::size_t address, std::string *reason = nullptr) override
    {
        if (address == 0u) return true;   // free(NULL) 是空操作
        for (std::size_t index = 0; index < blocks_.size(); ++index)
        {
            Block &block = blocks_[index];
            if (block.address != address) continue;
            if (block.free)
            {
                if (reason != nullptr) *reason = "这块内存已经 free 过了";
                return false;
            }
            block.free = true;
            return true;
        }
        if (reason != nullptr)
        {
            *reason = "地址 " + std::to_string(address) + " 不在堆里（堆区从 " +
                      std::to_string(base_) + " 起）";
        }
        return false;
    }

    /** 地址是否落在堆区（不管那块有没有被 free）。 */
    [[nodiscard]] bool contains(std::size_t address) const override
    {
        return address >= base_ && address - base_ < bytes_.size();
    }

    /**
     * @brief 地址所在的**已分配块**从这里起还能用多少字节。
     * @param address 地址。
     * @param capacity 输出：从 address 到块尾的字节数。
     * @return 地址不在任何已分配块里（没分配 / 已经 free）返回 false。
     */
    bool capacityAt(std::size_t address, std::size_t &capacity) const override
    {
        for (const Block &block : blocks_)
        {
            if (block.free) continue;
            if (address >= block.address && address - block.address < block.size)
            {
                capacity = block.size - (address - block.address);
                return true;
            }
        }
        return false;
    }

    /** 当前在用的字节数。 */
    [[nodiscard]] std::size_t liveBytes() const override
    {
        std::size_t total = 0;
        for (const Block &block : blocks_) if (!block.free) total += block.size;
        return total;
    }

    /** 还在用着的块数。 */
    [[nodiscard]] std::size_t liveBlocks() const override
    {
        std::size_t count = 0;
        for (const Block &block : blocks_) if (!block.free) ++count;
        return count;
    }

    /** 同时在用的字节数峰值。 */
    [[nodiscard]] std::size_t peakBytes() const override { return peak_; }

    /**
     * @brief 读 8 字节（只允许读**已分配**的块）。
     * @param address 地址。
     * @param out 结果。
     * @return 越界 / 已释放返回 false。
     */
    bool readInt(std::size_t address, std::int64_t &out) const
    {
        if (!usable(address)) return false;
        std::memcpy(&out, bytes_.data() + (address - base_), sizeof(out));
        return true;
    }

    /**
     * @brief 写 8 字节。
     * @param address 地址。
     * @param value 值。
     * @return 越界 / 已释放返回 false。
     */
    bool writeInt(std::size_t address, std::int64_t value)
    {
        if (!usable(address)) return false;
        std::memcpy(bytes_.data() + (address - base_), &value, sizeof(value));
        return true;
    }

    /**
     * @brief 读 8 字节，按 IEEE-754 双精度解释。
     * @param address 地址。
     * @param out 结果。
     * @return 越界 / 已释放返回 false。
     */
    bool readDouble(std::size_t address, double &out) const
    {
        if (!usable(address)) return false;
        std::memcpy(&out, bytes_.data() + (address - base_), sizeof(out));
        return true;
    }

    /**
     * @brief 把双精度写成 8 字节。
     * @param address 地址。
     * @param value 值。
     * @return 越界 / 已释放返回 false。
     */
    bool writeDouble(std::size_t address, double value)
    {
        if (!usable(address)) return false;
        std::memcpy(bytes_.data() + (address - base_), &value, sizeof(value));
        return true;
    }

    /**
     * @brief 按字节读一段（字符串跨 8 字节槽，不能按槽读）。
     * @param address 地址。
     * @param out 输出缓冲（至少 count 字节）。
     * @param count 字节数。
     * @return 这 count 个字节全在**同一块已分配内存**里返回 true。
     */
    bool readBytes(std::size_t address, char *out, std::size_t count) const override
    {
        if (count == 0u) return true;
        if (!usableRange(address, count)) return false;
        std::memcpy(out, bytes_.data() + (address - base_), count);
        return true;
    }

    /**
     * @brief 按字节写一段。
     * @param address 地址。
     * @param in 输入缓冲（至少 count 字节）。
     * @param count 字节数。
     * @return 这 count 个字节全在**同一块已分配内存**里返回 true。
     */
    bool writeBytes(std::size_t address, const char *in, std::size_t count) override
    {
        if (count == 0u) return true;
        if (!usableRange(address, count)) return false;
        std::memcpy(bytes_.data() + (address - base_), in, count);
        return true;
    }

private:
    /** [address, address+count) 是否整段落在某个**已分配**块里。 */
    [[nodiscard]] bool usableRange(std::size_t address, std::size_t count) const
    {
        if (!contains(address) || count > bytes_.size() - (address - base_)) return false;
        for (const Block &block : blocks_)
        {
            if (block.free) continue;
            if (address >= block.address && address - block.address < block.size &&
                count <= block.size - (address - block.address))
            {
                return true;
            }
        }
        return false;
    }

    /** 地址是否可读写：落在某个**已分配**块里，且 8 个字节都在块内。 */
    [[nodiscard]] bool usable(std::size_t address) const { return usableRange(address, 8u); }

    /** 更新峰值。 */
    void refreshPeak()
    {
        const std::size_t live = liveBytes();
        if (live > peak_) peak_ = live;
    }

    std::size_t base_ = 8u;             ///< 堆区起始地址。
    std::vector<std::uint8_t> bytes_;   ///< 字节区（从 base_ 开始）。
    std::vector<Block> blocks_;         ///< 块表，按地址升序。
    std::size_t peak_ = 0u;             ///< 峰值。
};

/**
 * @brief 调用帧：一层调用的局部变量存储 + 返回信息。
 *
 * 每进一层函数就在**堆上**单独开一帧（`Op::Call` 里 `std::make_unique<Frame>()`，
 * 对应 C 的 `malloc`），函数返回时随帧的所有权一起释放（`Op::Ret`，对应 `free`）——
 * 所以递归的每一层都有自己的一片 `locals`，同一份代码的两次调用互不覆盖。
 *
 * 这里用 `std::unique_ptr` 持有而不是裸 `new` / `delete`：分配点、释放点还是
 * "进函数 / 出函数"这两处，但所有权跟着帧走，任何一个返回分支都不会漏掉释放，
 * 也不需要手工配对（这就是"C++ 里跟 malloc/free 等价的那套机制"）。
 */
struct Frame
{
    std::map<std::string, Slot> locals; ///< 本帧的局部变量（形参 + 函数体里声明的）。
    std::size_t returnIp = 0;           ///< 返回地址。
    std::string returnDest;             ///< 返回值写回调用方的哪个变量（可空）。
};

/** 待绑定的实参。 */
struct PendingArg
{
    std::string name; ///< 形参名。
    Slot value;       ///< 值。
};

/** 一个可暂停、可恢复的 VM 执行上下文。全局区、堆、数据区和对象表由所有协程共享。 */
struct CoroutineContext
{
    enum class State { Running, Ready, Suspended, Waiting, Receiving, Done };

    std::vector<Slot> stack;
    std::vector<std::unique_ptr<Frame>> frames;
    std::vector<PendingArg> pending;
    std::size_t pc = 0;
    State state = State::Ready;
    std::size_t waitingFor = 0;
    std::size_t parent = 0;
    std::vector<std::string> inbox;
};

/* ---------------------------------------------------------------------------
 *  内部 C++ 运行时：字符串 / 格式化
 * ---------------------------------------------------------------------------
 *
 * 运行时字符串是“8 字节长度 + UTF-8 字节数据”，不附加结尾 NUL；数据区字面量只是
 * 复制源，仍用生成期哨兵界定。**读字节 / 拼接 / 切片 / 格式化这些活儿全在这层 C++ 里干**，宿主语言
 * （magel）只发一次调用 —— 不用在四元式里手写逐字节的循环，也就能顺手用上
 * `std::string` / `snprintf`。
 *
 * 与数学原生函数（`kNatives`）的两点不同：
 * 1. 参数与结果都是 **Slot**：地址 / 长度必须落在**整数槽**里（`(load, 地址, …)`
 *    认的是 `Slot.integer`，浮点槽的 `integer` 是 0，塞进去就成空指针了）；
 * 2. 要能用堆（造新串）与数据区（读字面量），还要能报错 —— 所以带一个 NativeContext。
 */

using NativeInvoker = std::function<bool(std::int64_t, const std::vector<NativeValue> &,
                                         NativeValue &)>;

/** 原生函数跑起来时的环境。 */
struct NativeContext
{
    HeapAllocator *heap = nullptr;   ///< 堆：造出来的字符串放这儿。
    const DataArea *data = nullptr;  ///< 数据区：字面量所在的那片静态内存。
    std::string *text = nullptr;     ///< 文本输出桶（`printf` 往里追加）。
    const std::vector<std::string> *input = nullptr;  ///< 输入项（按顺序取）。
    std::size_t *inputIndex = nullptr;                ///< 已经取到第几项。
    const std::vector<std::string> *programArguments = nullptr; ///< 程序入口参数。
    NativeInvoker *invoker = nullptr; ///< 同步调用 MAGEL 函数指针。
    struct ObjectStore *objects = nullptr;            ///< 运行期对象表（bint / 容器）。
    int *bigFloatPrecision = nullptr;                  ///< bfloat 除法的有效位数（本次 run 独立）。
    bool *exitRequested = nullptr;      ///< `exit(n)` 请求提前正常停机。
    int *exitCode = nullptr;            ///< `exit(n)` 提供的宿主状态码。
    FileApi *fileApi = nullptr;         ///< 文件系统适配器。
    std::string error;               ///< 非空 = 出错，调用方负责终止运行。
};

/** 扫数据区字面量时最多看多少字节（运行时堆字符串直接读取长度）。 */
constexpr std::size_t kStringScanLimit = 1u << 20;

/**
 * @brief 从堆或数据区里读一段字节。
 * @param ctx 环境。
 * @param address 地址。
 * @param out 输出缓冲。
 * @param count 字节数。
 * @return 读得到返回 true（已释放 / 越界返回 false）。
 */
bool nativeRead(const NativeContext &ctx, std::size_t address, char *out, std::size_t count)
{
    if (ctx.heap != nullptr && ctx.heap->contains(address))
    {
        return ctx.heap->readBytes(address, out, count);
    }
    return (ctx.data != nullptr) && ctx.data->readBytes(address, out, count);
}

/**
 * @brief 往堆或数据区按字节写一段（改字符串里的某个字节用它）。
 * @param ctx 环境。
 * @param address 地址。
 * @param in 输入缓冲。
 * @param count 字节数。
 * @return 成功返回 true。
 */
bool nativeWrite(NativeContext &ctx, std::size_t address, const char *in, std::size_t count)
{
    if (ctx.heap != nullptr && ctx.heap->contains(address))
    {
        return ctx.heap->writeBytes(address, in, count);
    }
    return (ctx.data != nullptr) && const_cast<DataArea *>(ctx.data)->writeBytes(address, in, count);
}

/**
 * @brief 读取显式长度的堆字符串，或读取生成期带哨兵的数据区字面量。
 * @param ctx 环境。
 * @param address 地址（0 = 空指针）。
 * @param out 结果。
 * @return 成功返回 true；长度非法、地址不可读或数据区哨兵缺失返回 false。
 */
bool nativeReadCString(const NativeContext &ctx, std::size_t address, std::string *out)
{
    out->clear();
    if (address == 0u) return true;   ///< 空指针当空串
    if (ctx.heap != nullptr && ctx.heap->contains(address))
    {
        std::int64_t length = 0;
        std::size_t capacity = 0u;
        if (!ctx.heap->readInt(address, length) || length < 0 ||
            !ctx.heap->capacityAt(address, capacity) ||
            static_cast<std::uint64_t>(length) > capacity - std::min<std::size_t>(capacity, 8u))
            return false;
        out->resize(static_cast<std::size_t>(length));
        return length == 0 || ctx.heap->readBytes(address + 8u, out->data(), out->size());
    }
    for (std::size_t index = 0; index < kStringScanLimit; ++index)
    {
        char ch = 0;
        if (!nativeRead(ctx, address + index, &ch, 1u)) return false;
        if (ch == '\0') return true;
        out->push_back(ch);
    }
    return false;
}

/**
 * @brief 造一个显式长度的堆字符串，返回长度字段的地址。
 * @param ctx 环境。
 * @param text UTF-8 字节内容。
 * @return 地址；0 = 失败（`ctx.error` 里是原因）。
 */
std::size_t nativeAllocString(NativeContext &ctx, const std::string &text)
{
    if (ctx.heap == nullptr) return 0u;
    const std::size_t bytes = text.size() + 8u;
    const std::size_t address = ctx.heap->allocate(bytes);
    if (address == 0u)
    {
        ctx.error = "堆内存不足：拼接 / 格式化需要 " + std::to_string(bytes) + " 字节";
        return 0u;
    }
    if (!ctx.heap->writeInt(address, static_cast<std::int64_t>(text.size())) ||
        (!text.empty() && !ctx.heap->writeBytes(address + 8u, text.data(), text.size())))
    {
        ctx.error = "往堆上写字符串失败（地址 " + std::to_string(address) + "）";
        return 0u;
    }
    return address;
}

/** 把私有运行时环境收窄成公开胶水接口。 */
class NativeCallContextImpl final : public NativeCallContext
{
public:
    using Invoker = NativeInvoker;

    explicit NativeCallContextImpl(NativeContext &context, Invoker invoker = {})
        : context_(context), invoker_(std::move(invoker)) {}

    bool readBytes(std::int64_t address, char *out, std::size_t count) const override
    {
        return address >= 0 && nativeRead(context_, static_cast<std::size_t>(address), out, count);
    }

    bool writeBytes(std::int64_t address, const char *data, std::size_t count) override
    {
        return address >= 0 && nativeWrite(context_, static_cast<std::size_t>(address), data, count);
    }

    bool readString(std::int64_t address, std::string &out) const override
    {
        return address >= 0 && nativeReadCString(context_, static_cast<std::size_t>(address), &out);
    }

    bool readList(std::int64_t handle, std::vector<NativeValue> &out) const override;

    bool invoke(std::int64_t functionPointer, const std::vector<NativeValue> &arguments,
                NativeValue &result) override
    {
        if (invoker_ && invoker_(functionPointer, arguments, result)) return true;
        if (context_.error.empty()) context_.error = "当前 VM 上下文不支持调用函数指针";
        return false;
    }

    std::int64_t makeString(const std::string &text) override
    {
        return static_cast<std::int64_t>(nativeAllocString(context_, text));
    }

    std::int64_t makeTemporary(std::size_t bytes) override;

    void fail(const std::string &message) override { context_.error = message; }

private:
    NativeContext &context_;
    Invoker invoker_;
};

/** 取一个实参的整数值（整数槽直接取，浮点槽取整）。 */
std::int64_t nativeInt(const Slot &slot)
{
    return slot.isFloat ? static_cast<std::int64_t>(slot.real) : slot.integer;
}

/**
 * @brief 整数 → 十进制文本（`int` 是 64 位，所以按 `long long` 打）。
 * @param value 值。
 * @return 文本。
 */
std::string nativeIntText(std::int64_t value) { return std::to_string(value); }

/**
 * @brief 浮点 → 文本：默认保留 3 位小数（语言的默认），去掉尾随的 0。
 *
 * 与 `%f` 的默认精度保持一致：`3.5` 打成 `3.5`（不是 `3.500`），`2.0` 打成 `2.0`。
 * @param value 值。
 * @param precision 小数位数。
 * @return 文本。
 */
std::string nativeFloatText(double value, int precision)
{
    if (precision < 0) precision = 3;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    std::string text = buffer;
    if (precision > 0 && text.find('.') != std::string::npos)
    {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text;
}

/**
 * @brief 按格式串把实参拼成文本。
 *
 * 这是 `printf` 与 `format` **共用的一份实现**（语言侧不写格式化逻辑）。占位符：
 *
 * | 写法 | 说明 |
 * | --- | --- |
 * | `%d` / `%i` | 有符号整数 |
 * | `%u` | 无符号整数 | `%x` / `%X` | 十六进制（小写 / 大写） |
 * | `%f` | 浮点（**默认 3 位小数**，`.N` 给定就按 N） |
 * | `%s` | 字符串（`.N` = 最多取 N 个字符） |
 * | `%c` | 一个字符（整数按低 8 位当字符码） |
 * | `%%` | 一个百分号 |
 *
 * 每个占位符前面可以写旗标 / 宽度 / 精度：`%-8d`（左对齐）、`%08.2f`（补零）、
 * `%.3s`（截断）。数字部分直接交给 `snprintf`，所以语义就是 C 的那一套。
 *
 * @param ctx 环境。
 * @param format 格式串（已经过语言的转义处理）。
 * @param args 实参（下标 0 起）。
 * @param argCount 实参个数。
 * @param out 拼出来的文本。
 * @return 成功返回 true；`ctx.error` 里是出错原因。
 */
bool nativeFormatInto(NativeContext &ctx, const std::string &format, const std::vector<Slot> &args,
                      std::size_t argCount, std::string *out)
{
    out->clear();
    /* `args` 里**头一个**是格式串自己（原生函数的第一个实参），真正的实参从
     * `args.size() - argCount` 开始 —— 之前从这里写错成 0，`%s` 就把格式串自己当成了
     * 第一个实参（打出来的东西自然全是错位的）。 */
    std::size_t next = args.size() - argCount;
    for (std::size_t index = 0; index < format.size(); ++index)
    {
        if (format[index] != '%')
        {
            out->push_back(format[index]);
            continue;
        }
        if (index + 1u >= format.size())
        {
            ctx.error = "格式串以 % 结尾（要打百分号请写 `%%`）";
            return false;
        }
        const char spec = format[++index];
        if (spec == '%')
        {
            out->push_back('%');
            continue;
        }

        /* 1. 收集旗标 / 宽度 / 精度（与 C 的 printf 同形）。 */
        std::string flags;
        std::string width;
        std::string precision;
        for (; index < format.size(); ++index)
        {
            const char ch = format[index];
            if (ch == '-' || ch == '+' || ch == ' ' || ch == '#') flags.push_back(ch);
            else if (ch == '0' && width.empty()) width.push_back(ch);
            else break;
        }
        for (; index < format.size() && std::isdigit(static_cast<unsigned char>(format[index]));
             ++index)
        {
            width.push_back(format[index]);
        }
        if (index < format.size() && format[index] == '.')
        {
            precision.push_back('.');
            ++index;
            while (index < format.size() && std::isdigit(static_cast<unsigned char>(format[index])))
            {
                precision.push_back(format[index]);
                ++index;
            }
        }
        if (index >= format.size())
        {
            ctx.error = "格式串里 `%` 后面缺转换字符（`%d` / `%f` / `%s` 这种）";
            return false;
        }
        const char conversion = format[index];
        const bool isString = (conversion == 's');
        const bool isChar = (conversion == 'c');
        if (next >= args.size())   /* 真正的实参在 `args` 里只到末尾 */
        {
            ctx.error = std::string("格式串里的占位符比实参多（%") + conversion + " 没东西可填)";
            return false;
        }
        const Slot &argument = args[next++];

        /* 2. `%s` / `%c` 不走 snprintf：字符串要先把地址读成文本。 */
        if (isString)
        {
            std::string text;
            if (argument.isText) text = argument.text;
            else if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(argument)), &text))
            {
                ctx.error = "格式化 %s 时读不到字符串（地址 " +
                            std::to_string(nativeInt(argument)) + " 不在任何一块已分配内存里）";
                return false;
            }
            /* 精度：最多取前 N 个字符（C 的 `%.3s` 就是这个意思）。 */
            if (precision.size() > 1u)
            {
                const std::size_t limit = static_cast<std::size_t>(
                    std::strtoull(precision.c_str() + 1, nullptr, 10));
                if (text.size() > limit) text.resize(limit);
            }
            const std::size_t pad = (width.empty() || text.size() >= std::strtoull(width.c_str(),
                                                                                  nullptr, 10))
                                        ? 0u
                                        : std::strtoull(width.c_str(), nullptr, 10) - text.size();
            if (pad != 0u && flags.find('-') == std::string::npos) out->append(pad, ' ');
            out->append(text);
            if (pad != 0u && flags.find('-') != std::string::npos) out->append(pad, ' ');
            continue;
        }
        if (isChar)
        {
            const char ch = static_cast<char>(nativeInt(argument) & 0xFF);
            out->push_back(ch);
            continue;
        }

        /* `%b` 不是 C++17 snprintf 的可移植转换，按 uint64 原始位模式输出。 */
        if (conversion == 'b')
        {
            std::uint64_t bits = static_cast<std::uint64_t>(nativeInt(argument));
            std::string digits;
            do { digits.insert(digits.begin(), (bits & 1u) != 0u ? '1' : '0'); bits >>= 1u; }
            while (bits != 0u);
            if (precision.size() > 1u)
            {
                const std::size_t minimum = std::min<std::size_t>(
                    std::strtoull(precision.c_str() + 1, nullptr, 10), 10000u);
                if (digits.size() < minimum) digits.insert(0u, minimum - digits.size(), '0');
            }
            const std::string prefix = flags.find('#') != std::string::npos ? "0b" : "";
            const std::size_t requested = std::min<std::size_t>(
                width.empty() ? 0u : std::strtoull(width.c_str(), nullptr, 10), 10000u);
            const std::size_t pad = requested > prefix.size() + digits.size()
                                        ? requested - prefix.size() - digits.size() : 0u;
            if (flags.find('-') != std::string::npos)
            {
                *out += prefix + digits;
                out->append(pad, ' ');
            }
            else if (width.find('0') == 0u)
            {
                *out += prefix;
                out->append(pad, '0');
                *out += digits;
            }
            else
            {
                out->append(pad, ' ');
                *out += prefix + digits;
            }
            continue;
        }

        /* 3. 其余数字：旗标 / 宽度 / 精度交给 snprintf。 */
        const bool isFloatConversion =
            (conversion == 'f' || conversion == 'F' || conversion == 'e' || conversion == 'E' ||
             conversion == 'g' || conversion == 'G');
        std::string spec2 = "%";
        spec2 += flags;
        spec2 += width;
        if (!precision.empty()) spec2 += precision;
        else if (isFloatConversion) spec2 += ".3";   /* 语言的默认：三位小数 */
        /* 整数一律按 64 位传（`long long` / `unsigned long long`）：`%d` 只吃 32 位，
         * 不补 `ll` 的话 `%d` 拿到 2^32 以上的值就会被截成低 32 位。 */
        if (!isFloatConversion) spec2 += "ll";
        spec2.push_back(conversion);

        char buffer[512];
        int written = 0;
        if (isFloatConversion)
        {
            const double value = argument.isFloat ? argument.real
                                                  : static_cast<double>(argument.integer);
            written = std::snprintf(buffer, sizeof(buffer), spec2.c_str(), value);
        }
        else if (conversion == 'x' || conversion == 'X' || conversion == 'u')
        {
            written = std::snprintf(buffer, sizeof(buffer), spec2.c_str(),
                                    static_cast<unsigned long long>(nativeInt(argument)));
        }
        else
        {
            written = std::snprintf(buffer, sizeof(buffer), spec2.c_str(),
                                    static_cast<long long>(nativeInt(argument)));
        }
        if (written < 0)
        {
            ctx.error = "格式化失败（转换 `%" + std::string(1, conversion) + "`）";
            return false;
        }
        out->append(buffer, std::min<std::size_t>(static_cast<std::size_t>(written),
                                                  sizeof(buffer) - 1u));
    }
    if (next != args.size())
    {
        ctx.error = "格式化时实参比格式串里的占位符多（多给了 " +
                    std::to_string(args.size() - next) + " 个）";
        return false;
    }
    return true;
}

/** 一个"内部 C++"原生函数（字符串 / 格式化那一套）。 */
struct SlotNative
{
    /**
     * 参数个数；**负数 = 变参**（吃光这次攒下的所有实参：`printf` / `format` 用它，
     * 第一个实参是格式串，其余按顺序对应占位符）。
     */
    int arity = 0;
    /// 实现。
    Slot (*apply)(NativeContext &, const std::vector<Slot> &) = nullptr;
};

/** `__vm_str_lit(地址) -> 堆上的新串地址`：把只读的字面量字节复制成一份**可写**的堆串。
 *
 * 字符串的"值"永远是堆地址（`\0` 结尾的字节串），数据区里只留只读的字面量字节当
 * 复制源 —— 于是每个 `string` 变量手里那块是自己能写的，`s[i] = 字符` 不会改到
 * 共享的字面量；所有权也就统一了：**变量即那块堆串的主人**。
 */
Slot strLit(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text))
    {
        ctx.error = "复制字符串字面量时读不到它（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, text)));
}

/** `__vm_str_len(地址) -> 长度`。 */
Slot strLen(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::size_t address = static_cast<std::size_t>(nativeInt(args[0]));
    std::int64_t length = 0;
    if (ctx.heap != nullptr && ctx.heap->contains(address) && ctx.heap->readInt(address, length) &&
        length >= 0)
        return Slot::fromInt(length);
    std::string text;
    if (!nativeReadCString(ctx, address, &text))
    {
        ctx.error = "取字符串长度时读不到它（地址 " + std::to_string(nativeInt(args[0])) +
                    " 不在任何一块已分配内存里，或者没有结尾的 \\0）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<std::int64_t>(text.size()));
}

/**
 * `__vm_str_join(串…) -> 新串地址`：把**一串**字符串接成一个（变参）。
 *
 * 之所以一次接一整条链（而不是两个两个接）：`a + b + c` 那样两两接会留下一个中间串，
 * 而中间串没有哪个变量"拥有"它 —— 没人释放（漏）。摊平成一次调用就只分配一个结果，
 * 它正好归被赋值的那个变量所有。
 */
Slot strJoin(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    for (const Slot &argument : args)
    {
        std::string part;
        if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(argument)), &part))
        {
            ctx.error = "拼接字符串时读不到操作数（地址 " +
                        std::to_string(nativeInt(argument)) + "）";
            return Slot::fromInt(0);
        }
        text += part;
    }
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, text)));
}

/**
 * `__vm_str_sub(串, 起, 止) -> 新串地址`：半开区间 `[起, 止)`，**越界自动夹到范围内**；
 * `止` 传负数表示"到末尾"。
 */
Slot strSub(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text))
    {
        ctx.error = "切片时读不到字符串（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(0);
    }
    const std::int64_t length = static_cast<std::int64_t>(text.size());
    std::int64_t begin = nativeInt(args[1]);
    std::int64_t end = nativeInt(args[2]);
    if (end < 0) end = length;
    if (begin < 0) begin = 0;
    if (begin > length) begin = length;
    if (end < begin) end = begin;
    if (end > length) end = length;
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(
        ctx, text.substr(static_cast<std::size_t>(begin),
                         static_cast<std::size_t>(end - begin)))));
}

/** `__vm_str_of_int(值) -> 新串地址`。 */
Slot strOfInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, nativeIntText(nativeInt(args[0])))));
}

/** `__vm_str_of_flt(值) -> 新串地址`（默认 3 位小数，`精度` 可覆盖）。 */
Slot strOfFlt(NativeContext &ctx, const std::vector<Slot> &args)
{
    const double value = args[0].isFloat ? args[0].real : static_cast<double>(args[0].integer);
    const int precision = (args.size() > 1u) ? static_cast<int>(nativeInt(args[1])) : 3;
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, nativeFloatText(value, precision))));
}

/** `__vm_str_fmt(格式串, 实参…) -> 新串地址`。 */
Slot strFormat(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (args.empty())
    {
        ctx.error = "format 至少要有格式串";
        return Slot::fromInt(0);
    }
    std::string format;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &format))
    {
        ctx.error = "format 的格式串读不到（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(0);
    }
    std::string text;
    if (!nativeFormatInto(ctx, format, args, args.size() - 1u, &text)) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, text)));
}

/** `__vm_print(格式串, 实参…) -> 0`：把格式化结果追加到文本输出桶（`printf` 用它）。 */
Slot strPrint(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (args.empty())
    {
        ctx.error = "printf 至少要有格式串";
        return Slot::fromInt(0);
    }
    std::string format;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &format))
    {
        ctx.error = "printf 的格式串读不到（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(0);
    }
    std::string text;
    if (!nativeFormatInto(ctx, format, args, args.size() - 1u, &text)) return Slot::fromInt(0);
    if (ctx.text != nullptr) *ctx.text += text;
    return Slot::fromInt(0);
}

Slot putChar(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (ctx.text != nullptr) ctx.text->push_back(static_cast<char>(nativeInt(args[0]) & 0xff));
    return Slot::fromInt(0);
}

/** `__vm_str_cmp(左, 右) -> -1 / 0 / 1`（字典序；空指针当空串）。 */
Slot strCmp(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string left, right;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &left) ||
        !nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &right))
    {
        ctx.error = "比较字符串时读不到操作数（地址 " + std::to_string(nativeInt(args[0])) + " / " +
                    std::to_string(nativeInt(args[1])) + "）";
        return Slot::fromInt(0);
    }
    const int order = left.compare(right);
    return Slot::fromInt(order < 0 ? -1 : (order > 0 ? 1 : 0));
}

/** `__vm_str_pos(串, 子串) -> 位置 / -1`：子串第一次出现的位置。 */
Slot strPos(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text, needle;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text) ||
        !nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &needle))
    {
        ctx.error = "查找子串时读不到操作数（地址 " + std::to_string(nativeInt(args[0])) + " / " +
                    std::to_string(nativeInt(args[1])) + "）";
        return Slot::fromInt(-1);
    }
    const std::size_t found = text.find(needle);
    return Slot::fromInt(found == std::string::npos ? -1 : static_cast<std::int64_t>(found));
}

/**
 * `__vm_str_set(串, 下标, 字符码) -> 0`：往字符串的第 `下标` 个字节写一个字符。
 *
 * 字符串在堆上是**可写**的（字面量当值用时已经复制成自己的那块），越界就报错 ——
 * 和数组下标一个脾气。写进去的是低 8 位。
 */
Slot strSet(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::size_t address = static_cast<std::size_t>(nativeInt(args[0]));
    std::string text;
    if (!nativeReadCString(ctx, address, &text))
    {
        ctx.error = "改字符时读不到字符串（地址 " + std::to_string(address) + "）";
        return Slot::fromInt(0);
    }
    const std::int64_t index = nativeInt(args[1]);
    if (index < 0 || static_cast<std::size_t>(index) >= text.size())
    {
        ctx.error = "字符串下标越界：" + std::to_string(index) + "（长度 " +
                    std::to_string(text.size()) + "）";
        return Slot::fromInt(0);
    }
    const char code = static_cast<char>(nativeInt(args[2]) & 0xFF);
    if (!nativeWrite(ctx, address + 8u + static_cast<std::size_t>(index), &code, 1u))
    {
        ctx.error = "往字符串里写字符失败（地址 " + std::to_string(address) + "）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(0);
}

/** `__vm_str_at(串, 下标) -> 字符码 / -1`：按下标取一个字符。 */
Slot strAt(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text))
    {
        ctx.error = "取字符时读不到字符串（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(-1);
    }
    /* 长度是显式元数据，下标必须严格落在 [0, length) 内。 */
    const std::int64_t index = nativeInt(args[1]);
    if (index < 0 || static_cast<std::size_t>(index) >= text.size())
    {
        ctx.error = "字符串下标越界：" + std::to_string(index) + "（长度 " +
                    std::to_string(text.size()) + "）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<unsigned char>(text[static_cast<std::size_t>(index)]));
}

/**
 * `__vm_memcpy(目标, 源, 字节数) -> 0`：逐字节搬一段（C++ 里就是 `memcpy`）。
 *
 * 源与目标整段必须**各自落在一块已分配内存里**（堆块 / 数据区符号），否则报错 ——
 * 以前语言侧是"在目标地址上读 8 字节 → 只改最低那个字节 → 写回"，那么读堆串最后一
 * 个字节时那 8 字节会越过块尾（现在字符串在堆上，块是紧的）。
 */
Slot memCopy(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::size_t target = static_cast<std::size_t>(nativeInt(args[0]));
    const std::size_t source = static_cast<std::size_t>(nativeInt(args[1]));
    const std::int64_t count = nativeInt(args[2]);
    if (count < 0)
    {
        ctx.error = "memcpy 的字节数不能是负数：" + std::to_string(count);
        return Slot::fromInt(0);
    }
    if (count == 0) return Slot::fromInt(0);
    std::string buffer(static_cast<std::size_t>(count), '\0');
    if (!nativeRead(ctx, source, &buffer[0], static_cast<std::size_t>(count)))
    {
        ctx.error = "memcpy 读不到源地址 " + std::to_string(source) + " 处的 " +
                    std::to_string(count) + " 个字节（不在任何一块已分配内存里）";
        return Slot::fromInt(0);
    }
    if (!nativeWrite(ctx, target, buffer.data(), static_cast<std::size_t>(count)))
    {
        ctx.error = "memcpy 写不进目标地址 " + std::to_string(target) + " 处的 " +
                    std::to_string(count) + " 个字节（不在任何一块已分配内存里）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(0);
}

/** `__vm_memset(地址, 字节值, 字节数) -> 0`：把一段内存填成同一个字节。 */
Slot memSet(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::size_t address = static_cast<std::size_t>(nativeInt(args[0]));
    const char value = static_cast<char>(nativeInt(args[1]) & 0xFF);
    const std::int64_t count = nativeInt(args[2]);
    if (count < 0)
    {
        ctx.error = "memset 的字节数不能是负数：" + std::to_string(count);
        return Slot::fromInt(0);
    }
    if (count == 0) return Slot::fromInt(0);
    const std::string buffer(static_cast<std::size_t>(count), value);
    if (!nativeWrite(ctx, address, buffer.data(), static_cast<std::size_t>(count)))
    {
        ctx.error = "memset 写不进地址 " + std::to_string(address) + " 处的 " +
                    std::to_string(count) + " 个字节（不在任何一块已分配内存里）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(0);
}

/**
 * `__vm_read_text() -> 堆上的新串地址`：把**下一项输入原文**读成一个字符串。
 *
 * `input_number(x)` 走的是 `(read, _, _, x)`（按变量类型解析一个数），而字符串没法
 * "解析"成一个数，所以单独给一个原生函数：输入项本身就是文本，直接复制到堆上。
 * 注意取完就往前走一项，两种读法**共用**同一个输入序号。
 */
Slot readText(NativeContext &ctx, const std::vector<Slot> &)
{
    if (ctx.input == nullptr || ctx.inputIndex == nullptr)
    {
        ctx.error = "这个程序没有接输入，取不到字符串";
        return Slot::fromInt(0);
    }
    if (*ctx.inputIndex >= ctx.input->size())
    {
        ctx.error = "输入不够（第 " + std::to_string(*ctx.inputIndex + 1u) + " 项缺失）";
        return Slot::fromInt(0);
    }
    const std::string text = (*ctx.input)[(*ctx.inputIndex)++];
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, text)));
}

/** 标准输入是不是交互式终端——决定 `__vm_pause` 要不要真的停下来等。 */
bool standardInputIsInteractive()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

/**
 * `__vm_pause() -> 0`：调试用"临时停一下"，等到一行输入再继续（`std::pause()` 降到它）。
 *
 * **只在交互式终端里真的停**：标准输入不是终端（重定向、管道、CI、嵌入式宿主）时立刻返回，
 * 所以回归样例和自动化跑批不会被它挂住。停下时提示走 stderr，不污染程序的 stdout；
 * 读到的内容直接丢弃（它是"按回车继续"，不是数据输入——要数据请用 `lang::inputLine`）。
 */
Slot pauseForDebug(NativeContext &, const std::vector<Slot> &)
{
    if (standardInputIsInteractive())
    {
        std::fputs("stackvm：按回车继续…", stderr);
        int character = 0;
        while ((character = std::getchar()) != '\n' && character != EOF) { }
    }
    return Slot::fromInt(0);
}

bool takeInput(NativeContext &ctx, const char *name, std::string *value)
{
    if (ctx.input == nullptr || ctx.inputIndex == nullptr)
    {
        ctx.error = std::string("这个程序没有接输入，无法调用 std::") + name;
        return false;
    }
    if (*ctx.inputIndex >= ctx.input->size())
    {
        ctx.error = "输入不够（第 " + std::to_string(*ctx.inputIndex + 1u) + " 项缺失）";
        return false;
    }
    *value = (*ctx.input)[(*ctx.inputIndex)++];
    return true;
}

Slot inputInt(NativeContext &ctx, const std::vector<Slot> &)
{
    std::string text;
    if (!takeInput(ctx, "inputInt", &text)) return Slot::fromInt(0);
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
    {
        ctx.error = "std::inputInt 读到的不是合法 int：\"" + text + "\"";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(value);
}

Slot inputUInt(NativeContext &ctx, const std::vector<Slot> &)
{
    std::string text;
    if (!takeInput(ctx, "inputUInt", &text)) return Slot::fromInt(0);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
    {
        ctx.error = "std::inputUInt 读到的不是合法 uint：\"" + text + "\"";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<std::int64_t>(value));
}

Slot inputFloat(NativeContext &ctx, const std::vector<Slot> &)
{
    std::string text;
    if (!takeInput(ctx, "inputFloat", &text)) return Slot::fromDouble(0.0);
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(value))
    {
        ctx.error = "std::inputFloat 读到的不是有限浮点数：\"" + text + "\"";
        return Slot::fromDouble(0.0);
    }
    return Slot::fromDouble(value);
}

Slot inputBool(NativeContext &ctx, const std::vector<Slot> &)
{
    std::string text;
    if (!takeInput(ctx, "inputBool", &text)) return Slot::fromInt(0);
    if (text == "true") return Slot::fromInt(1);
    if (text == "false") return Slot::fromInt(0);
    ctx.error = "std::inputBool 只接受 true 或 false，实际为：\"" + text + "\"";
    return Slot::fromInt(0);
}

Slot inputChar(NativeContext &ctx, const std::vector<Slot> &)
{
    std::string text;
    if (!takeInput(ctx, "inputChar", &text)) return Slot::fromInt(0);
    return Slot::fromInt(text.empty() ? 0 : static_cast<unsigned char>(text.front()));
}

enum class ParseStatus : std::int64_t { Ok = 0, Empty = 1, Overflow = 2, Invalid = 3 };

template <typename Integer>
ParseStatus parseIntegerText(NativeContext &ctx, const Slot &source, Integer *value)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(source)), &text))
    {
        ctx.error = "std::Parse 收到了无效字符串地址";
        return ParseStatus::Invalid;
    }
    if (text.empty()) return ParseStatus::Empty;
    Integer parsed{};
    const char *const begin = text.data();
    const char *const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed, 10);
    if (result.ec == std::errc::result_out_of_range) return ParseStatus::Overflow;
    if (result.ec != std::errc() || result.ptr != end) return ParseStatus::Invalid;
    if (value != nullptr) *value = parsed;
    return ParseStatus::Ok;
}

ParseStatus parseFloatText(NativeContext &ctx, const Slot &source, double *value)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(source)), &text))
    {
        ctx.error = "std::Parse 收到了无效字符串地址";
        return ParseStatus::Invalid;
    }
    if (text.empty()) return ParseStatus::Empty;
    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || !std::isfinite(parsed)) return ParseStatus::Overflow;
    if (end != text.c_str() + text.size()) return ParseStatus::Invalid;
    if (value != nullptr) *value = parsed;
    return ParseStatus::Ok;
}

Slot parseInt64Error(NativeContext &ctx, const std::vector<Slot> &args)
{
    return Slot::fromInt(static_cast<std::int64_t>(parseIntegerText<std::int64_t>(ctx, args[0], nullptr)));
}

Slot parseInt64Value(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::int64_t value = 0;
    const ParseStatus status = parseIntegerText(ctx, args[0], &value);
    if (status != ParseStatus::Ok && ctx.error.empty()) ctx.error = "std::Parse.parseInt64 输入无效";
    return Slot::fromInt(value);
}

Slot parseUInt64Error(NativeContext &ctx, const std::vector<Slot> &args)
{
    return Slot::fromInt(static_cast<std::int64_t>(parseIntegerText<std::uint64_t>(ctx, args[0], nullptr)));
}

Slot parseUInt64Value(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::uint64_t value = 0;
    const ParseStatus status = parseIntegerText(ctx, args[0], &value);
    if (status != ParseStatus::Ok && ctx.error.empty()) ctx.error = "std::Parse.parseUInt64 输入无效";
    return Slot::fromInt(static_cast<std::int64_t>(value));
}

Slot parseFloat64Error(NativeContext &ctx, const std::vector<Slot> &args)
{
    return Slot::fromInt(static_cast<std::int64_t>(parseFloatText(ctx, args[0], nullptr)));
}

Slot parseFloat64Value(NativeContext &ctx, const std::vector<Slot> &args)
{
    double value = 0.0;
    const ParseStatus status = parseFloatText(ctx, args[0], &value);
    if (status != ParseStatus::Ok && ctx.error.empty()) ctx.error = "std::Parse.parseFloat64 输入无效";
    return Slot::fromDouble(value);
}

ParseStatus parseBoolText(NativeContext &ctx, const Slot &source, bool *value)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(source)), &text))
    {
        ctx.error = "std::Parse 收到了无效字符串地址";
        return ParseStatus::Invalid;
    }
    if (text.empty()) return ParseStatus::Empty;
    if (text == "true" || text == "false")
    {
        if (value != nullptr) *value = text == "true";
        return ParseStatus::Ok;
    }
    return ParseStatus::Invalid;
}

Slot parseBoolError(NativeContext &ctx, const std::vector<Slot> &args)
{
    return Slot::fromInt(static_cast<std::int64_t>(parseBoolText(ctx, args[0], nullptr)));
}

Slot parseBoolValue(NativeContext &ctx, const std::vector<Slot> &args)
{
    bool value = false;
    const ParseStatus status = parseBoolText(ctx, args[0], &value);
    if (status != ParseStatus::Ok && ctx.error.empty()) ctx.error = "std::Parse.parseBool 输入无效";
    return Slot::fromInt(value ? 1 : 0);
}

std::string normalizePathText(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string prefix;
    std::size_t begin = 0u;
    bool absolute = false;
    if (path.size() >= 2u && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
    {
        prefix = path.substr(0u, 2u);
        begin = 2u;
        if (begin < path.size() && path[begin] == '/') { absolute = true; ++begin; }
    }
    else if (!path.empty() && path[0] == '/')
    {
        absolute = true;
        begin = 1u;
    }
    std::vector<std::string> parts;
    while (begin <= path.size())
    {
        const std::size_t end = path.find('/', begin);
        const std::string part = path.substr(begin, end == std::string::npos
                                                        ? std::string::npos : end - begin);
        if (!part.empty() && part != ".")
        {
            if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
            else if (part != ".." || !absolute) parts.push_back(part);
        }
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    std::string result = prefix;
    if (absolute) result += '/';
    for (const std::string &part : parts)
    {
        if (!result.empty() && result.back() != '/') result += '/';
        result += part;
    }
    if (result.empty()) return ".";
    return result;
}

Slot pathNormalize(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string path;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &path))
        return ctx.error = "std::Path.normalize 收到了无效字符串地址", Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, normalizePathText(path))));
}

Slot pathJoin(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string left;
    std::string right;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &left) ||
        !nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &right))
        return ctx.error = "std::Path.join 收到了无效字符串地址", Slot::fromInt(0);
    const bool rightAbsolute = !right.empty() &&
        (right[0] == '/' || right[0] == '\\' ||
         (right.size() >= 2u && std::isalpha(static_cast<unsigned char>(right[0])) && right[1] == ':'));
    const std::string joined = rightAbsolute ? right : left + (left.empty() ? "" : "/") + right;
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, normalizePathText(joined))));
}

Slot pathFileName(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string path;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &path))
        return ctx.error = "std::Path.fileName 收到了无效字符串地址", Slot::fromInt(0);
    path = normalizePathText(path);
    const std::size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1u);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, name)));
}

Slot pathExtension(NativeContext &ctx, const std::vector<Slot> &args)
{
    const Slot nameSlot = pathFileName(ctx, args);
    if (!ctx.error.empty()) return Slot::fromInt(0);
    std::string name;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(nameSlot)), &name))
        return ctx.error = "std::Path.extension 无法读取文件名", Slot::fromInt(0);
    ctx.heap->release(static_cast<std::size_t>(nativeInt(nameSlot)));
    const std::size_t dot = name.find_last_of('.');
    const std::string extension = (dot == std::string::npos || dot == 0u)
                                      ? std::string() : name.substr(dot);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, extension)));
}

/* ---------------------------------------------------------------------------
 *  运行期对象：大整数（bint）与三个容器（vec / set / map）
 * ---------------------------------------------------------------------------
 *
 * 这些东西的"值"**不是**内存地址，而是**对象表里的句柄**：表在 C++ 侧，一个
 * `std::shared_ptr<RuntimeObject>` 装下一种对象。宿主语言的类型只决定"句柄怎么用"：
 *
 *   * `bint`     —— 任意精度整数，后端是 `library/big_number` 的 `bigint::BigInt`；
 *   * `vec<T>`   —— 有序表（`std::vector`）：按下标增删查改；
 *   * `set<T>`   —— 有序集合（`std::map` 的键）：元素不重复、按升序遍历；
 *   * `map<K,V>` —— 映射（`std::map`）；
 *   * `obj` —— **装箱格子**（一个值 + 它的类型号）：`set` 装进去、`get` 取出来。
 *
 * 容器里的字符串按**副本**存（`std::string`），取出来时在堆上复制一份新的 —— 这样
 * "变量出了作用域被释放"不会让容器里的元素悬空，容器也就活得比某根字符串久。
 *
 * 句柄取一个**很高的区间**（`kObjectHandleBase` 起），和堆地址（几千）、数据区地址
 * 分开：`(free, …)` / `(drop, …)` 一眼就能分出"这是对象句柄还是内存地址"。
 */

/** 对象句柄的起点（1e12）：堆地址 / 数据区地址都远小于它。 */
constexpr std::int64_t kObjectHandleBase = 1000000000000LL;

/** 对象种类。 */
enum class ObjectKind
{
    BigInt,   ///< 任意精度整数（bint）
    BigFloat, ///< 任意精度十进制浮点（bfloat）
    Vector,   ///< 有序表（vec<T>）
    Set,      ///< 有序集合（set<T>）
    Map,      ///< 映射（map<K,V>）
    Obj,      ///< 装箱格子（obj）：一个值 + 它的类型号
    File,     ///< 宿主文件句柄（file）
    Process,  ///< 宿主进程管道（proc）
    Arena,    ///< 批量持有字节堆分配的标准库 Arena
};

/** 种类名（诊断文案里用）。 */
[[nodiscard]] const char *objectKindName(ObjectKind kind)
{
    switch (kind)
    {
    case ObjectKind::BigInt: return "BigInteger";
    case ObjectKind::BigFloat: return "BigDecimal";
    case ObjectKind::Vector: return "vec";
    case ObjectKind::Set: return "set";
    case ObjectKind::Map: return "map";
    case ObjectKind::Obj: return "obj";
    case ObjectKind::File: return "file";
    case ObjectKind::Process: return "proc";
    case ObjectKind::Arena: return "arena";
    }
    return "?";
}

/** 集合 / 映射的键：对象按可跨副本保留的身份排序，其余按值排序。 */
struct ObjectKey
{
    bool isStruct = false;
    bool isObject = false;
    bool isText = false;
    bool isFloat = false;
    bool ownsObject = false;
    std::int64_t integer = 0;
    std::int64_t ownedInteger = 0;
    double real = 0.0;
    std::string text;
    std::vector<ObjectKey> fields;

    /** 类型不同时按"数值 < 字符串"排（保证严格弱序）；同类型按值比。 */
    bool operator<(const ObjectKey &other) const
    {
        if (isStruct != other.isStruct) return !isStruct;
        if (isStruct) return fields < other.fields;
        if (isText != other.isText) return !isText;
        if (isText) return text < other.text;
        if (isFloat != other.isFloat)
        {
            const double left = isFloat ? real : static_cast<double>(integer);
            const double right = other.isFloat ? other.real : static_cast<double>(other.integer);
            return left < right;
        }
        return isFloat ? (real < other.real) : (integer < other.integer);
    }
};

/** 容器里的一个值：数值（整数 / 浮点）或字符串（**存副本**）。 */
struct ObjectValue
{
    bool isStruct = false;
    bool isText = false;
    bool isFloat = false;
    bool ownsObject = false;
    std::int64_t integer = 0;
    std::int64_t objectIdentity = 0;
    double real = 0.0;
    std::string text;
    std::vector<ObjectValue> fields;

    /** 当键用（集合元素、映射的键）。 */
    [[nodiscard]] ObjectKey key(bool keepOwnership = false) const
    {
        ObjectKey out;
        out.isStruct = isStruct;
        out.isObject = objectIdentity != 0;
        out.isText = isText;
        out.isFloat = isFloat;
        out.ownsObject = keepOwnership && ownsObject;
        out.integer = objectIdentity != 0 ? objectIdentity : integer;
        out.ownedInteger = integer;
        out.real = real;
        out.text = text;
        for (const ObjectValue &field : fields) out.fields.push_back(field.key(keepOwnership));
        return out;
    }
};

/** 一个运行期对象（几种里的一种，用同一个结构装下，省一套 union 的麻烦）。 */
struct RuntimeObject
{
    ObjectKind kind = ObjectKind::BigInt;
    std::int64_t identity = 0;                ///< 值副本共享的键身份；新对象由 ObjectStore::add 分配。
#if STACKVM_WITH_BIGINT
    bigint::BigInt number;                     ///< BigInt：值本身（关闭大整数时这个字段不在）
#endif
#if STACKVM_WITH_BIGFLOAT
    bigfloat::BigFloat decimal;                 ///< BigFloat：任意精度十进制浮点
    int decimalPrecision = 0;                  ///< 0 使用运行期默认值；正数是该值的除法有效位数。
#endif
    std::vector<ObjectValue> items;            ///< Vector：有序元素
    std::map<ObjectKey, ObjectValue> entries;  ///< Set / Map：键 → 值（Set 的值不用）
    /**
     * Obj：**类型号**（由前端在编译期算好，`nameof(类型)` 给的就是它；0 = 还没 set 过）。
     * 有了类型号，`obj.name()` 才能和 `nameof(...)` 比对 —— 泛型就是靠这一条分支的。
     */
    std::int64_t objTag = 0;
    /** Obj：类型名（**只为诊断文案**：`obj.get` 取错类型时要说清里面装的是什么）。 */
    std::string objTypeName;
    /// Obj：装着的值（数值 / 字符串副本；装指针时就是那个地址）。
    ObjectValue objValue;
    /// Obj：objValue.integer 是不是一份由这个盒子持有的运行期对象句柄。
    bool objOwnsObject = false;
    /// File：宿主文件流。
    std::shared_ptr<FileHandle> file;
    /// File：诊断用路径。
    std::string filePath;
    /// File：标准库 Result 包装层最近一次操作的错误。
    std::string fileError;
    /// File：最近一次无陷阱操作是否成功。
    bool fileSucceeded = true;
    /// Process：`popen` 返回的宿主管道。
    std::FILE *process = nullptr;
    /// Process：诊断用命令。
    std::string command;
    /// Arena：由该 arena 持有、reset/drop 时成批释放的字节堆地址。
    std::vector<std::size_t> arenaBlocks;
};

#if !STACKVM_WITH_BIGINT
/**
 * @brief 没把大整数编进来时的一句人话（比"函数入口不存在"强）。只有"轻量构建"里才有。
 * @param ctx 环境。
 * @return 永远返回 0（调用方直接返回它）。
 */
Slot bigIntDisabled(NativeContext &ctx)
{
    ctx.error = "这个构建没有把大整数编进来（附加功能）：用 "
                "`make -C library/stackvm STACKVM_WITH_BIGINT=1` 重建虚拟机，"
                "宿主再链上 build/big_number/libbig_number.a";
    return Slot::fromInt(0);
}
#endif

/** 对象表：句柄 → 对象。 */
struct ObjectStore
{
    std::map<std::int64_t, std::shared_ptr<RuntimeObject>> objects;   ///< 还活着的对象
    std::int64_t next = kObjectHandleBase;                            ///< 下一个句柄
    std::vector<std::size_t> materializedHeap;
    std::vector<std::int64_t> materializedObjects;

    /** 收下一个对象，返回它的句柄。 */
    std::int64_t add(std::shared_ptr<RuntimeObject> object)
    {
        const std::int64_t handle = next++;
        if (object->identity == 0) object->identity = handle;
        objects[handle] = std::move(object);
        return handle;
    }

    /** 按句柄找对象；已经释放 / 不认识返回 nullptr。 */
    RuntimeObject *find(std::int64_t handle)
    {
        const auto found = objects.find(handle);
        return (found == objects.end()) ? nullptr : found->second.get();
    }

    bool copyValue(ObjectValue *value, std::string *error)
    {
        if (value == nullptr) return true;
        for (ObjectValue &field : value->fields)
            if (!copyValue(&field, error)) return false;
        if (!value->ownsObject) return true;
        const std::int64_t nestedCopy = copyHandle(value->integer, error);
        if (nestedCopy == 0) return false;
        value->integer = nestedCopy;
        return true;
    }

    bool copyKey(ObjectKey *key, std::string *error)
    {
        if (key == nullptr) return true;
        for (ObjectKey &field : key->fields)
            if (!copyKey(&field, error)) return false;
        if (!key->ownsObject) return true;
        const std::int64_t nestedCopy = copyHandle(key->ownedInteger, error);
        if (nestedCopy == 0) return false;
        key->ownedInteger = nestedCopy;
        return true;
    }

    bool ownKey(ObjectKey *key, std::string *error)
    {
        if (key == nullptr) return true;
        for (ObjectKey &field : key->fields)
            if (!ownKey(&field, error)) return false;
        if (!key->isObject || key->ownsObject) return true;
        const std::int64_t nestedCopy = copyHandle(key->ownedInteger, error);
        if (nestedCopy == 0) return false;
        key->ownedInteger = nestedCopy;
        key->ownsObject = true;
        return true;
    }

    void releaseValue(const ObjectValue &value)
    {
        for (const ObjectValue &field : value.fields) releaseValue(field);
        if (value.ownsObject) release(value.integer);
    }

    void releaseKey(const ObjectKey &key)
    {
        for (const ObjectKey &field : key.fields) releaseKey(field);
        if (key.ownsObject) release(key.ownedInteger);
    }

    void releaseContents(const RuntimeObject &object)
    {
        for (const ObjectValue &value : object.items) releaseValue(value);
        for (const auto &entry : object.entries)
        {
            releaseKey(entry.first);
            releaseValue(entry.second);
        }
        if (object.kind == ObjectKind::Obj && object.objOwnsObject)
        {
            release(object.objValue.integer);
        }
        if (object.kind == ObjectKind::File && object.file)
        {
            object.file->close();
        }
        if (object.kind == ObjectKind::Process && object.process != nullptr)
        {
#ifdef _WIN32
            _pclose(object.process);
#else
            pclose(object.process);
#endif
        }
    }

    std::shared_ptr<RuntimeObject> copyObject(const RuntimeObject &object, std::string *error)
    {
        if (object.kind == ObjectKind::File || object.kind == ObjectKind::Process ||
            object.kind == ObjectKind::Arena)
        {
            if (error != nullptr)
            {
                *error = std::string(objectKindName(object.kind)) +
                         " 句柄不能复制；请把它当作资源句柄传递并显式 close/delete";
            }
            return nullptr;
        }
        auto copy = std::make_shared<RuntimeObject>(object);
        for (ObjectValue &value : copy->items)
        {
            if (!copyValue(&value, error)) return nullptr;
        }
        if (!copy->entries.empty())
        {
            std::map<ObjectKey, ObjectValue> copiedEntries;
            for (auto entry : copy->entries)
            {
                ObjectKey key = entry.first;
                if (!copyKey(&key, error)) return nullptr;
                if (!copyValue(&entry.second, error)) return nullptr;
                copiedEntries.emplace(std::move(key), std::move(entry.second));
            }
            copy->entries = std::move(copiedEntries);
        }
        if (copy->kind == ObjectKind::Obj && copy->objOwnsObject)
        {
            if (!copyValue(&copy->objValue, error)) return nullptr;
        }
        return copy;
    }

    std::int64_t copyHandle(std::int64_t handle, std::string *error)
    {
        if (handle < kObjectHandleBase)
        {
            if (error != nullptr) *error = "不是一个能复制的对象句柄：" + std::to_string(handle);
            return 0;
        }
        RuntimeObject *object = find(handle);
        if (object == nullptr)
        {
            if (error != nullptr) *error = "这个对象已经释放过了（句柄 " + std::to_string(handle) + "）";
            return 0;
        }
        std::shared_ptr<RuntimeObject> copy = copyObject(*object, error);
        if (!copy) return 0;
        return add(std::move(copy));
    }

    /** 释放一个对象；确实释放了返回 true。 */
    bool release(std::int64_t handle)
    {
        const auto found = objects.find(handle);
        if (found == objects.end()) return false;
        std::shared_ptr<RuntimeObject> object = std::move(found->second);
        objects.erase(found);
        releaseContents(*object);
        return true;
    }

    /** 还活着的对象个数（停机时 != 0 就是漏了）。 */
    [[nodiscard]] std::size_t liveCount() const { return objects.size(); }
};

std::int64_t NativeCallContextImpl::makeTemporary(std::size_t bytes)
{
    if (!context_.heap || !context_.objects) { fail("temporary storage unavailable"); return 0; }
    const auto address = context_.heap->allocate(std::max<std::size_t>(8, bytes));
    if (!address) { fail("ABI temporary allocation failed"); return 0; }
    context_.objects->materializedHeap.push_back(address);
    return static_cast<std::int64_t>(address);
}

bool NativeCallContextImpl::readList(std::int64_t handle,
                                     std::vector<NativeValue> &out) const
{
    out.clear();
    if (context_.objects == nullptr) return false;
    RuntimeObject *object = context_.objects->find(handle);
    if (object == nullptr || object->kind != ObjectKind::Vector) return false;
    out.reserve(object->items.size());
    for (const ObjectValue &item : object->items)
    {
        if (item.isText) out.push_back(NativeValue::fromText(item.text));
        else if (item.isFloat) out.push_back(NativeValue::fromFloat(item.real));
        else out.push_back(NativeValue::fromInt(item.integer));
    }
    return true;
}

/** 句柄是不是"对象表里的句柄"（不是堆地址 / 数据区地址 / 0）。 */
[[nodiscard]] bool isObjectHandle(std::int64_t handle) { return handle >= kObjectHandleBase; }

/**
 * @brief 把一个实参当句柄取对象。
 * @param ctx 环境。
 * @param handle 句柄（`Slot`）。
 * @param kind 期望的种类。
 * @param who 谁的参数（诊断文案）。
 * @return 对象；不是句柄 / 已经释放 / 种类不对时写 `ctx.error` 并返回 nullptr。
 */
RuntimeObject *nativeObject(NativeContext &ctx, const Slot &handle, ObjectKind kind,
                            const char *who)
{
    if (ctx.objects == nullptr || !isObjectHandle(handle.integer))
    {
        ctx.error = std::string(who) + " 需要一个 " + objectKindName(kind) +
                    " 值，拿到的是 " + std::to_string(handle.integer) + "（不是对象句柄）";
        return nullptr;
    }
    RuntimeObject *object = ctx.objects->find(handle.integer);
    if (object == nullptr)
    {
        ctx.error = std::string(who) + " 用的 " + objectKindName(kind) + " 已经释放过了（句柄 " +
                    std::to_string(handle.integer) + "）";
        return nullptr;
    }
    if (object->kind != kind)
    {
        ctx.error = std::string(who) + " 要的是 " + objectKindName(kind) + "，拿到的是 " +
                    objectKindName(object->kind);
        return nullptr;
    }
    return object;
}

/**
 * @brief 一条实参 → 容器里的值 / 键。
 * @param ctx 环境。
 * @param value 值（数值就是 Slot 本身；字符串是它的地址）。
 * @param isText true = 按字符串读（**读成副本**）。
 * @param isObject true = 这个整数是一份由调用者持有的对象句柄。
 * @param copyObject true = 存入容器 / obj 时复制一份，false = 只用于查找比较。
 * @param out 结果。
 * @return 成功返回 true；字符串读不到时写 `ctx.error` 并返回 false。
 */
bool nativeValue(NativeContext &ctx, const Slot &value, bool isText, std::int64_t objectFlag,
                 bool copyObject, ObjectValue *out)
{
    if (objectFlag > 1)
    {
        std::string descriptor;
        if (!nativeReadCString(ctx, static_cast<std::size_t>(objectFlag), &descriptor) ||
            descriptor.empty() || descriptor[0] != '@')
        {
            ctx.error = "容器结构体布局描述无效";
            return false;
        }
        out->isStruct = true;
        for (std::size_t index = 1; index < descriptor.size(); ++index)
        {
            std::int64_t bits = 0;
            const std::size_t address = static_cast<std::size_t>(value.integer) + (index - 1u) * 8u;
            if (!nativeRead(ctx, address, reinterpret_cast<char *>(&bits), sizeof(bits)))
            {
                ctx.error = "容器结构体字段地址无效";
                if (ctx.objects != nullptr) ctx.objects->releaseValue(*out);
                out->fields.clear();
                return false;
            }
            ObjectValue field;
            const char kind = descriptor[index];
            if (kind != 'i' && kind != 's' && kind != 'o' && kind != 'f')
            {
                ctx.error = "容器结构体字段布局无效";
                if (ctx.objects != nullptr) ctx.objects->releaseValue(*out);
                out->fields.clear();
                return false;
            }
            Slot fieldValue = Slot::fromInt(bits);
            if (kind == 'f')
            {
                double real = 0.0;
                std::memcpy(&real, &bits, sizeof(real));
                fieldValue = Slot::fromDouble(real);
            }
            if (!nativeValue(ctx, fieldValue, kind == 's', kind == 'o' ? 1 : 0,
                             copyObject, &field))
            {
                if (ctx.objects != nullptr) ctx.objects->releaseValue(*out);
                out->fields.clear();
                return false;
            }
            out->fields.push_back(std::move(field));
        }
        return true;
    }
    out->isText = isText;
    out->ownsObject = false;
    if (!isText)
    {
        out->isFloat = value.isFloat;
        out->integer = value.integer;
        out->objectIdentity = 0;
        out->real = value.real;
        if (objectFlag == 1)
        {
            RuntimeObject *object = ctx.objects->find(value.integer);
            if (object == nullptr)
            {
                ctx.error = "对象键/值已经释放或不是有效句柄：" +
                            std::to_string(value.integer);
                return false;
            }
            out->objectIdentity = object->identity;
            out->isFloat = false;
            out->real = 0.0;
            if (copyObject)
            {
                std::string error;
                const std::int64_t copy = ctx.objects->copyHandle(value.integer, &error);
                if (copy == 0)
                {
                    ctx.error = error;
                    return false;
                }
                out->integer = copy;
                out->ownsObject = true;
            }
        }
        return true;
    }
    if (!nativeReadCString(ctx, static_cast<std::size_t>(value.integer), &out->text))
    {
        ctx.error = "读容器元素里的字符串失败（地址 " + std::to_string(value.integer) + "）";
        return false;
    }
    return true;
}

/**
 * @brief 容器里的值 / 键 → 一个 Slot（字符串会在堆上复制一份新的）。
 * @param ctx 环境。
 * @param value 值。
 * @param out 结果。
 * @return 成功返回 true。
 */
bool nativeSlot(NativeContext &ctx, const ObjectValue &value, Slot *out)
{
    if (value.isStruct)
    {
        if (ctx.heap == nullptr || ctx.objects == nullptr) return false;
        const std::size_t bytes = std::max<std::size_t>(8u, value.fields.size() * 8u);
        const std::size_t address = ctx.heap->allocate(bytes);
        if (address == 0u)
        {
            ctx.error = "容器结构体取值时堆内存不足";
            return false;
        }
        ctx.objects->materializedHeap.push_back(address);
        for (std::size_t index = 0; index < value.fields.size(); ++index)
        {
            Slot field;
            if (!nativeSlot(ctx, value.fields[index], &field)) return false;
            if (value.fields[index].isText)
                ctx.objects->materializedHeap.push_back(static_cast<std::size_t>(field.integer));
            if (value.fields[index].ownsObject || value.fields[index].objectIdentity != 0)
                ctx.objects->materializedObjects.push_back(field.integer);
            const std::int64_t bits = field.isFloat ? [&]() {
                std::int64_t raw = 0;
                std::memcpy(&raw, &field.real, sizeof(raw));
                return raw;
            }() : field.integer;
            if (!ctx.heap->writeBytes(address + index * 8u,
                                      reinterpret_cast<const char *>(&bits), sizeof(bits)))
            {
                ctx.error = "容器结构体取值时写入字段失败";
                return false;
            }
        }
        *out = Slot::fromInt(static_cast<std::int64_t>(address));
        return true;
    }
    if (!value.isText)
    {
        if (value.ownsObject || value.objectIdentity != 0)
        {
            std::string error;
            const std::int64_t copy = ctx.objects->copyHandle(value.integer, &error);
            if (copy == 0)
            {
                ctx.error = error;
                return false;
            }
            *out = Slot::fromInt(copy);
            return true;
        }
        *out = value.isFloat ? Slot::fromDouble(value.real) : Slot::fromInt(value.integer);
        return true;
    }
    const std::size_t address = nativeAllocString(ctx, value.text);
    if (address == 0u) return false;
    *out = Slot::fromInt(static_cast<std::int64_t>(address));
    return true;
}

/** 造一个对象并返回句柄。 */
Slot nativeNewObject(NativeContext &ctx, ObjectKind kind)
{
    if (ctx.objects == nullptr)
    {
        ctx.error = std::string("没有接上对象表，建不了 ") + objectKindName(kind);
        return Slot::fromInt(0);
    }
    auto object = std::make_shared<RuntimeObject>();
    object->kind = kind;
    return Slot::fromInt(ctx.objects->add(std::move(object)));
}

#if STACKVM_WITH_BIGINT

/** 收下一个算好的大整数（并返回它的句柄）。 */
Slot nativeNewBigInt(NativeContext &ctx, bigint::BigInt value)
{
    const Slot handle = nativeNewObject(ctx, ObjectKind::BigInt);
    RuntimeObject *object = ctx.objects->find(handle.integer);
    if (object == nullptr) return Slot::fromInt(0);
    object->number = std::move(value);
    return handle;
}

/* ---- 大整数（bint）---- */

/** `__vm_bint_from_int(值) -> bint`。 */
Slot bintFromInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    return nativeNewBigInt(ctx, bigint::BigInt(args[0].isFloat
                                                   ? static_cast<std::int64_t>(args[0].real)
                                                   : args[0].integer));
}

/** `__vm_bint_from_str(十进制文本地址) -> bint`。 */
Slot bintFromStr(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text))
    {
        ctx.error = "构造大整数时读不到文本（地址 " + std::to_string(nativeInt(args[0])) + "）";
        return Slot::fromInt(0);
    }
    bool ok = false;
    const bigint::BigInt value = bigint::BigInt::fromString(text, &ok);
    if (!ok)
    {
        ctx.error = "不是合法的大整数文本：\"" + text + "\"（只认可选的 +/- 号加十进制数字）";
        return Slot::fromInt(0);
    }
    return nativeNewBigInt(ctx, value);
}

/** `__vm_bint_from_flt(值) -> bint`（按向零截断）。 */
Slot bintFromFloat(NativeContext &ctx, const std::vector<Slot> &args)
{
    const double value = args[0].isFloat ? args[0].real : static_cast<double>(args[0].integer);
    return nativeNewBigInt(ctx, bigint::BigInt(static_cast<std::int64_t>(value)));
}

/**
 * 大整数的 `+ - * / %`：取两个句柄 → 算 → 把结果装进**新的**对象。
 * @param op `+` / `-` / `*` / `/` / `%`。
 */
Slot bintArith(NativeContext &ctx, const std::vector<Slot> &args, char op)
{
    RuntimeObject *left = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数运算");
    if (left == nullptr) return Slot::fromInt(0);
    RuntimeObject *right = nativeObject(ctx, args[1], ObjectKind::BigInt, "大整数运算");
    if (right == nullptr) return Slot::fromInt(0);
    if ((op == '/' || op == '%') && right->number.isZero())
    {
        ctx.error = std::string("大整数 ") + op + " 0：除数是零";
        return Slot::fromInt(0);
    }
    switch (op)
    {
    case '+': return nativeNewBigInt(ctx, left->number + right->number);
    case '-': return nativeNewBigInt(ctx, left->number - right->number);
    case '*': return nativeNewBigInt(ctx, left->number * right->number);
    case '/': return nativeNewBigInt(ctx, left->number / right->number);
    default: return nativeNewBigInt(ctx, left->number % right->number);
    }
}

Slot bintAdd(NativeContext &ctx, const std::vector<Slot> &args) { return bintArith(ctx, args, '+'); }
Slot bintSub(NativeContext &ctx, const std::vector<Slot> &args) { return bintArith(ctx, args, '-'); }
Slot bintMul(NativeContext &ctx, const std::vector<Slot> &args) { return bintArith(ctx, args, '*'); }
Slot bintDiv(NativeContext &ctx, const std::vector<Slot> &args) { return bintArith(ctx, args, '/'); }
Slot bintMod(NativeContext &ctx, const std::vector<Slot> &args) { return bintArith(ctx, args, '%'); }

/** `__vm_bint_neg(值) -> bint`：取负。 */
Slot bintNeg(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数取负");
    if (value == nullptr) return Slot::fromInt(0);
    return nativeNewBigInt(ctx, -value->number);
}

/** `__vm_bint_cmp(左, 右) -> -1 / 0 / 1`。 */
Slot bintCmp(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *left = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数比较");
    if (left == nullptr) return Slot::fromInt(0);
    RuntimeObject *right = nativeObject(ctx, args[1], ObjectKind::BigInt, "大整数比较");
    if (right == nullptr) return Slot::fromInt(0);
    if (left->number < right->number) return Slot::fromInt(-1);
    if (right->number < left->number) return Slot::fromInt(1);
    return Slot::fromInt(0);
}

/** `__vm_bint_str(值) -> 新串地址`：转成十进制文本（`.str()`）。 */
Slot bintToStr(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数转字符串");
    if (value == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, value->number.toString())));
}

/**
 * @brief 大整数 → 64 位整数：取**低 64 位**（二进制补码截断，和 C 的整数转换一个脾气）。
 *
 * 不能借 `toDouble()` 绕（`double` 只有 53 位有效位，`(int)9223372036854775807` 会
 * 因为四舍五入溢出成 0）；这里按 2^32 进制两位两位地取余数拼出来。
 * @param value 大整数。
 * @return 值。
 */
std::int64_t bigIntToInt64(const bigint::BigInt &value)
{
    const bigint::BigInt base(4294967296LL);   /* 2^32 */
    bigint::BigInt current = value.abs();
    std::uint64_t low = 0u;
    for (int index = 0; index < 2; ++index)
    {
        bigint::BigInt quotient;
        bigint::BigInt remainder;
        if (!current.divmod(base, &quotient, &remainder)) break;
        low |= static_cast<std::uint64_t>(remainder.toDouble()) << (32 * index);
        current = quotient;
    }
    const std::int64_t bits = static_cast<std::int64_t>(low);
    return (value.sign() < 0) ? -bits : bits;
}

/** `__vm_bint_to_int(值) -> 整数`（超出 64 位取低 64 位）。 */
Slot bintToInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数转 int");
    if (value == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(bigIntToInt64(value->number));
}

/** `__vm_bint_to_flt(值) -> 浮点`。 */
Slot bintToFloat(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数转 double");
    if (value == nullptr) return Slot::fromDouble(0.0);
    return Slot::fromDouble(value->number.toDouble());
}

/** `__vm_read_bint() -> bint`：把下一项输入按十进制读成一个大整数（`input_bstr`）。 */
Slot readBigInt(NativeContext &ctx, const std::vector<Slot> &)
{
    if (ctx.input == nullptr || ctx.inputIndex == nullptr)
    {
        ctx.error = "这个程序没有接输入，取不到大整数";
        return Slot::fromInt(0);
    }
    if (*ctx.inputIndex >= ctx.input->size())
    {
        ctx.error = "输入不够（第 " + std::to_string(*ctx.inputIndex + 1u) + " 项缺失）";
        return Slot::fromInt(0);
    }
    const std::string text = (*ctx.input)[(*ctx.inputIndex)++];
    bool ok = false;
    const bigint::BigInt value = bigint::BigInt::fromString(text, &ok);
    if (!ok)
    {
        ctx.error = "输入项不是合法的大整数：\"" + text + "\"";
        return Slot::fromInt(0);
    }
    return nativeNewBigInt(ctx, value);
}

#else  /* !STACKVM_WITH_BIGINT：大整数没编进来，这几个名字都指向"一句人话" */

/* 名字照样在表里（调用点不用改），但一调就报"这个构建没有把大整数编进来" ——
 * 比"函数入口不存在"清楚得多。 */
#define STACKVM_BINT_STUB(name)     Slot name(NativeContext &cx, const std::vector<Slot> &) { return bigIntDisabled(cx); }

STACKVM_BINT_STUB(bintFromInt)
STACKVM_BINT_STUB(bintFromStr)
STACKVM_BINT_STUB(bintFromFloat)
STACKVM_BINT_STUB(bintAdd)
STACKVM_BINT_STUB(bintSub)
STACKVM_BINT_STUB(bintMul)
STACKVM_BINT_STUB(bintDiv)
STACKVM_BINT_STUB(bintMod)
STACKVM_BINT_STUB(bintNeg)
STACKVM_BINT_STUB(bintCmp)
STACKVM_BINT_STUB(bintToStr)
STACKVM_BINT_STUB(bintToInt)
STACKVM_BINT_STUB(bintToFloat)
STACKVM_BINT_STUB(readBigInt)

#undef STACKVM_BINT_STUB

#endif  /* STACKVM_WITH_BIGINT */

#if STACKVM_WITH_BIGFLOAT

Slot nativeNewBigFloat(NativeContext &ctx, bigfloat::BigFloat value, int precision = 0)
{
    const Slot handle = nativeNewObject(ctx, ObjectKind::BigFloat);
    RuntimeObject *object = ctx.objects->find(handle.integer);
    if (object == nullptr) return Slot::fromInt(0);
    object->decimal = std::move(value);
    object->decimalPrecision = precision;
    return handle;
}

Slot bfloatFromInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    return nativeNewBigFloat(ctx, bigfloat::BigFloat::fromInt(
        bigint::BigInt(args[0].isFloat ? static_cast<std::int64_t>(args[0].real) : args[0].integer)));
}

Slot bfloatFromFloat(NativeContext &ctx, const std::vector<Slot> &args)
{
    return nativeNewBigFloat(ctx, bigfloat::BigFloat::fromDouble(
        args[0].isFloat ? args[0].real : static_cast<double>(args[0].integer)));
}

Slot bfloatFromStr(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &text))
    {
        ctx.error = "构造大浮点数时读不到文本";
        return Slot::fromInt(0);
    }
    bool ok = false;
    const bigfloat::BigFloat value = bigfloat::BigFloat::fromString(text, &ok);
    if (!ok)
    {
        ctx.error = "不是合法的大浮点数文本：\"" + text + "\"";
        return Slot::fromInt(0);
    }
    return nativeNewBigFloat(ctx, value);
}

Slot bfloatFromBigInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigInt, "大整数转大浮点数");
    if (value == nullptr) return Slot::fromInt(0);
    return nativeNewBigFloat(ctx, bigfloat::BigFloat::fromInt(value->number));
}

Slot bfloatArith(NativeContext &ctx, const std::vector<Slot> &args, char op)
{
    RuntimeObject *left = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数运算");
    if (left == nullptr) return Slot::fromInt(0);
    RuntimeObject *right = nativeObject(ctx, args[1], ObjectKind::BigFloat, "大浮点数运算");
    if (right == nullptr) return Slot::fromInt(0);
    if (op == '/' && right->decimal.isZero())
    {
        ctx.error = "大浮点数除数是零";
        return Slot::fromInt(0);
    }
    /* 结果的精度取两个操作数精度的**最大值**（而不是只看左操作数）。 */
    const int precision = std::max(left->decimalPrecision, right->decimalPrecision);
    if (op == '+')
    {
        auto result = left->decimal + right->decimal;
        return nativeNewBigFloat(ctx, precision ? result.round(precision) : result, precision);
    }
    if (op == '-')
    {
        auto result = left->decimal - right->decimal;
        return nativeNewBigFloat(ctx, precision ? result.round(precision) : result, precision);
    }
    if (op == '*')
    {
        auto result = left->decimal * right->decimal;
        return nativeNewBigFloat(ctx, precision ? result.round(precision) : result, precision);
    }
    const int digits = precision != 0 ? precision : (ctx.bigFloatPrecision ? *ctx.bigFloatPrecision : 34);
    return nativeNewBigFloat(ctx, left->decimal.divide(right->decimal, digits), precision);
}

Slot bfloatAdd(NativeContext &c, const std::vector<Slot> &a) { return bfloatArith(c, a, '+'); }
Slot bfloatSub(NativeContext &c, const std::vector<Slot> &a) { return bfloatArith(c, a, '-'); }
Slot bfloatMul(NativeContext &c, const std::vector<Slot> &a) { return bfloatArith(c, a, '*'); }
Slot bfloatDiv(NativeContext &c, const std::vector<Slot> &a) { return bfloatArith(c, a, '/'); }

Slot bfloatNeg(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数取负");
    return value ? nativeNewBigFloat(ctx, -value->decimal, value->decimalPrecision) : Slot::fromInt(0);
}

Slot bfloatCmp(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *left = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数比较");
    RuntimeObject *right = nativeObject(ctx, args[1], ObjectKind::BigFloat, "大浮点数比较");
    return (left && right) ? Slot::fromInt(left->decimal.compare(right->decimal)) : Slot::fromInt(0);
}

Slot bfloatToStr(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数转字符串");
    if (value == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, value->decimal.toString(0))));
}

Slot bfloatToInt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数转 int");
    if (value == nullptr) return Slot::fromInt(0);
    bigint::BigInt result = value->decimal.mantissa();
    const int exponent = value->decimal.exponent10();
    bigint::BigInt scale(1);
    for (int i = 0; i < std::abs(exponent); ++i) scale *= bigint::BigInt(10);
    result = exponent >= 0 ? result * scale : result / scale;
    return Slot::fromInt(bigIntToInt64(result));
}

Slot bfloatToFloat(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigFloat, "大浮点数转 double");
    if (value == nullptr) return Slot::fromDouble(0.0);
    return Slot::fromDouble(std::strtod(value->decimal.toString(17).c_str(), nullptr));
}

Slot bfloatSetValuePrecision(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *value = nativeObject(ctx, args[0], ObjectKind::BigFloat, "BigDecimal.setPrecision");
    if (value == nullptr) return Slot::fromInt(0);
    const std::int64_t digits = nativeInt(args[1]);
    if (digits < 1 || digits > 100000)
    {
        ctx.error = "BigDecimal 有效位数必须在 1..100000 之间，现在是 " + std::to_string(digits);
        return Slot::fromInt(0);
    }
    const int previous = value->decimalPrecision != 0 ? value->decimalPrecision :
                         (ctx.bigFloatPrecision ? *ctx.bigFloatPrecision : 34);
    value->decimal = value->decimal.round(static_cast<int>(digits));
    value->decimalPrecision = static_cast<int>(digits);
    return Slot::fromInt(previous);
}

Slot readBigFloat(NativeContext &ctx, const std::vector<Slot> &)
{
    if (ctx.input == nullptr || ctx.inputIndex == nullptr || *ctx.inputIndex >= ctx.input->size())
    {
        ctx.error = "输入不够，取不到大浮点数";
        return Slot::fromInt(0);
    }
    bool ok = false;
    const std::string text = (*ctx.input)[(*ctx.inputIndex)++];
    const bigfloat::BigFloat value = bigfloat::BigFloat::fromString(text, &ok);
    if (!ok)
    {
        ctx.error = "输入项不是合法的大浮点数：\"" + text + "\"";
        return Slot::fromInt(0);
    }
    return nativeNewBigFloat(ctx, value);
}

#else

Slot bigFloatDisabled(NativeContext &ctx)
{
    ctx.error = "这个构建没有把大浮点数编进来：用 STACKVM_WITH_BIGFLOAT=1 重建虚拟机";
    return Slot::fromInt(0);
}
#define STACKVM_BFLOAT_STUB(name) Slot name(NativeContext &c, const std::vector<Slot> &) { return bigFloatDisabled(c); }
STACKVM_BFLOAT_STUB(bfloatFromInt)
STACKVM_BFLOAT_STUB(bfloatFromFloat)
STACKVM_BFLOAT_STUB(bfloatFromStr)
STACKVM_BFLOAT_STUB(bfloatFromBigInt)
STACKVM_BFLOAT_STUB(bfloatAdd)
STACKVM_BFLOAT_STUB(bfloatSub)
STACKVM_BFLOAT_STUB(bfloatMul)
STACKVM_BFLOAT_STUB(bfloatDiv)
STACKVM_BFLOAT_STUB(bfloatNeg)
STACKVM_BFLOAT_STUB(bfloatCmp)
STACKVM_BFLOAT_STUB(bfloatToStr)
STACKVM_BFLOAT_STUB(bfloatToInt)
STACKVM_BFLOAT_STUB(bfloatToFloat)
STACKVM_BFLOAT_STUB(bfloatSetValuePrecision)
STACKVM_BFLOAT_STUB(readBigFloat)
#undef STACKVM_BFLOAT_STUB

#endif

/* ---- 容器（vec / set / map）---- */

/** 按下标取第 `index` 个（越界报错）。 */
bool vecIndex(NativeContext &ctx, RuntimeObject *object, std::int64_t index, std::size_t *out,
              const char *who)
{
    if (index < 0 || static_cast<std::size_t>(index) >= object->items.size())
    {
        ctx.error = std::string(who) + " 下标越界：" + std::to_string(index) + "（长度 " +
                    std::to_string(object->items.size()) + "）";
        return false;
    }
    *out = static_cast<std::size_t>(index);
    return true;
}

/** `__vm_vec_new() -> vec`。 */
Slot vecNew(NativeContext &ctx, const std::vector<Slot> &)
{
    return nativeNewObject(ctx, ObjectKind::Vector);
}

/** 把宿主入口参数复制成 `lang::List<lang::String>`。 */
Slot programArguments(NativeContext &ctx, const std::vector<Slot> &)
{
    const Slot handle = nativeNewObject(ctx, ObjectKind::Vector);
    RuntimeObject *object = ctx.objects == nullptr ? nullptr : ctx.objects->find(handle.integer);
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.programArguments == nullptr) return handle;
    object->items.reserve(ctx.programArguments->size());
    for (const std::string &argument : *ctx.programArguments)
    {
        ObjectValue value;
        value.isText = true;
        value.text = argument;
        object->items.push_back(std::move(value));
    }
    return handle;
}

/** `__vm_vec_pack(isText, isObject, values...) -> vec`：MAGEL 变长参数的调用点打包。 */
Slot vecPack(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (args.size() < 2u)
    {
        ctx.error = "变长参数打包至少需要两个类型标志";
        return Slot::fromInt(0);
    }
    auto object = std::make_shared<RuntimeObject>();
    object->kind = ObjectKind::Vector;
    const bool isText = nativeInt(args[0]) != 0;
    const std::int64_t isObject = nativeInt(args[1]);
    for (std::size_t index = 2u; index < args.size(); ++index)
    {
        ObjectValue value;
        if (!nativeValue(ctx, args[index], isText, isObject, true, &value))
        {
            for (const ObjectValue &stored : object->items) ctx.objects->releaseValue(stored);
            return Slot::fromInt(0);
        }
        object->items.push_back(std::move(value));
    }
    return Slot::fromInt(ctx.objects->add(std::move(object)));
}

/** `__vm_vec_push(vec, 值, 值是不是字符串, 值是不是对象句柄) -> 0`：追加到末尾。 */
Slot vecPush(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec.push");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), true,
                     &value)) return Slot::fromInt(0);
    object->items.push_back(std::move(value));
    return Slot::fromInt(0);
}

/** `__vm_vec_insert(vec, 下标, 值, 是不是字符串, 是不是对象句柄) -> 0`：插到下标处（允许 == 长度）。 */
Slot vecInsert(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec.insert");
    if (object == nullptr) return Slot::fromInt(0);
    const std::int64_t index = nativeInt(args[1]);
    if (index < 0 || static_cast<std::size_t>(index) > object->items.size())
    {
        ctx.error = "vec.insert 的位置越界：" + std::to_string(index) + "（长度 " +
                    std::to_string(object->items.size()) + "，只能落在 0..长度 之间）";
        return Slot::fromInt(0);
    }
    ObjectValue value;
    if (!nativeValue(ctx, args[2], nativeInt(args[3]) != 0, nativeInt(args[4]), true,
                     &value)) return Slot::fromInt(0);
    object->items.insert(object->items.begin() + index, std::move(value));
    return Slot::fromInt(0);
}

/** `__vm_vec_at(vec, 下标, 是不是字符串, 是不是对象句柄) -> 值`。 */
Slot vecAt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec 取元素");
    if (object == nullptr) return Slot::fromInt(0);
    std::size_t index = 0u;
    if (!vecIndex(ctx, object, nativeInt(args[1]), &index, "vec")) return Slot::fromInt(0);
    Slot out;
    if (!nativeSlot(ctx, object->items[index], &out)) return Slot::fromInt(0);
    return out;
}

/** `__vm_vec_set(vec, 下标, 值, 是不是字符串, 是不是对象句柄) -> 0`。 */
Slot vecSet(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec 改元素");
    if (object == nullptr) return Slot::fromInt(0);
    std::size_t index = 0u;
    if (!vecIndex(ctx, object, nativeInt(args[1]), &index, "vec")) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[2], nativeInt(args[3]) != 0, nativeInt(args[4]), true,
                     &value)) return Slot::fromInt(0);
    ctx.objects->releaseValue(object->items[index]);
    object->items[index] = std::move(value);
    return Slot::fromInt(0);
}

/** `__vm_vec_remove(vec, 下标) -> 0`：删掉下标处那个。 */
Slot vecRemove(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec.remove");
    if (object == nullptr) return Slot::fromInt(0);
    std::size_t index = 0u;
    if (!vecIndex(ctx, object, nativeInt(args[1]), &index, "vec.remove")) return Slot::fromInt(0);
    ctx.objects->releaseValue(object->items[index]);
    object->items.erase(object->items.begin() + static_cast<std::ptrdiff_t>(index));
    return Slot::fromInt(0);
}

/** `__vm_vec_pop(vec, 是不是字符串, 是不是对象句柄) -> 值`：弹出末尾（空表报错）。 */
Slot vecPop(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec.pop");
    if (object == nullptr) return Slot::fromInt(0);
    if (object->items.empty())
    {
        ctx.error = "vec.pop：表是空的";
        return Slot::fromInt(0);
    }
    Slot out;
    if (!nativeSlot(ctx, object->items.back(), &out)) return Slot::fromInt(0);
    ctx.objects->releaseValue(object->items.back());
    object->items.pop_back();
    return out;
}

/** `__vm_vec_len(vec) -> 元素个数`。 */
Slot vecLen(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "len(vec)");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(object->items.size()));
}

/** `__vm_vec_clear(vec) -> 0`。 */
Slot vecClear(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec.clear");
    if (object == nullptr) return Slot::fromInt(0);
    for (const ObjectValue &value : object->items) ctx.objects->releaseValue(value);
    object->items.clear();
    return Slot::fromInt(0);
}

/** List 的稳定迭代归并排序：O(n log n) 时间、O(n) 辅助空间，不使用递归栈。 */
Slot vecStableSort(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::sortValue");
    if (object == nullptr) return Slot::fromInt(0);
    const std::size_t count = object->items.size();
    if (count < 2u) return Slot::fromInt(0);

#ifdef SIMPLE_SORT
    /* 希尔排序：就地、不额外分配，间距逐步缩到 1；代价是**不稳定**。
     * 定义 `SIMPLE_SORT` 时替换掉默认的自底向上归并，见 README 的构建开关一节。 */
    for (std::size_t gap = count / 2u; gap > 0u; gap /= 2u)
    {
        for (std::size_t index = gap; index < count; ++index)
        {
            ObjectValue held = std::move(object->items[index]);
            std::size_t slot = index;
            while (slot >= gap && held.key() < object->items[slot - gap].key())
            {
                object->items[slot] = std::move(object->items[slot - gap]);
                slot -= gap;
            }
            object->items[slot] = std::move(held);
        }
    }
    return Slot::fromInt(0);
#else
    std::vector<ObjectValue> source = std::move(object->items);
    std::vector<ObjectValue> target(count);
    for (std::size_t width = 1u; width < count;)
    {
        for (std::size_t left = 0u; left < count; left += width * 2u)
        {
            const std::size_t middle = std::min(left + width, count);
            const std::size_t end = std::min(left + width * 2u, count);
            std::size_t first = left;
            std::size_t second = middle;
            std::size_t output = left;
            while (first < middle && second < end)
            {
                /* 相等时取左段，稳定性由这一处分支保证。 */
                if (source[second].key() < source[first].key())
                    target[output++] = std::move(source[second++]);
                else
                    target[output++] = std::move(source[first++]);
            }
            while (first < middle) target[output++] = std::move(source[first++]);
            while (second < end) target[output++] = std::move(source[second++]);
        }
        source.swap(target);
        if (width > count / 2u) break;
        width *= 2u;
    }
    object->items = std::move(source);
    return Slot::fromInt(0);
#endif
}

/** `std::algorithms::sortValue` 的值语义入口：复制 List，再对副本做稳定排序。 */
Slot vecSortedCopy(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (ctx.objects == nullptr)
    {
        ctx.error = "std::algorithms::sortValue 缺少对象表";
        return Slot::fromInt(0);
    }
    std::string error;
    const std::int64_t copy = ctx.objects->copyHandle(nativeInt(args[0]), &error);
    if (copy == 0)
    {
        ctx.error = error;
        return Slot::fromInt(0);
    }
    vecStableSort(ctx, {Slot::fromInt(copy)});
    if (!ctx.error.empty())
    {
        ctx.objects->release(copy);
        return Slot::fromInt(0);
    }
    return Slot::fromInt(copy);
}

/** `__vm_vec_find(vec, 值, 是不是字符串, 是不是对象句柄) -> 下标 / -1`（线性找第一个相等的）。 */
Slot vecFind(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector, "vec 查找");
    if (object == nullptr) return Slot::fromInt(-1);
    ObjectValue wanted;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &wanted)) return Slot::fromInt(-1);
    const ObjectKey key = wanted.key();
    for (std::size_t index = 0; index < object->items.size(); ++index)
    {
        if (object->items[index].key() < key) continue;
        if (key < object->items[index].key()) continue;
        return Slot::fromInt(static_cast<std::int64_t>(index));
    }
    return Slot::fromInt(-1);
}

bool vecCallbackValue(NativeContext &ctx, const ObjectValue &value, NativeValue *out,
                      std::size_t *temporaryString)
{
    *temporaryString = 0u;
    if (value.isStruct)
    {
        Slot materialized;
        if (!nativeSlot(ctx, value, &materialized)) return false;
        *out = NativeValue::fromInt(materialized.integer);
    }
    else if (value.isText)
    {
        *temporaryString = nativeAllocString(ctx, value.text);
        if (*temporaryString == 0u) return false;
        *out = NativeValue::fromInt(static_cast<std::int64_t>(*temporaryString));
    }
    else if (value.isFloat) *out = NativeValue::fromFloat(value.real);
    else *out = NativeValue::fromInt(value.integer);
    return true;
}

Slot vecPredicate(NativeContext &ctx, const std::vector<Slot> &args, bool all)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         all ? "std::algorithms::all"
                                             : "std::algorithms::any");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    for (const ObjectValue &item : object->items)
    {
        NativeValue argument;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, item, &argument, &temporary)) return Slot::fromInt(0);
        NativeValue result;
        const bool invoked = (*ctx.invoker)(nativeInt(args[1]), {argument}, result);
        if (temporary != 0u) ctx.heap->release(temporary);
        if (!invoked) return Slot::fromInt(0);
        const bool accepted = result.asInt() != 0;
        if (all && !accepted) return Slot::fromInt(0);
        if (!all && accepted) return Slot::fromInt(1);
    }
    return Slot::fromInt(all ? 1 : 0);
}

Slot vecAll(NativeContext &ctx, const std::vector<Slot> &args)
{
    return vecPredicate(ctx, args, true);
}

Slot vecAny(NativeContext &ctx, const std::vector<Slot> &args)
{
    return vecPredicate(ctx, args, false);
}

Slot vecFold(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::fold");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    NativeValue accumulated = args[1].isFloat ? NativeValue::fromFloat(args[1].real)
                                               : NativeValue::fromInt(args[1].integer);
    for (const ObjectValue &item : object->items)
    {
        NativeValue value;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, item, &value, &temporary)) return Slot::fromInt(0);
        NativeValue next;
        const bool invoked = (*ctx.invoker)(nativeInt(args[2]), {accumulated, value}, next);
        if (temporary != 0u) ctx.heap->release(temporary);
        if (!invoked) return Slot::fromInt(0);
        accumulated = std::move(next);
    }
    return accumulated.isFloat() ? Slot::fromDouble(accumulated.floating)
                                 : Slot::fromInt(accumulated.integer);
}

/**
 * @brief 算法回调的返回值 → 容器里的值。
 *
 * `kind`：0 = 整数（`char` / `bool` 也在这一档），1 = 浮点（f64），2 = 字符串，
 * 3 = 对象句柄。回调只能传整数与浮点（`invoker` 的约定），所以字符串是**地址**、
 * 对象是**句柄**，都装在 `integer` 里。
 */
bool nativeCallbackResult(NativeContext &ctx, const NativeValue &produced, std::int64_t kind,
                          std::int64_t layout,
                          ObjectValue *out)
{
    out->isText = false;
    out->isFloat = false;
    out->ownsObject = false;
    out->objectIdentity = 0;
    out->integer = 0;
    out->real = 0.0;
    if (kind == 2)
    {
        out->isText = true;
        const std::size_t address = static_cast<std::size_t>(produced.asInt());
        const bool read = nativeReadCString(ctx, address, &out->text);
        /* 回调返回的堆串所有权归调用方（MAGEL 的 `return` 是 move）：内容已经复制进
         * `out->text`，这块堆串就地还掉，否则每个字符串元素漏一块。 */
        if (ctx.heap != nullptr) ctx.heap->release(address);
        if (!read)
        {
            ctx.error = "算法回调返回的字符串读不到（地址 " +
                        std::to_string(produced.asInt()) + "）";
            return false;
        }
        return true;
    }
    if (kind == 3)
        return nativeValue(ctx, Slot::fromInt(produced.asInt()), false, true, true, out);
    if (kind == 4)
        return nativeValue(ctx, Slot::fromInt(produced.asInt()), false, layout, true, out);
    if (kind == 1)
    {
        out->isFloat = true;
        out->real = produced.floating;
        return true;
    }
    out->integer = produced.asInt();
    return true;
}

/** 容器里的值复制一份：`filter` 留下的元素要和原表**各自**持有。 */
bool nativeCopyValue(NativeContext &ctx, const ObjectValue &source, bool isText, bool isObject,
                     ObjectValue *out)
{
    if (source.isStruct)
    {
        *out = source;
        std::string error;
        if (!ctx.objects->copyValue(out, &error))
        {
            ctx.error = error;
            return false;
        }
        return true;
    }
    out->isText = isText;
    out->ownsObject = false;
    out->isFloat = false;
    out->objectIdentity = 0;
    out->integer = 0;
    out->real = 0.0;
    if (isText)
    {
        out->text = source.text;   /* 字符串在 ObjectValue 里是值语义 */
        return true;
    }
    out->isFloat = source.isFloat;
    out->real = source.real;
    out->integer = source.integer;
    if (!isObject) return true;
    std::string error;
    const std::int64_t copy = ctx.objects->copyHandle(source.integer, &error);
    if (copy == 0)
    {
        ctx.error = error;
        return false;
    }
    const RuntimeObject *object = ctx.objects->find(copy);
    out->integer = copy;
    out->objectIdentity = object == nullptr ? source.objectIdentity : object->identity;
    out->ownsObject = true;
    return true;
}

/** 算法内建中途失败的统一收尾：释放已经攒下的元素，返回 0。 */
Slot algorithmAbort(NativeContext &ctx, std::vector<ObjectValue> &items)
{
    for (const ObjectValue &value : items) ctx.objects->releaseValue(value);
    if (ctx.error.empty()) ctx.error = "算法内建执行失败";
    return Slot::fromInt(0);
}

/** `__vm_vec_map(vec, 回调, 结果种类) -> 新 vec`：逐元素换成回调的返回值。 */
Slot vecMap(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::map");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    const std::int64_t kind = nativeInt(args[2]);
    std::vector<ObjectValue> items;
    items.reserve(object->items.size());
    for (const ObjectValue &item : object->items)
    {
        NativeValue argument;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, item, &argument, &temporary))
            return algorithmAbort(ctx, items);
        NativeValue produced;
        const bool invoked = (*ctx.invoker)(nativeInt(args[1]), {argument}, produced);
        /* 字符串实参的临时块同时也是回调形参的存储：回调 `return 形参` 时结果
         * 就是它（MAGEL 的 return 是 move），这时不能先还，交给 `nativeCallbackResult`
         * 统一释放；结果不是它才在这里还。 */
        if (temporary != 0u &&
            (kind != 2 || produced.asInt() != static_cast<std::int64_t>(temporary)))
            ctx.heap->release(temporary);
        if (!invoked) return algorithmAbort(ctx, items);
        ObjectValue value;
        if (!nativeCallbackResult(ctx, produced, kind, nativeInt(args[3]), &value))
            return algorithmAbort(ctx, items);
        items.push_back(std::move(value));
    }
    auto result = std::make_shared<RuntimeObject>();
    result->kind = ObjectKind::Vector;
    result->items = std::move(items);
    return Slot::fromInt(ctx.objects->add(std::move(result)));
}

/** `__vm_vec_filter(vec, 回调, 是不是字符串, 是不是对象) -> 新 vec`：留下回调为真的元素。 */
Slot vecFilter(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::filter");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    const bool isText = nativeInt(args[2]) != 0;
    const bool isObject = nativeInt(args[3]) != 0;
    std::vector<ObjectValue> items;
    for (const ObjectValue &item : object->items)
    {
        NativeValue argument;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, item, &argument, &temporary))
            return algorithmAbort(ctx, items);
        NativeValue produced;
        const bool invoked = (*ctx.invoker)(nativeInt(args[1]), {argument}, produced);
        if (temporary != 0u) ctx.heap->release(temporary);
        if (!invoked) return algorithmAbort(ctx, items);
        if (produced.asInt() == 0) continue;
        ObjectValue value;
        if (!nativeCopyValue(ctx, item, isText, isObject, &value))
            return algorithmAbort(ctx, items);
        items.push_back(std::move(value));
    }
    auto result = std::make_shared<RuntimeObject>();
    result->kind = ObjectKind::Vector;
    result->items = std::move(items);
    return Slot::fromInt(ctx.objects->add(std::move(result)));
}

/** `__vm_vec_reduce(vec, 回调) -> 值`：拿**第一个元素**当初值往后折叠；空表报错。 */
Slot vecReduce(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::reduce");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    if (object->items.empty())
    {
        ctx.error = "std::algorithms::reduce 不能作用在空表上（要给初值就用 fold）";
        return Slot::fromInt(0);
    }
    NativeValue accumulated;
    std::size_t seedTemporary = 0u;
    if (!vecCallbackValue(ctx, object->items[0], &accumulated, &seedTemporary))
        return Slot::fromInt(0);
    for (std::size_t index = 1u; index < object->items.size(); ++index)
    {
        NativeValue value;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, object->items[index], &value, &temporary))
            return Slot::fromInt(0);
        NativeValue next;
        const bool invoked = (*ctx.invoker)(nativeInt(args[1]), {accumulated, value}, next);
        if (temporary != 0u) ctx.heap->release(temporary);
        if (!invoked) return Slot::fromInt(0);
        accumulated = std::move(next);
    }
    /* 初值那份临时串和回调返回的那份都**不在这里还**：可能是数据区字面量，
     * 也可能是只有一个元素时直接当结果用的那份（跟 `vecFold` 一个口径）。 */
    return accumulated.isFloat() ? Slot::fromDouble(accumulated.floating)
                                 : Slot::fromInt(accumulated.integer);
}

/** `__vm_vec_sum(vec, 是不是浮点) -> 值`：数值求和（空表给 0 / 0.0）。 */
Slot vecSum(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::sum");
    if (object == nullptr) return Slot::fromInt(0);
    if (nativeInt(args[1]) != 0)
    {
        double total = 0.0;
        for (const ObjectValue &item : object->items)
            total += item.isFloat ? item.real : static_cast<double>(item.integer);
        return Slot::fromDouble(total);
    }
    std::int64_t total = 0;
    for (const ObjectValue &item : object->items)
        total += item.isFloat ? static_cast<std::int64_t>(item.real) : item.integer;
    return Slot::fromInt(total);
}

/** `__vm_vec_count_if(vec, 回调) -> 整数`：回调为真的元素个数。 */
Slot vecCountIf(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::count");
    if (object == nullptr) return Slot::fromInt(0);
    if (ctx.invoker == nullptr)
    {
        ctx.error = "当前 VM 上下文不能从算法胶水调用函数指针";
        return Slot::fromInt(0);
    }
    std::int64_t total = 0;
    for (const ObjectValue &item : object->items)
    {
        NativeValue argument;
        std::size_t temporary = 0u;
        if (!vecCallbackValue(ctx, item, &argument, &temporary)) return Slot::fromInt(0);
        NativeValue produced;
        const bool invoked = (*ctx.invoker)(nativeInt(args[1]), {argument}, produced);
        if (temporary != 0u) ctx.heap->release(temporary);
        if (!invoked) return Slot::fromInt(0);
        if (produced.asInt() != 0) ++total;
    }
    return Slot::fromInt(total);
}

/** `__vm_vec_count_value(vec, 值, 是不是字符串, 是不是对象) -> 整数`：等于该值的元素个数。 */
Slot vecCountValue(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::count");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue wanted;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &wanted)) return Slot::fromInt(0);
    const ObjectKey key = wanted.key();
    std::int64_t total = 0;
    for (const ObjectValue &item : object->items)
    {
        const ObjectKey candidate = item.key();
        if (candidate < key) continue;
        if (key < candidate) continue;
        ++total;
    }
    return Slot::fromInt(total);
}

/** 已排序 List 的 lower_bound 二分查找；相等元素返回第一项。 */
Slot vecBinarySearch(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::binarySearch");
    if (object == nullptr) return Slot::fromInt(-1);
    ObjectValue wanted;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &wanted))
        return Slot::fromInt(-1);
    const ObjectKey key = wanted.key();
    std::size_t low = 0u;
    std::size_t high = object->items.size();
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2u;
        if (object->items[middle].key() < key) low = middle + 1u;
        else high = middle;
    }
    if (low >= object->items.size() || object->items[low].key() < key ||
        key < object->items[low].key())
        return Slot::fromInt(-1);
    return Slot::fromInt(static_cast<std::int64_t>(low));
}

Slot vecExtreme(NativeContext &ctx, const std::vector<Slot> &args, bool maximum)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         maximum ? "std::algorithms::max"
                                                 : "std::algorithms::min");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue first;
    const std::int64_t objectValue = nativeInt(args[3]);
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, objectValue, false, &first))
        return Slot::fromInt(0);
    const ObjectValue *selected = &first;
    for (const ObjectValue &item : object->items)
    {
        const bool replace = maximum ? selected->key() < item.key() : item.key() < selected->key();
        if (replace) selected = &item;
    }
    if (selected == &first && objectValue == 1) first.ownsObject = true;
    Slot result;
    if (!nativeSlot(ctx, *selected, &result)) return Slot::fromInt(0);
    return result;
}

Slot vecMin(NativeContext &ctx, const std::vector<Slot> &args)
{
    return vecExtreme(ctx, args, false);
}

Slot vecMax(NativeContext &ctx, const std::vector<Slot> &args)
{
    return vecExtreme(ctx, args, true);
}

/** 接收 `[low, high]` List 与 value，返回落在闭区间内的值。 */
Slot vecClamp(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *bounds = nativeObject(ctx, args[0], ObjectKind::Vector,
                                         "std::algorithms::clamp");
    if (bounds == nullptr) return Slot::fromInt(0);
    if (bounds->items.size() != 2u)
    {
        ctx.error = "std::algorithms::clamp 的边界胶水需要 low/high 两项";
        return Slot::fromInt(0);
    }
    if (bounds->items[1].key() < bounds->items[0].key())
    {
        ctx.error = "std::algorithms::clamp 要求 low <= high";
        return Slot::fromInt(0);
    }
    ObjectValue value;
    const std::int64_t objectValue = nativeInt(args[3]);
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, objectValue, false, &value))
        return Slot::fromInt(0);
    const ObjectValue *selected = &value;
    if (value.key() < bounds->items[0].key()) selected = &bounds->items[0];
    else if (bounds->items[1].key() < value.key()) selected = &bounds->items[1];
    if (selected == &value && objectValue == 1) value.ownsObject = true;
    Slot result;
    if (!nativeSlot(ctx, *selected, &result)) return Slot::fromInt(0);
    return result;
}

/** `__vm_set_new() -> set`。 */
Slot setNew(NativeContext &ctx, const std::vector<Slot> &)
{
    return nativeNewObject(ctx, ObjectKind::Set);
}

/** `__vm_set_add(set, 值, 是不是字符串, 是不是对象句柄) -> 0`（已经在里面就什么都不做）。 */
Slot setAdd(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "set.add");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &value)) return Slot::fromInt(0);
    const ObjectKey key = value.key();
    if (object->entries.count(key) == 0u)
    {
        ObjectValue stored;
        if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), true,
                         &stored)) return Slot::fromInt(0);
        object->entries.emplace(key, std::move(stored));
    }
    return Slot::fromInt(0);
}

/** `__vm_set_has(set, 值, 是不是字符串, 是不是对象句柄) -> 0 / 1`。 */
Slot setHas(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "set.has");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &value)) return Slot::fromInt(0);
    return Slot::fromInt(object->entries.count(value.key()) != 0u ? 1 : 0);
}

/** `__vm_set_remove(set, 值, 是不是字符串, 是不是对象句柄) -> 0 / 1`（在不在）。 */
Slot setRemove(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "set.remove");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &value)) return Slot::fromInt(0);
    const auto found = object->entries.find(value.key());
    if (found == object->entries.end()) return Slot::fromInt(0);
    ctx.objects->releaseValue(found->second);
    object->entries.erase(found);
    return Slot::fromInt(1);
}

/** `__vm_set_at(set, 第几个, 是不是字符串, 是不是对象句柄) -> 值`（按键的升序）。 */
Slot setAt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "set.at");
    if (object == nullptr) return Slot::fromInt(0);
    const std::int64_t wanted = nativeInt(args[1]);
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= object->entries.size())
    {
        ctx.error = "set.at 的位置越界：" + std::to_string(wanted) + "（元素个数 " +
                    std::to_string(object->entries.size()) + "）";
        return Slot::fromInt(0);
    }
    auto it = object->entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(wanted));
    /* 集合里存进去的"值"就是元素本身（`setAdd` 用的是 `entries[key] = value`），
     * 所以直接取 `second` 就行。 */
    Slot out;
    if (!nativeSlot(ctx, it->second, &out)) return Slot::fromInt(0);
    return out;
}

/** `__vm_set_len(set) -> 元素个数`。 */
Slot setLen(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "len(set)");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(object->entries.size()));
}

/** `__vm_set_clear(set) -> 0`。 */
Slot setClear(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Set, "set.clear");
    if (object == nullptr) return Slot::fromInt(0);
    for (const auto &entry : object->entries)
    {
        ctx.objects->releaseKey(entry.first);
        ctx.objects->releaseValue(entry.second);
    }
    object->entries.clear();
    return Slot::fromInt(0);
}

/** `__vm_map_new() -> map`。 */
Slot mapNew(NativeContext &ctx, const std::vector<Slot> &)
{
    return nativeNewObject(ctx, ObjectKind::Map);
}

/** `__vm_map_put(map, 键, 键是不是字符串, 键是不是对象句柄, 值, 值是不是字符串, 值是不是对象句柄) -> 0`。 */
Slot mapPut(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.put");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue key;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &key)) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[4], nativeInt(args[5]) != 0, nativeInt(args[6]), true,
                     &value)) return Slot::fromInt(0);
    const ObjectKey lookup = key.key();
    const auto found = object->entries.find(lookup);
    if (found != object->entries.end())
    {
        ctx.objects->releaseValue(found->second);
        found->second = std::move(value);
        return Slot::fromInt(0);
    }
    ObjectKey storedKey = lookup;
    if (storedKey.isObject || storedKey.isStruct)
    {
        std::string error;
        if (!ctx.objects->ownKey(&storedKey, &error))
        {
            ctx.error = error;
            ctx.objects->releaseValue(value);
            return Slot::fromInt(0);
        }
    }
    object->entries.emplace(std::move(storedKey), std::move(value));
    return Slot::fromInt(0);
}

/** `__vm_map_get(map, 键, 键是不是字符串, 键是不是对象句柄, 值是不是字符串, 值是不是对象句柄) -> 值`（没有这个键给"零值"）。 */
Slot mapGet(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.get");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue key;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &key)) return Slot::fromInt(0);
    const auto found = object->entries.find(key.key());
    if (found == object->entries.end())
    {
        /* 没有这个键：给"零"（数值 0 / 空串），拿 has() 判断在不在。 */
        return (nativeInt(args[4]) != 0)
                   ? Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, std::string())))
                   : Slot::fromInt(0);
    }
    Slot out;
    if (!nativeSlot(ctx, found->second, &out)) return Slot::fromInt(0);
    return out;
}

/** `__vm_map_has(map, 键, 键是不是字符串, 键是不是对象句柄) -> 0 / 1`。 */
Slot mapHas(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.has");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue key;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &key)) return Slot::fromInt(0);
    return Slot::fromInt(object->entries.count(key.key()) != 0u ? 1 : 0);
}

/** `__vm_map_remove(map, 键, 键是不是字符串, 键是不是对象句柄) -> 0 / 1`（在不在）。 */
Slot mapRemove(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.remove");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue key;
    if (!nativeValue(ctx, args[1], nativeInt(args[2]) != 0, nativeInt(args[3]), false,
                     &key)) return Slot::fromInt(0);
    const auto found = object->entries.find(key.key());
    if (found == object->entries.end()) return Slot::fromInt(0);
    ctx.objects->releaseKey(found->first);
    ctx.objects->releaseValue(found->second);
    object->entries.erase(found);
    return Slot::fromInt(1);
}

/** `__vm_map_len(map) -> 键值对个数`。 */
Slot mapLen(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "len(map)");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(static_cast<std::int64_t>(object->entries.size()));
}

/** `__vm_map_clear(map) -> 0`。 */
Slot mapClear(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.clear");
    if (object == nullptr) return Slot::fromInt(0);
    for (const auto &entry : object->entries)
    {
        ctx.objects->releaseKey(entry.first);
        ctx.objects->releaseValue(entry.second);
    }
    object->entries.clear();
    return Slot::fromInt(0);
}

/** `__vm_map_key_at(map, 第几个, 键是不是字符串, 键是不是对象句柄) -> 键`（按键的升序）。 */
Slot mapKeyAt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.key_at");
    if (object == nullptr) return Slot::fromInt(0);
    const std::int64_t wanted = nativeInt(args[1]);
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= object->entries.size())
    {
        ctx.error = "map.key_at 的位置越界：" + std::to_string(wanted) + "（键值对个数 " +
                    std::to_string(object->entries.size()) + "）";
        return Slot::fromInt(0);
    }
    auto it = object->entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(wanted));
    const auto fromKey = [&](const auto &self, const ObjectKey &source) -> ObjectValue {
        ObjectValue value;
        value.isStruct = source.isStruct;
        value.isText = source.isText;
        value.isFloat = source.isFloat;
        value.integer = source.ownsObject ? source.ownedInteger : source.integer;
        value.objectIdentity = source.isObject ? source.integer : 0;
        value.real = source.real;
        value.text = source.text;
        value.ownsObject = source.ownsObject;
        for (const ObjectKey &field : source.fields) value.fields.push_back(self(self, field));
        return value;
    };
    ObjectValue key = fromKey(fromKey, it->first);
    Slot out;
    if (!nativeSlot(ctx, key, &out)) return Slot::fromInt(0);
    return out;
}

/** `__vm_map_val_at(map, 第几个, 值是不是字符串, 值是不是对象句柄) -> 值`（按键的升序）。 */
Slot mapValAt(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Map, "map.val_at");
    if (object == nullptr) return Slot::fromInt(0);
    const std::int64_t wanted = nativeInt(args[1]);
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= object->entries.size())
    {
        ctx.error = "map.val_at 的位置越界：" + std::to_string(wanted) + "（键值对个数 " +
                    std::to_string(object->entries.size()) + "）";
        return Slot::fromInt(0);
    }
    auto it = object->entries.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(wanted));
    Slot out;
    if (!nativeSlot(ctx, it->second, &out)) return Slot::fromInt(0);
    return out;
}

/**
 * `__vm_object_copy(句柄) -> 新句柄`：**复制一份**对象（`bint` / 容器都一样）。
 *
 * 为什么需要：`bint a = b;` / `vec<int> x = y;` 这类写法如果只把句柄抄一份，两个变量
 * 就指向同一个对象 —— 谁被重新赋值（旧值要还回去）另一个就悬空了。所以对象类类型
 * 定成**值语义**：赋值 / 初始化都复制一份（和结构体一个脾气），别名从源头上没有。
 */
Slot objectCopy(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::int64_t handle = nativeInt(args[0]);
    if (ctx.objects == nullptr)
    {
        ctx.error = "不是一个能复制的对象句柄：" + std::to_string(handle);
        return Slot::fromInt(0);
    }
    std::string error;
    const std::int64_t copy = ctx.objects->copyHandle(handle, &error);
    if (copy == 0)
    {
        ctx.error = error;
        return Slot::fromInt(0);
    }
    return Slot::fromInt(copy);
}

/** `__vm_object_free(句柄) -> 0`：把对象还回去（`delete` 用；不是句柄就报错）。 */
Slot objectFree(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::int64_t handle = nativeInt(args[0]);
    if (ctx.objects == nullptr || !isObjectHandle(handle))
    {
        ctx.error = "不是一个能释放的对象句柄：" + std::to_string(handle);
        return Slot::fromInt(0);
    }
    if (!ctx.objects->release(handle))
    {
        ctx.error = "这个对象已经释放过了（句柄 " + std::to_string(handle) + "）";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(0);
}

Slot memoryLiveBytes(NativeContext &ctx, const std::vector<Slot> &)
{
    return Slot::fromInt(ctx.heap == nullptr ? 0 : static_cast<std::int64_t>(ctx.heap->liveBytes()));
}

Slot memoryLiveBlocks(NativeContext &ctx, const std::vector<Slot> &)
{
    return Slot::fromInt(ctx.heap == nullptr ? 0 : static_cast<std::int64_t>(ctx.heap->liveBlocks()));
}

Slot memoryPeakBytes(NativeContext &ctx, const std::vector<Slot> &)
{
    return Slot::fromInt(ctx.heap == nullptr ? 0 : static_cast<std::int64_t>(ctx.heap->peakBytes()));
}

Slot arenaCreate(NativeContext &ctx, const std::vector<Slot> &)
{
    return nativeNewObject(ctx, ObjectKind::Arena);
}

Slot arenaAllocate(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *arena = nativeObject(ctx, args[0], ObjectKind::Arena, "std::Arena.allocate");
    if (arena == nullptr || ctx.heap == nullptr) return Slot::fromInt(0);
    const std::int64_t requested = nativeInt(args[1]);
    if (requested < 0)
    {
        ctx.error = "std::Arena.allocate 的字节数不能为负";
        return Slot::fromInt(0);
    }
    if (requested == 0) return Slot::fromInt(0);
    const std::size_t address = ctx.heap->allocate(static_cast<std::size_t>(requested));
    if (address == 0u)
    {
        ctx.error = "std::Arena.allocate 分配失败";
        return Slot::fromInt(0);
    }
    arena->arenaBlocks.push_back(address);
    return Slot::fromInt(static_cast<std::int64_t>(address));
}

bool resetArena(NativeContext &ctx, RuntimeObject *arena)
{
    if (arena == nullptr || ctx.heap == nullptr) return false;
    for (const std::size_t address : arena->arenaBlocks)
    {
        std::string ignored;
        ctx.heap->release(address, &ignored);
    }
    arena->arenaBlocks.clear();
    return true;
}

Slot arenaReset(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *arena = nativeObject(ctx, args[0], ObjectKind::Arena, "std::Arena.reset");
    resetArena(ctx, arena);
    return Slot::fromInt(0);
}

Slot arenaDrop(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (ctx.objects == nullptr) return Slot::fromInt(0);
    const std::int64_t handle = nativeInt(args[0]);
    RuntimeObject *arena = ctx.objects->find(handle);
    if (arena == nullptr) return Slot::fromInt(0); /* 值副本重复析构时保持幂等。 */
    if (arena->kind != ObjectKind::Arena)
    {
        ctx.error = "std::Arena.drop 收到了错误种类的对象句柄";
        return Slot::fromInt(0);
    }
    resetArena(ctx, arena);
    ctx.objects->release(handle);
    return Slot::fromInt(0);
}

/* ---- 装箱格子（obj）----
 *
 * 盒子就是一个"值 + 类型号"：类型号由**前端在编译期**算好（`nameof(类型)` 给的也是它），
 * 所以 `obj.name()` 可以和 `nameof(...)` 直接比 —— 泛型分支就是这么写的。
 *
 * 它同时是**接口指针的运行时表示**：接口指针（`Shape mut*`）是"胖指针"，除了对象的
 * 地址还带着那个对象的具体类型号，调 `q.area()` 时正是靠它挑到具体实现的。
 */

/** `__vm_obj_new() -> obj`：造一个空格子（类型号 0 = 还没 set 过）。 */
Slot objNew(NativeContext &ctx, const std::vector<Slot> &)
{
    return nativeNewObject(ctx, ObjectKind::Obj);
}

/**
 * `__vm_obj_set(obj, 值, 类型号, 值是不是字符串, 值是不是对象句柄, 类型名地址) -> 0`：把值装进去。
 *
 * 原来的值直接丢掉（格子只有一个），类型号与类型名一起换成新的。
 */
Slot objSet(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Obj, "obj.set");
    if (object == nullptr) return Slot::fromInt(0);
    ObjectValue value;
    if (!nativeValue(ctx, args[1], nativeInt(args[3]) != 0, nativeInt(args[4]), false,
                     &value)) return Slot::fromInt(0);
    std::string name;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[5])), &name))
    {
        ctx.error = "obj.set 读不到类型名（地址 " + std::to_string(nativeInt(args[5])) + "）";
        return Slot::fromInt(0);
    }
    if (object->objOwnsObject) ctx.objects->release(object->objValue.integer);
    object->objTag = nativeInt(args[2]);
    object->objTypeName = std::move(name);
    object->objValue = std::move(value);
    object->objOwnsObject = nativeInt(args[4]) != 0;
    object->objValue.ownsObject = object->objOwnsObject;
    return Slot::fromInt(0);
}

/**
 * @brief `obj.get` / `obj.get` 取指针 共用的**类型校验**。
 *
 * 前端把"这个位置上允许出现的类型号"全部算好跟在后面（`args[4..]`）：
 * 目标是具体类型时就是它自己，目标是接口指针时是"所有实现了这个接口的结构体指针"。
 * 对不上就写 `ctx.error`（`run()` 会转成运行期错误）。
 *
 * @param ctx 环境。
 * @param object 盒子。
 * @param args 实参：`[1]` 要取的类型的名字地址、`[2]`/`[3]`/`[4]` 形状标志、`[5..]` 允许的类型号。
 * @param who 谁在取（诊断文案）。
 * @return 允许返回 true。
 */
bool objCheckTag(NativeContext &ctx, const RuntimeObject *object, const std::vector<Slot> &args,
                 const char *who)
{
    for (std::size_t index = 5u; index < args.size(); ++index)
    {
        if (nativeInt(args[index]) == object->objTag) return true;
    }
    std::string wanted = "?";
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &wanted)) wanted = "?";
    const std::string inside =
        object->objTypeName.empty()
            ? std::string("还没 set 过（类型号 0）")
            : object->objTypeName + "（类型号 " + std::to_string(object->objTag) + "）";
    ctx.error = std::string(who) + "：类型对不上 —— obj 里装的是 " + inside + "，这里要取 " +
                wanted + "。先 `b.name()` 问类型号、和 `nameof(类型)` 比一比再取";
    return false;
}

/**
 * `__vm_obj_get(obj, 要取的类型的名字地址, 是不是字符串, 是不是浮点, 是不是对象句柄, 允许的类型号…) -> 值`。
 *
 * 取出来的是**裸值**（整数 / 浮点 / 堆上的新串 / 指针的地址）。
 */
Slot objGet(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Obj, "obj.get");
    if (object == nullptr) return Slot::fromInt(0);
    if (!objCheckTag(ctx, object, args, "obj.get")) return Slot::fromInt(0);
    const ObjectValue &value = object->objValue;
    if (nativeInt(args[2]) != 0)
    {
        if (!value.isText)
        {
            ctx.error = "obj.get：obj 里装的不是字符串，取不出字符串";
            return Slot::fromInt(0);
        }
        return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, value.text)));
    }
    if (nativeInt(args[3]) != 0) return Slot::fromDouble(value.real);
    if (nativeInt(args[4]) != 0)
    {
        std::string error;
        const std::int64_t copy = ctx.objects->copyHandle(value.integer, &error);
        if (copy == 0)
        {
            ctx.error = error;
            return Slot::fromInt(0);
        }
        return Slot::fromInt(copy);
    }
    return Slot::fromInt(value.integer);
}

Slot objHasTag(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = ctx.objects->find(nativeInt(args[0]));
    if (object == nullptr || object->kind != ObjectKind::Obj) return Slot::fromInt(0);
    for (std::size_t index = 1u; index < args.size(); ++index)
        if (nativeInt(args[index]) == object->objTag) return Slot::fromInt(1);
    return Slot::fromInt(0);
}

/**
 * `__vm_obj_get_ptr(obj, 要取的类型的名字地址, 0, 0, 1, 允许的类型号…) -> 新的 obj（胖指针）`。
 *
 * 接口指针走这一条：校验过关后**复制一份胖指针**（值语义），交给变量持有 ——
 * 于是"两个接口指针"各拿各的类型号，谁先出块被释放都不会把对方弄悬空。
 */
Slot objGetPointer(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Obj, "obj.get");
    if (object == nullptr) return Slot::fromInt(0);
    if (!objCheckTag(ctx, object, args, "obj.get")) return Slot::fromInt(0);
    std::string error;
    std::shared_ptr<RuntimeObject> copy = ctx.objects->copyObject(*object, &error);
    if (!copy)
    {
        ctx.error = error;
        return Slot::fromInt(0);
    }
    return Slot::fromInt(ctx.objects->add(std::move(copy)));
}

/** `__vm_obj_name(obj) -> 类型号`：和 `nameof(类型)` 是同一个编号（空格子给 0）。 */
Slot objName(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Obj, "obj.name");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(object->objTag);
}

/** `__vm_obj_addr(obj) -> 地址`：胖指针里那个对象的地址（接口分派把它当 `self` 传下去）。 */
Slot objAddress(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeObject(ctx, args[0], ObjectKind::Obj, "接口指针");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(object->objValue.integer);
}

/**
 * `__vm_iface_fail(类型号, 接口.方法 名地址) -> 0`：`q.area()` 分派不到实现时的报错出口。
 *
 * 两种情形都落到这里：接口指针是空的（类型号 0），或者它指着的对象没实现这个接口。
 */
Slot ifaceFail(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string who = "接口方法";
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &who))
    {
        who = "接口方法";
    }
    const std::int64_t tag = nativeInt(args[0]);
    ctx.error = "接口分派失败：" + who + (tag == 0 ? " 的接口指针是空的（没指向任何对象）"
                                                   : " 没有类型号 " + std::to_string(tag) +
                                                         " 的实现");
    return Slot::fromInt(0);
}

RuntimeObject *nativeFile(NativeContext &ctx, const Slot &handle, const char *who)
{
    RuntimeObject *object = nativeObject(ctx, handle, ObjectKind::File, who);
    if (object == nullptr) return nullptr;
    if (!object->file || !object->file->isOpen())
    {
        ctx.error = std::string(who) + " 用到的文件已经关闭";
        return nullptr;
    }
    return object;
}

/** `__vm_file_open(路径, 模式) -> file`。模式同 C 的 fopen：`r` / `w` / `a` / `r+`。 */
Slot fileOpen(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string path;
    std::string mode;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &path))
    {
        ctx.error = "fopen 读不到路径字符串";
        return Slot::fromInt(0);
    }
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &mode))
    {
        ctx.error = "fopen 读不到模式字符串";
        return Slot::fromInt(0);
    }
    FileApi *const api = ctx.fileApi != nullptr ? ctx.fileApi : &defaultFileApi();
    std::string openError;
    std::shared_ptr<FileHandle> stream = api->open(path, mode, openError);
    if (!stream || !stream->isOpen())
    {
        ctx.error = openError.empty() ? "打不开文件：" + path : openError;
        return Slot::fromInt(0);
    }
    auto object = std::make_shared<RuntimeObject>();
    object->kind = ObjectKind::File;
    object->file = std::move(stream);
    object->filePath = path;
    return Slot::fromInt(ctx.objects->add(std::move(object)));
}

/** Result 包装层使用：打开失败只返回空句柄，不终止 VM。 */
Slot fileTryOpen(NativeContext &ctx, const std::vector<Slot> &args)
{
    const Slot result = fileOpen(ctx, args);
    ctx.error.clear();
    return result;
}

Slot fileValid(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = ctx.objects->find(nativeInt(args[0]));
    return Slot::fromInt(object != nullptr && object->kind == ObjectKind::File &&
                                 object->file && object->file->isOpen()
                             ? 1 : 0);
}

/** `__vm_file_read(file) -> string`：读下一行；EOF 返回空串。 */
Slot fileRead(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeFile(ctx, args[0], "fread");
    if (object == nullptr) return Slot::fromInt(0);
    std::string line;
    std::string error;
    if (!object->file->readLine(line, error))
    {
        ctx.error = error + "：" + object->filePath;
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, line)));
}

/** `__vm_file_write(file, text) -> bytes`。 */
Slot fileWrite(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeFile(ctx, args[0], "fwrite");
    if (object == nullptr) return Slot::fromInt(0);
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &text))
    {
        ctx.error = "fwrite 读不到要写入的字符串";
        return Slot::fromInt(0);
    }
    std::string error;
    if (!object->file->write(text, error))
    {
        ctx.error = error + "：" + object->filePath;
        return Slot::fromInt(0);
    }
    return Slot::fromInt(static_cast<std::int64_t>(text.size()));
}

RuntimeObject *fileForResult(NativeContext &ctx, const Slot &handle)
{
    RuntimeObject *object = ctx.objects->find(nativeInt(handle));
    if (object == nullptr || object->kind != ObjectKind::File)
        return nullptr;
    object->fileError.clear();
    object->fileSucceeded = false;
    if (!object->file || !object->file->isOpen())
    {
        object->fileError = "文件已经关闭";
        return object;
    }
    return object;
}

Slot fileTryRead(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = fileForResult(ctx, args[0]);
    if (object == nullptr || !object->file || !object->file->isOpen()) return Slot::fromInt(0);
    std::string line;
    std::string error;
    if (!object->file->readLine(line, error))
    {
        object->fileError = error.empty() ? "读取文件失败" : error;
        return Slot::fromInt(0);
    }
    object->fileSucceeded = true;
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, line)));
}

Slot fileTryWrite(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = fileForResult(ctx, args[0]);
    if (object == nullptr || !object->file || !object->file->isOpen()) return Slot::fromInt(-1);
    std::string text;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[1])), &text))
    {
        object->fileError = "读不到待写入字符串";
        ctx.error.clear();
        return Slot::fromInt(-1);
    }
    std::string error;
    if (!object->file->write(text, error))
    {
        object->fileError = error.empty() ? "写入文件失败" : error;
        return Slot::fromInt(-1);
    }
    object->fileSucceeded = true;
    return Slot::fromInt(static_cast<std::int64_t>(text.size()));
}

Slot fileTryFlush(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = fileForResult(ctx, args[0]);
    if (object == nullptr || !object->file || !object->file->isOpen()) return Slot::fromInt(-1);
    std::string error;
    if (!object->file->flush(error))
    {
        object->fileError = error.empty() ? "刷新文件失败" : error;
        return Slot::fromInt(-1);
    }
    object->fileSucceeded = true;
    return Slot::fromInt(0);
}

Slot fileTrySeek(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = fileForResult(ctx, args[0]);
    if (object == nullptr || !object->file || !object->file->isOpen()) return Slot::fromInt(-1);
    std::string error;
    std::int64_t position = -1;
    if (!object->file->seek(nativeInt(args[1]), static_cast<int>(nativeInt(args[2])),
                            position, error))
    {
        object->fileError = error.empty() ? "文件定位失败" : error;
        return Slot::fromInt(-1);
    }
    object->fileSucceeded = true;
    return Slot::fromInt(position);
}

Slot fileOperationSucceeded(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = ctx.objects->find(nativeInt(args[0]));
    return Slot::fromInt(object != nullptr && object->kind == ObjectKind::File &&
                                 object->fileSucceeded ? 1 : 0);
}

Slot fileLastError(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = ctx.objects->find(nativeInt(args[0]));
    const std::string error = object != nullptr && object->kind == ObjectKind::File
                                  ? object->fileError : "无效的 file 句柄";
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, error)));
}

Slot fileTryClose(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::int64_t handle = nativeInt(args[0]);
    RuntimeObject *object = ctx.objects->find(handle);
    if (object != nullptr && object->kind == ObjectKind::File)
        ctx.objects->release(handle);
    return Slot::fromInt(0);
}

Slot fileEof(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeFile(ctx, args[0], "feof");
    if (object == nullptr) return Slot::fromInt(0);
    return Slot::fromInt(object->file->eof() ? 1 : 0);
}

Slot fileClose(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeFile(ctx, args[0], "fclose");
    if (object == nullptr) return Slot::fromInt(0);
    ctx.objects->release(nativeInt(args[0]));
    return Slot::fromInt(0);
}

RuntimeObject *nativeProcess(NativeContext &ctx, const Slot &handle, const char *who)
{
    RuntimeObject *object = nativeObject(ctx, handle, ObjectKind::Process, who);
    if (object == nullptr) return nullptr;
    if (object->process == nullptr)
    {
        ctx.error = std::string(who) + " 用到的进程已经关闭";
        return nullptr;
    }
    return object;
}

/** `__vm_proc_run(命令) -> proc`：启动宿主命令并打开 stdout 管道。 */
Slot procRun(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string command;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &command))
    {
        ctx.error = "prun 读不到命令字符串";
        return Slot::fromInt(0);
    }
#ifdef _WIN32
    std::FILE *pipe = _popen(command.c_str(), "r");
#else
    std::FILE *pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr)
    {
        ctx.error = "启动命令失败：" + command;
        return Slot::fromInt(0);
    }
    auto object = std::make_shared<RuntimeObject>();
    object->kind = ObjectKind::Process;
    object->process = pipe;
    object->command = command;
    return Slot::fromInt(ctx.objects->add(std::move(object)));
}

/** `__vm_proc_read(proc) -> string`：读一行 stdout；EOF 返回空串。 */
Slot procRead(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeProcess(ctx, args[0], "pread");
    if (object == nullptr) return Slot::fromInt(0);
    std::string line;
    char buffer[512];
    if (std::fgets(buffer, sizeof(buffer), object->process) != nullptr)
    {
        line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    }
    return Slot::fromInt(static_cast<std::int64_t>(nativeAllocString(ctx, line)));
}

/** `__vm_proc_close(proc) -> exit_code`。 */
Slot procClose(NativeContext &ctx, const std::vector<Slot> &args)
{
    RuntimeObject *object = nativeProcess(ctx, args[0], "pclose");
    if (object == nullptr) return Slot::fromInt(0);
#ifdef _WIN32
    const int code = _pclose(object->process);
#else
    const int code = pclose(object->process);
#endif
    object->process = nullptr;
    return Slot::fromInt(code);
}

/** `__vm_proc_system(命令) -> exit_code`：一次性运行宿主命令。 */
Slot procSystem(NativeContext &ctx, const std::vector<Slot> &args)
{
    std::string command;
    if (!nativeReadCString(ctx, static_cast<std::size_t>(nativeInt(args[0])), &command))
    {
        ctx.error = "psystem 读不到命令字符串";
        return Slot::fromInt(0);
    }
    return Slot::fromInt(std::system(command.c_str()));
}

Slot programExit(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (ctx.exitRequested != nullptr) *ctx.exitRequested = true;
    if (ctx.exitCode != nullptr) *ctx.exitCode = static_cast<int>(nativeInt(args[0]));
    return Slot::fromInt(0);
}

Slot programAssert(NativeContext &ctx, const std::vector<Slot> &args)
{
    if (nativeInt(args[0]) == 0) ctx.error = "断言失败：assert(x) 的 x 为假";
    return Slot::fromInt(0);
}

Slot programPanic(NativeContext &ctx, const std::vector<Slot> &args)
{
    const std::size_t address = static_cast<std::size_t>(nativeInt(args[0]));
    std::string message;
    if (!nativeReadCString(ctx, address, &message))
    {
        ctx.error = "panic 读不到错误信息字符串（地址 " +
                    std::to_string(address) + "）";
        return Slot::fromInt(0);
    }
    ctx.error = "panic：" + message;
    return Slot::fromInt(0);
}

/** 名字 → 内部 C++ 原生函数。 */
struct SlotNativeEntry { const char *first; SlotNative second; };
struct SlotNativeTable {
    const SlotNativeEntry *begin_;
    std::size_t count_;
    const SlotNativeEntry *find(const std::string &name) const {
        for (std::size_t i = 0; i < count_; ++i) if (name == begin_[i].first) return begin_ + i;
        return nullptr;
    }
    const SlotNativeEntry *end() const { return nullptr; }
};
const SlotNativeEntry kSlotNativeEntries[] = {
    {"__vm_exit", {1, programExit}},
    {"__vm_assert", {1, programAssert}},
    {"__vm_panic", {1, programPanic}},
    {"__vm_str_lit", {1, strLit}},
    {"__vm_str_len", {1, strLen}},
    {"__vm_str_join", {-1, strJoin}},
    {"__vm_str_sub", {3, strSub}},
    {"__vm_str_of_int", {1, strOfInt}},
    {"__vm_str_of_flt", {2, strOfFlt}},
    {"__vm_str_cmp", {2, strCmp}},
    {"__vm_str_pos", {2, strPos}},
    {"__vm_str_at", {2, strAt}},
    {"__vm_str_set", {3, strSet}},
    {"__vm_read_text", {0, readText}},
    {"__vm_input_line", {0, readText}},
    {"__vm_pause", {0, pauseForDebug}},
    {"__vm_input_int", {0, inputInt}},
    {"__vm_input_uint", {0, inputUInt}},
    {"__vm_input_float", {0, inputFloat}},
    {"__vm_input_bool", {0, inputBool}},
    {"__vm_input_char", {0, inputChar}},
    {"__vm_memcpy", {3, memCopy}},
    {"__vm_memset", {3, memSet}},
    {"std::__parseInt64Error", {1, parseInt64Error}},
    {"std::__parseInt64Value", {1, parseInt64Value}},
    {"std::__parseUInt64Error", {1, parseUInt64Error}},
    {"std::__parseUInt64Value", {1, parseUInt64Value}},
    {"std::__parseFloat64Error", {1, parseFloat64Error}},
    {"std::__parseFloat64Value", {1, parseFloat64Value}},
    {"std::__parseBoolError", {1, parseBoolError}},
    {"std::__parseBoolValue", {1, parseBoolValue}},
    {"std::__pathJoin", {2, pathJoin}},
    {"std::__pathFileName", {1, pathFileName}},
    {"std::__pathExtension", {1, pathExtension}},
    {"std::__pathNormalize", {1, pathNormalize}},
    /* 大整数（bint）：后端是 library/big_number */
    {"__vm_bint_from_int", {1, bintFromInt}},
    {"__vm_bint_from_str", {1, bintFromStr}},
    {"__vm_bint_from_flt", {1, bintFromFloat}},
    {"__vm_bint_add", {2, bintAdd}},
    {"__vm_bint_sub", {2, bintSub}},
    {"__vm_bint_mul", {2, bintMul}},
    {"__vm_bint_div", {2, bintDiv}},
    {"__vm_bint_mod", {2, bintMod}},
    {"__vm_bint_neg", {1, bintNeg}},
    {"__vm_bint_cmp", {2, bintCmp}},
    {"__vm_bint_str", {1, bintToStr}},
    {"__vm_bint_to_int", {1, bintToInt}},
    {"__vm_bint_to_flt", {1, bintToFloat}},
    {"__vm_read_bint", {0, readBigInt}},
    {"__vm_input_bint", {0, readBigInt}},
    /* 任意精度十进制浮点（bfloat）：后端是 library/big_number */
    {"__vm_bfloat_from_int", {1, bfloatFromInt}},
    {"__vm_bfloat_from_flt", {1, bfloatFromFloat}},
    {"__vm_bfloat_from_str", {1, bfloatFromStr}},
    {"__vm_bfloat_from_bint", {1, bfloatFromBigInt}},
    {"__vm_bfloat_add", {2, bfloatAdd}},
    {"__vm_bfloat_sub", {2, bfloatSub}},
    {"__vm_bfloat_mul", {2, bfloatMul}},
    {"__vm_bfloat_div", {2, bfloatDiv}},
    {"__vm_bfloat_neg", {1, bfloatNeg}},
    {"__vm_bfloat_cmp", {2, bfloatCmp}},
    {"__vm_bfloat_str", {1, bfloatToStr}},
    {"__vm_bfloat_to_int", {1, bfloatToInt}},
    {"__vm_bfloat_to_flt", {1, bfloatToFloat}},
    {"__vm_bfloat_set_value_precision", {2, bfloatSetValuePrecision}},
    {"__vm_read_bfloat", {0, readBigFloat}},
    {"__vm_input_bfloat", {0, readBigFloat}},
    /* 容器：vec / set / map */
    {"__vm_program_arguments", {0, programArguments}},
    {"__vm_vec_new", {0, vecNew}},
    {"__vm_vec_pack", {-1, vecPack}},
    {"__vm_vec_push", {4, vecPush}},
    {"__vm_vec_insert", {5, vecInsert}},
    {"__vm_vec_at", {4, vecAt}},
    {"__vm_vec_set", {5, vecSet}},
    {"__vm_vec_remove", {2, vecRemove}},
    {"__vm_vec_pop", {3, vecPop}},
    {"__vm_vec_len", {1, vecLen}},
    {"__vm_vec_clear", {1, vecClear}},
    {"__vm_vec_stable_sort", {1, vecStableSort}},
    {"__vm_vec_sorted_copy", {1, vecSortedCopy}},
    {"__vm_vec_find", {4, vecFind}},
    {"__vm_vec_binary_search", {4, vecBinarySearch}},
    {"__vm_vec_min", {4, vecMin}},
    {"__vm_vec_max", {4, vecMax}},
    {"__vm_vec_clamp", {4, vecClamp}},
    {"__vm_vec_all", {2, vecAll}},
    {"__vm_vec_any", {2, vecAny}},
    {"__vm_vec_fold", {3, vecFold}},
    {"__vm_vec_map", {4, vecMap}},
    {"__vm_vec_filter", {4, vecFilter}},
    {"__vm_vec_reduce", {2, vecReduce}},
    {"__vm_vec_sum", {2, vecSum}},
    {"__vm_vec_count_if", {2, vecCountIf}},
    {"__vm_vec_count_value", {4, vecCountValue}},
    {"__vm_set_new", {0, setNew}},
    {"__vm_set_add", {4, setAdd}},
    {"__vm_set_has", {4, setHas}},
    {"__vm_set_remove", {4, setRemove}},
    {"__vm_set_at", {4, setAt}},
    {"__vm_set_len", {1, setLen}},
    {"__vm_set_clear", {1, setClear}},
    {"__vm_map_new", {0, mapNew}},
    {"__vm_map_put", {7, mapPut}},
    {"__vm_map_get", {6, mapGet}},
    {"__vm_map_has", {4, mapHas}},
    {"__vm_map_remove", {4, mapRemove}},
    {"__vm_map_len", {1, mapLen}},
    {"__vm_map_clear", {1, mapClear}},
    {"__vm_map_key_at", {4, mapKeyAt}},
    {"__vm_map_val_at", {4, mapValAt}},
    {"__vm_object_copy", {1, objectCopy}},
    {"__vm_object_free", {1, objectFree}},
    {"std::__memoryLiveBytes", {0, memoryLiveBytes}},
    {"std::__memoryLiveBlocks", {0, memoryLiveBlocks}},
    {"std::__memoryPeakBytes", {0, memoryPeakBytes}},
    {"std::__arenaCreate", {0, arenaCreate}},
    {"std::__arenaAllocate", {2, arenaAllocate}},
    {"std::__arenaReset", {1, arenaReset}},
    {"std::__arenaDrop", {1, arenaDrop}},
    /* 装箱格子（obj）：值 + 类型号；也是接口指针（胖指针）的运行时表示 */
    {"__vm_obj_new", {0, objNew}},
    {"__vm_obj_set", {6, objSet}},
    {"__vm_obj_get", {-1, objGet}},        /* 变参：后面跟一串"允许的类型号" */
    {"__vm_obj_has_tag", {-1, objHasTag}},
    {"__vm_obj_get_ptr", {-1, objGetPointer}},
    {"__vm_obj_name", {1, objName}},
    {"__vm_obj_addr", {1, objAddress}},
    {"__vm_iface_fail", {2, ifaceFail}},
    {"__vm_str_fmt", {-1, strFormat}},
    {"__vm_print", {-1, strPrint}},
    {"__vm_put_char", {1, putChar}},
    {"__vm_file_open", {2, fileOpen}},
    {"__vm_file_try_open", {2, fileTryOpen}},
    {"__vm_file_valid", {1, fileValid}},
    {"__vm_file_read", {1, fileRead}},
    {"__vm_file_write", {2, fileWrite}},
    {"__vm_file_eof", {1, fileEof}},
    {"__vm_file_close", {1, fileClose}},
    {"std::__fileTryRead", {1, fileTryRead}},
    {"std::__fileTryWrite", {2, fileTryWrite}},
    {"std::__fileTryFlush", {1, fileTryFlush}},
    {"std::__fileTrySeek", {3, fileTrySeek}},
    {"std::__fileSucceeded", {1, fileOperationSucceeded}},
    {"std::__fileLastError", {1, fileLastError}},
    {"std::__fileTryClose", {1, fileTryClose}},
    {"__vm_proc_run", {1, procRun}},
    {"__vm_proc_read", {1, procRead}},
    {"__vm_proc_close", {1, procClose}},
    {"__vm_proc_system", {1, procSystem}},
};
const SlotNativeTable kSlotNatives{kSlotNativeEntries, sizeof(kSlotNativeEntries) / sizeof(kSlotNativeEntries[0])};

} // namespace

// ---------------------------------------------------------------------------
//  四元式
// ---------------------------------------------------------------------------

std::string Quad::arg(std::size_t index) const
{
    if (index >= args.size()) return std::string();
    const std::string &value = args[index];
    return (value == "_") ? std::string() : value;
}

QuadFile parseQuads(std::string_view text, std::string label)
{
    QuadFile file;
    file.path = std::move(label);

    std::istringstream stream{std::string(text)};
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(stream, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        // 1. 注释：可能是元信息（名称 / 期望 / 输入）。
        if (trimmed[0] == '#')
        {
            const std::string body = trim(trimmed.substr(1u));
            const auto colon = body.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim(body.substr(0u, colon));
            const std::string value = trim(body.substr(colon + 1u));
            if (key == "名称") file.name = value;
            else if (key == "期望") file.expected = value;
            else if (key == "期望文本") file.expectedText = value;
            else if (key == "输入") file.input = value;
            continue;
        }

        // 2. 四元式：`N: (op, a, b, r)`。
        const auto open = trimmed.find('(');
        const auto close = trimmed.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open)
        {
            file.diagnostics.push_back(
                "第 " + std::to_string(lineNumber) + " 行：不是四元式（缺少括号）");
            continue;
        }

        std::vector<std::string> parts = splitArguments(trimmed.substr(open + 1u, close - open - 1u));
        if (parts.empty())
        {
            file.diagnostics.push_back(
                "第 " + std::to_string(lineNumber) + " 行：四元式是空的");
            continue;
        }

        Quad quad;
        quad.line = lineNumber;
        quad.op = parts[0];
        for (std::size_t index = 1u; index < parts.size(); ++index) quad.args.push_back(parts[index]);
        file.quads.push_back(std::move(quad));
    }
    return file;
}

QuadFile loadQuadFile(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        QuadFile file;
        file.path = path;
        file.diagnostics.push_back("打不开文件：" + path);
        return file;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return parseQuads(buffer.str(), path);
}

TypeInfo typeOf(std::string_view name)
{
    TypeInfo type;
    type.name = std::string(name);
    if (name == "i8") { type.bits = 8; type.size = 1; }
    else if (name == "u8") { type.bits = 8; type.size = 1; type.isUnsigned = true; }
    else if (name == "i16" || name.empty()) { type.bits = 16; type.size = 2; }
    else if (name == "u16") { type.bits = 16; type.size = 2; type.isUnsigned = true; }
    else if (name == "i32") { type.bits = 32; type.size = 4; }
    else if (name == "u32") { type.bits = 32; type.size = 4; type.isUnsigned = true; }
    else if (name == "i64") { type.bits = 64; type.size = 8; }
    else if (name == "u64") { type.bits = 64; type.size = 8; type.isUnsigned = true; }
    /* MAGEL 语言只写 float32 / float64；`f32` / `f64` 是**四元式层面**的旧名字，
     * 仓库里手写的 .quad 还在用，所以 VM 两个都认（VM 不是语言表面）。 */
    else if (name == "float32" || name == "f32") { type.bits = 32; type.size = 4; type.isFloat = true; }
    else if (name == "float64" || name == "f64") { type.bits = 64; type.size = 8; type.isFloat = true; }
    if (type.name.empty()) type.name = "i16";
    return type;
}

std::string Instruction::toString() const
{
    switch (op)
    {
    case Op::PushInt: return "PUSH " + std::to_string(intValue);
    case Op::PushDouble: return "PUSH " + formatDouble(doubleValue);
    case Op::PushText: return "PUSH \"" + textValue + "\"";
    case Op::Load: return "LOAD " + name;
    case Op::Store: return "STORE " + name;
    case Op::Declare: return "DECLARE " + name;
    case Op::Alloc: return "ALLOC";
    case Op::Free: return "FREE";
    case Op::Drop: return "DROP";
    case Op::Check: return "CHECK " + name;
    case Op::CheckPointer: return "CHECKPTR " + name + " stride " + std::to_string(intValue);
    case Op::CallIndirect:
        return name2.empty() ? std::string("CALLPTR") : ("CALLPTR -> " + name2);
    case Op::Neg: return "NEG";
    case Op::Not: return "NOT";
    case Op::Add: return "ADD";
    case Op::Sub: return "SUB";
    case Op::Mul: return "MUL";
    case Op::Div: return "DIV";
    case Op::Mod: return "MOD";
    case Op::Shl: return "SHL";
    case Op::Shr: return "SAR";
    case Op::ShrU: return "SHR";
    case Op::BitAnd: return "AND";
    case Op::BitOr: return "OR";
    case Op::BitXor: return "XOR";
    case Op::Eq: return "EQ";
    case Op::Ne: return "NE";
    case Op::Lt: return "LT";
    case Op::Le: return "LE";
    case Op::Gt: return "GT";
    case Op::Ge: return "GE";
    case Op::LtU: return "LTU";
    case Op::LeU: return "LEU";
    case Op::GtU: return "GTU";
    case Op::GeU: return "GEU";
    case Op::ToDouble: return "I2F";
    case Op::ToInt: return "F2I";
    case Op::MulWide: return "MULWIDE";
    case Op::DivWide: return "DIVWIDE";
    case Op::Jmp: return "JMP " + name;
    case Op::Jz: return "JZ " + name;
    case Op::Jnz: return "JNZ " + name;
    case Op::Print:
        switch (format)
        {
        case PrintFormat::Unsigned: return "PRINT u";
        case PrintFormat::Float: return "PRINT f";
        case PrintFormat::Text: return "PRINT text";
        default: return "PRINT";
        }
    case Op::PrintText: return "PRINT \"" + textValue + "\"";
    case Op::Read: return "READ " + name;
    case Op::Getc: return "GETC " + name;
    case Op::Putc: return "PUTC";
    case Op::Param: return "PARAM " + name;
    case Op::Call: return name2.empty() ? ("CALL " + name) : ("CALL " + name + " -> " + name2);
    case Op::Ret: return "RET";
    case Op::Halt: return "HALT";
    case Op::AddrOf: return "ADDR " + name;
    case Op::LoadAt: return "LOADAT";
    case Op::StoreAt: return "STOREAT";
    }
    return "?";
}

std::string SourceLocation::toString() const
{
    if (line > 0)
    {
        return file.empty() ? ("第 " + std::to_string(line) + " 行")
                            : (file + ":" + std::to_string(line));
    }
    if (quadLine > 0u)
    {
        return (file.empty() ? std::string("四元式") : file) + ":" + std::to_string(quadLine);
    }
    return function.empty() ? std::string("?") : function;
}

void DebugInfo::add(std::size_t instruction, SourceLocation location)
{
    if (!location.known()) return;
    if (!entries.empty())
    {
        const auto &last = entries.back();
        // 同一指令下标重复登记（函数标签本身不产生指令，"标签"与"紧跟它的第一条语句"
        // 会落在同一条指令上）：后来者更精确，覆盖掉。这样**每个下标最多一条**。
        if (last.first == instruction)
        {
            entries.back().second = std::move(location);
            return;
        }
        // 位置没变（一条语句的几十条指令）就不用再记，表因此保持很小。
        if (last.second.function == location.function && last.second.file == location.file &&
            last.second.line == location.line && last.second.quadLine == location.quadLine)
            return;
    }
    entries.emplace_back(instruction, std::move(location));
}

const SourceLocation *DebugInfo::lookup(std::size_t instruction) const
{
    if (entries.empty() || instruction < entries.front().first) return nullptr;
    std::size_t low = 0u;
    std::size_t high = entries.size();
    while (low + 1u < high)
    {
        const std::size_t middle = (low + high) / 2u;
        if (entries[middle].first <= instruction)
            low = middle;
        else
            high = middle;
    }
    return &entries[low].second;
}

// ---------------------------------------------------------------------------
//  组装
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief 组装期的临时状态。
 */
struct Assembler
{
    const QuadFile *file = nullptr;
    Program *program = nullptr;
    std::size_t quadLine = 0u;     ///< 当前正在组装的那条四元式在 `.quad` 里的行号。
    std::string function;          ///< 当前所在的函数 / 标签名。

    /**
     * @brief 追加一条指令，并顺手登记它的源码位置。
     *
     * 位置来自"当前正在组装的那条四元式"：行号是 `.quad` 的文本行、函数名是最近扫过的
     * `(label …)`。相同位置的连续指令由 `DebugInfo::add` 去重，表不会随指令数线性膨胀。
     *
     * @param instruction 指令。
     */
    void emit(Instruction instruction)
    {
        instruction.quadIndex = program->code.size();
        SourceLocation location;
        location.function = function;
        location.quadLine = quadLine;
        if (file != nullptr) location.file = file->path;
        program->debug.add(program->code.size(), std::move(location));
        program->code.push_back(std::move(instruction));
    }

    /**
     * @brief 只带操作码的指令（避免聚合初始化漏字段的警告）。
     * @param op 操作码。
     */
    void emitOp(Op op)
    {
        Instruction instruction;
        instruction.op = op;
        emit(std::move(instruction));
    }

    /**
     * @brief 把操作数压栈（立即数直接压，变量名走 `LOAD`）。
     * @param token 操作数原文。
     * @param index 来自第几条四元式。
     * @return 成功返回 true。
     */
    bool operand(const std::string &token, std::size_t index)
    {
        if (token.empty())
        {
            program->diagnostics.push_back("第 " + std::to_string(index + 1u) +
                                           " 条四元式：缺少操作数");
            return false;
        }

        // 1. 字符串字面量。
        if (token.front() == '"' || token.front() == '\'')
        {
            Instruction instruction;
            instruction.op = Op::PushText;
            instruction.textValue = unquote(token);
            emit(std::move(instruction));
            return true;
        }

        // 2. 数字。
        bool isFloat = false;
        std::int64_t intValue = 0;
        double doubleValue = 0.0;
        if (parseNumber(token, isFloat, intValue, doubleValue))
        {
            Instruction instruction;
            instruction.op = isFloat ? Op::PushDouble : Op::PushInt;
            instruction.intValue = intValue;
            instruction.doubleValue = doubleValue;
            emit(std::move(instruction));
            return true;
        }

        // 3. 变量。
        Instruction instruction;
        instruction.op = Op::Load;
        instruction.name = token;
        emit(std::move(instruction));
        return true;
    }

    /**
     * @brief 弹栈写回变量。
     * @param token 变量名。
     * @param index 来自第几条四元式。
     * @return 成功返回 true。
     */
    bool store(const std::string &token, std::size_t index)
    {
        if (token.empty())
        {
            program->diagnostics.push_back("第 " + std::to_string(index + 1u) +
                                           " 条四元式：缺少结果变量");
            return false;
        }
        Instruction instruction;
        instruction.op = Op::Store;
        instruction.name = token;
        emit(std::move(instruction));
        return true;
    }

    /**
     * @brief 打印格式：优先用后缀类型，其次看变量声明。
     * @param typeName 后缀类型名（可空）。
     * @param variable 变量名（可空）。
     * @return 格式。
     */
    PrintFormat formatFor(const std::string &typeName, const std::string &variable) const
    {
        TypeInfo type;
        if (!typeName.empty())
        {
            type = typeOf(typeName);
        }
        else if (!variable.empty())
        {
            const auto found = program->types.find(variable);
            type = (found == program->types.end()) ? typeOf("") : found->second;
        }
        else
        {
            type = typeOf("");
        }
        if (type.isFloat) return PrintFormat::Float;
        return type.isUnsigned ? PrintFormat::Unsigned : PrintFormat::Signed;
    }

    /**
     * @brief 比较指令：按类型选有符号 / 无符号版本。
     * @param base 比较符（`<` 等）。
     * @param type 类型。
     * @return 操作码。
     */
    static Op compareOp(const std::string &base, const TypeInfo &type)
    {
        if (base == "==") return Op::Eq;
        if (base == "!=") return Op::Ne;
        if (!type.isUnsigned)
        {
            if (base == "<") return Op::Lt;
            if (base == "<=") return Op::Le;
            if (base == ">") return Op::Gt;
            return Op::Ge;
        }
        if (base == "<") return Op::LtU;
        if (base == "<=") return Op::LeU;
        if (base == ">") return Op::GtU;
        return Op::GeU;
    }
};

/**
 * @brief 把 `op:type` 拆成基础操作符与类型。
 * @param op 原文。
 * @param typeName 输出类型名。
 * @return 基础操作符。
 */
std::string splitType(const std::string &op, std::string &typeName)
{
    // `:=` 自己就带冒号，不能按类型后缀切。
    if (op == ":=")
    {
        typeName.clear();
        return op;
    }
    const auto colon = op.find(':');
    if ((colon == std::string::npos) || (colon + 1u >= op.size()))
    {
        typeName.clear();
        return op;
    }
    typeName = op.substr(colon + 1u);
    return op.substr(0u, colon);
}

} // namespace

bool assemble(const QuadFile &file, Program &out)
{
    out = Program{};
    out.name = file.name;

    Assembler assembler;
    assembler.file = &file;
    assembler.program = &out;

    // 0. 先扫一遍"标签 → 形参名"：前端把形参声明排在函数标签**前面**
    //    （`(decl, T, local, a)` … `(label, _, _, f)`），所以紧挨着标签之前那一段
    //    连续的 decl 就是这个函数的形参、顺序就是实参顺序。函数指针的间接调用
    //    按位置绑定实参，靠的就是这张表。
    {
        std::vector<std::string> pending;
        for (const Quad &quad : file.quads)
        {
            std::string ignored;
            const std::string base = splitType(quad.op, ignored);
            if (base == "decl")
            {
                if (quad.arg(1) == "local") pending.push_back(quad.arg(2));
                continue;
            }
            if (base == "label")
            {
                if (!pending.empty()) out.parameters[quad.arg(2)] = pending;
                pending.clear();
                continue;
            }
            pending.clear();
        }
    }

    for (std::size_t index = 0; index < file.quads.size(); ++index)
    {
        const Quad &quad = file.quads[index];
        std::string typeName;
        const std::string base = splitType(quad.op, typeName);
        assembler.quadLine = quad.line;

        // 1. 声明：登记类型；`local` 作用域再发一条 `DECLARE`（到当前帧里开槽）。
        //    `global` 作用域不需要指令：全局槽由第一次写入按需创建。
        if (base == "decl")
        {
            out.types[quad.arg(2)] = typeOf(quad.arg(0));
            if (quad.arg(1) == "local")
            {
                Instruction instruction;
                instruction.op = Op::Declare;
                instruction.name = quad.arg(2);
                assembler.emit(std::move(instruction));
            }
            continue;
        }
        // 2. 标签：记下当前位置（也当作后续指令的"函数名"，调试信息要用）。
        if (base == "label")
        {
            out.labels[quad.arg(2)] = out.code.size();
            assembler.function = quad.arg(2);
            if (!quad.arg(2).empty())
            {
                SourceLocation location;
                location.function = quad.arg(2);
                location.quadLine = quad.line;
                location.file = file.path;
                out.debug.add(out.code.size(), std::move(location));
            }
            continue;
        }
        // 3. 停机。
        if (base == "halt")
        {
            assembler.emitOp(Op::Halt);
            continue;
        }

        // 4. 赋值与一元运算。
        if (base == ":=")
        {
            assembler.operand(quad.arg(0), index);
            assembler.store(quad.arg(2), index);
            continue;
        }
        if (base == "neg" || base == "~")
        {
            const TypeInfo type = typeName.empty() ? typeOf("") : typeOf(typeName);
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = (base == "neg") ? Op::Neg : Op::Not;
            instruction.intValue = type.bits;
            instruction.unsignedOp = type.isUnsigned;
            instruction.isFloat = type.isFloat;
            assembler.emit(std::move(instruction));
            assembler.store(quad.arg(2), index);
            continue;
        }

        // 5. 类型转换。
        if (base == "cvt")
        {
            const TypeInfo type = typeOf(typeName);
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = type.isFloat ? Op::ToDouble : Op::ToInt;
            instruction.intValue = type.bits;
            instruction.unsignedOp = type.isUnsigned;
            instruction.isFloat = type.isFloat;
            assembler.emit(std::move(instruction));
            assembler.store(quad.arg(2), index);
            continue;
        }

        // 6. 算术 / 位运算。
        static const std::map<std::string, Op> kBinary = {
            {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Mod},
            {"&", Op::BitAnd}, {"|", Op::BitOr}, {"^", Op::BitXor},
        };
        const auto binary = kBinary.find(base);
        if (binary != kBinary.end())
        {
            const TypeInfo type = typeName.empty() ? typeOf("") : typeOf(typeName);
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            Instruction instruction;
            instruction.op = binary->second;
            instruction.intValue = type.bits;
            instruction.unsignedOp = type.isUnsigned;
            instruction.isFloat = type.isFloat;
            assembler.emit(std::move(instruction));
            assembler.store(quad.arg(2), index);
            continue;
        }
        if (base == "<<" || base == ">>")
        {
            const TypeInfo type = typeOf(typeName);
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            Op op = Op::Shl;
            if (base == ">>") op = type.isUnsigned ? Op::ShrU : Op::Shr;
            assembler.emitOp(op);
            assembler.store(quad.arg(2), index);
            continue;
        }

        // 7. 比较。
        if (base == "==" || base == "!=" || base == "<" || base == "<=" ||
            base == ">" || base == ">=")
        {
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            assembler.emitOp(Assembler::compareOp(base, typeOf(typeName)));
            assembler.store(quad.arg(2), index);
            continue;
        }

        // 8. 双结果乘除：`(mul:u32, a, b, lo, hi)` / `(div:u32, a, b, q, r)`。
        if (base == "mul" || base == "div")
        {
            // 双字乘除按类型位宽切高低半 / 商余，所以把位宽与符号带进指令。
            TypeInfo wideType = typeName.empty() ? typeOf("") : typeOf(typeName);
            if (typeName.empty())
            {
                const auto declared = out.types.find(quad.arg(0));
                if (declared != out.types.end()) wideType = declared->second;
            }
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            Instruction wide;
            wide.op = (base == "mul") ? Op::MulWide : Op::DivWide;
            wide.intValue = wideType.bits;
            wide.unsignedOp = wideType.isUnsigned;
            assembler.emit(std::move(wide));
            assembler.store(quad.arg(3), index); // 先弹 hi / r
            assembler.store(quad.arg(2), index); // 再弹 lo / q
            continue;
        }

        // 9. 跳转。
        if (base == "j" || base == "jz" || base == "jnz")
        {
            Instruction instruction;
            if (base != "j") assembler.operand(quad.arg(0), index);
            instruction.op = (base == "j") ? Op::Jmp : ((base == "jz") ? Op::Jz : Op::Jnz);
            instruction.name = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }

        // 10. 输入输出。
        if (base == "write")
        {
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = Op::Print;
            instruction.format = assembler.formatFor(typeName, quad.arg(0));
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "print")
        {
            Instruction instruction;
            instruction.op = Op::PrintText;
            instruction.textValue = unquote(quad.arg(0));
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "putc")
        {
            assembler.operand(quad.arg(0), index);
            assembler.emitOp(Op::Putc);
            continue;
        }
        if (base == "read" || base == "getc")
        {
            Instruction instruction;
            instruction.op = (base == "read") ? Op::Read : Op::Getc;
            instruction.name = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }

        // 11. 调用与返回。
        if (base == "param")
        {
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = Op::Param;
            instruction.name = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "call")
        {
            Instruction instruction;
            instruction.op = Op::Call;
            instruction.name = quad.arg(0);
            instruction.name2 = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "ret")
        {
            assembler.operand(quad.arg(0), index);
            assembler.emitOp(Op::Ret);
            continue;
        }

        // 11.5 数据区三件套：`(addr, 符号, _, t)` / `(load, 指针, _, 目标)` / `(store, 指针, 值, _)`。
        if (base == "addr")
        {
            Instruction instruction;
            instruction.op = Op::AddrOf;
            instruction.name = quad.arg(0);
            assembler.emit(std::move(instruction));
            assembler.store(quad.arg(2), index);
            continue;
        }
        if (base == "alloc")
        {
            // `(alloc, 字节数, _, 目标)`：弹栈取字节数，把拿到的地址压栈再由 `store` 写目标。
            assembler.operand(quad.arg(0), index);
            assembler.emitOp(Op::Alloc);
            assembler.store(quad.arg(2), index);
            continue;
        }
        if (base == "free")
        {
            // `(free, 地址, _, _)`：把地址压栈，`Op::Free` 弹出来还回堆里。
            assembler.operand(quad.arg(0), index);
            assembler.emitOp(Op::Free);
            continue;
        }
        if (base == "drop")
        {
            // `(drop, 地址, _, _)`：软释放（作用域出口用；地址不是活跃堆块就跳过）。
            assembler.operand(quad.arg(0), index);
            assembler.emitOp(Op::Drop);
            continue;
        }
        if (base == "check")
        {
            // `(check, 下标, 上限, 标签)`：下标越界就终止运行（上限可以是常量也可以是变量）。
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            Instruction instruction;
            instruction.op = Op::Check;
            instruction.name = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "checkptr")
        {
            // `(checkptr, 下标, 基址, 步长[, 标签])`：按"基址所在那一块"的容量查下标。
            assembler.operand(quad.arg(0), index);
            assembler.operand(quad.arg(1), index);
            std::int64_t stride = 8;
            if (!quad.arg(2).empty()) stride = std::strtoll(quad.arg(2).c_str(), nullptr, 10);
            Instruction instruction;
            instruction.op = Op::CheckPointer;
            instruction.intValue = stride;
            instruction.name = quad.arg(3);
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "callptr")
        {
            // `(callptr, 函数指针, _, 结果)`：目标是个**值**（不是标签名），所以它的
            // 地址先压栈，`Op::CallIndirect` 弹出来去数据区读标签。
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = Op::CallIndirect;
            instruction.name2 = quad.arg(2);
            assembler.emit(std::move(instruction));
            continue;
        }
        if (base == "load")
        {
            assembler.operand(quad.arg(0), index);
            Instruction instruction;
            instruction.op = Op::LoadAt;
            // `(load:f64, 指针, _, 目标)`：浮点按 IEEE-754 双精度解释这 8 个字节
            // （数据区每个槽位就是 8 字节，整型与浮点共用同一片内存）。
            instruction.isFloat = typeOf(typeName).isFloat;
            assembler.emit(std::move(instruction));
            assembler.store(quad.arg(2), index);
            continue;
        }
        if (base == "store")
        {
            assembler.operand(quad.arg(0), index); // 指针
            assembler.operand(quad.arg(1), index); // 值
            Instruction instruction;
            instruction.op = Op::StoreAt;
            instruction.isFloat = typeOf(typeName).isFloat;
            assembler.emit(std::move(instruction));
            continue;
        }

        // 12. 其它：明确报出来，而不是悄悄跳过。
        out.diagnostics.push_back("第 " + std::to_string(index + 1u) +
                                  " 条四元式：本虚拟机不支持操作 '" + quad.op + "'");
    }

    // 13. 收尾：没有 Halt 就补一条，避免"掉出程序尾"。
    if (out.code.empty() || out.code.back().op != Op::Halt)
    {
        Instruction halt;
        halt.op = Op::Halt;
        out.code.push_back(std::move(halt));
    }
    /* 裸四元式没有可见性语法，保持历史行为：它的标签默认都是公开入口。
     * 具备模块语义的前端可在 assemble 后收窄这张表。 */
    for (const auto &[label, ignored] : out.labels)
    {
        (void)ignored;
        out.exports.insert(label);
    }
    return out.ok();
}

// ---------------------------------------------------------------------------
//  执行
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief 读一个字面量。
 * @param token 原文。
 * @param isNumber 输出：是不是数字。
 * @param isFloat 输出：是不是浮点。
 * @param intValue 输出：整数值。
 * @param doubleValue 输出：浮点值。
 * @param text 输出：字符串内容。
 * @return 是字面量返回 true。
 */
bool readLiteral(const std::string &token, bool &isNumber, bool &isFloat,
                 std::int64_t &intValue, double &doubleValue, std::string &text)
{
    if (token.empty() || token == "_") return false;
    if (token.front() == '"' || token.front() == '\'')
    {
        isNumber = false;
        text = unquote(token);
        return true;
    }
    return parseNumber(token, isFloat, intValue, doubleValue) ? (isNumber = true, true) : false;
}

/**
 * @brief 一条四元式读到的变量（用于死代码分析）。
 * @param quad 四元式。
 * @return 变量名列表（不含字面量）。
 */
std::vector<std::string> readOperands(const Quad &quad)
{
    std::string typeName;
    const std::string base = splitType(quad.op, typeName);
    std::vector<std::string> names;

    /**
     * 收一个可能读到的操作数。
     * @param token 原文。
     */
    auto take = [&](const std::string &token) {
        if (token.empty()) return;
        bool isNumber = false, isFloat = false;
        std::int64_t intValue = 0;
        double doubleValue = 0.0;
        std::string text;
        if (readLiteral(token, isNumber, isFloat, intValue, doubleValue, text)) return;
        names.push_back(token);
    };

    if (base == ":=" || base == "neg" || base == "~" || base == "cvt" ||
        base == "write" || base == "putc" || base == "param" || base == "ret" ||
        base == "jz" || base == "jnz")
    {
        take(quad.arg(0));
        return names;
    }
    if (base == "mul" || base == "div")
    {
        take(quad.arg(0));
        take(quad.arg(1));
        return names;
    }
    // 二元运算与比较：前两个操作数。
    take(quad.arg(0));
    take(quad.arg(1));
    return names;
}

/**
 * @brief 这条四元式会不会写变量（写的是第几个参数）。
 * @param base 基础操作符。
 * @return 写入的参数下标；不写返回 -1。
 */
int writesTo(const std::string &base)
{
    if (base == ":=" || base == "neg" || base == "~" || base == "cvt" ||
        base == "==" || base == "!=" || base == "<" || base == "<=" ||
        base == ">" || base == ">=" || base == "mul" || base == "div" ||
        base == "read" || base == "getc")
    {
        return 2;
    }
    if (base == "+" || base == "-" || base == "*" || base == "/" || base == "%" ||
        base == "&" || base == "|" || base == "^" || base == "<<" || base == ">>")
    {
        return 2;
    }
    return -1;
}

} // namespace

OptimizationReport optimize(QuadFile &file, std::size_t maxRounds)
{
    OptimizationReport report;

    // 0. 先收集变量类型，折叠时按类型回绕。
    std::map<std::string, TypeInfo> types;
    for (const Quad &quad : file.quads)
    {
        std::string typeName;
        if (splitType(quad.op, typeName) == "decl") types[quad.arg(2)] = typeOf(quad.arg(0));
    }

    for (std::size_t round = 0; round < maxRounds; ++round)
    {
        ++report.rounds;
        bool changed = false;
        std::map<std::string, std::string> constants; // 变量 → 已知字面量原文
        std::vector<Quad> passOne;
        passOne.reserve(file.quads.size());

        for (Quad quad : file.quads)
        {
            std::string typeName;
            const std::string base = splitType(quad.op, typeName);
            const TypeInfo type = typeName.empty()
                                      ? (types.count(quad.arg(2)) != 0u ? types[quad.arg(2)] : typeOf(""))
                                      : typeOf(typeName);

            /**
             * 把第 index 个操作数换成已知常量。
             */
            auto substitute = [&](std::size_t index) {
                if (index >= quad.args.size()) return;
                const std::string token = quad.args[index];
                if (token.empty() || token == "_") return;
                if (token.front() == '"' || token.front() == '\'') return;
                const auto found = constants.find(token);
                if (found != constants.end()) quad.args[index] = found->second;
            };

            // 1. 标签 / 调用会打断常量传播；写入变量的指令清掉该变量的常量。
            if (base == "label" || base == "call" || base == "halt")
            {
                constants.clear();
                passOne.push_back(std::move(quad));
                continue;
            }
            if (base == "read" || base == "getc")
            {
                constants.erase(quad.arg(2));
                passOne.push_back(std::move(quad));
                continue;
            }

            // 2. 代入已知常量。
            substitute(0);
            substitute(1);

            // 3. 常量折叠：两个操作数（或一元的一个）都是字面量。
            bool folded = false;
            {
                bool leftIsNumber = false, leftIsFloat = false;
                std::int64_t leftInt = 0;
                double leftDouble = 0.0;
                std::string leftText;
                bool rightIsNumber = false, rightIsFloat = false;
                std::int64_t rightInt = 0;
                double rightDouble = 0.0;
                std::string rightText;

                const bool hasLeft = readLiteral(quad.arg(0), leftIsNumber, leftIsFloat,
                                                 leftInt, leftDouble, leftText);
                const bool hasRight = readLiteral(quad.arg(1), rightIsNumber, rightIsFloat,
                                                  rightInt, rightDouble, rightText);

                static const std::map<std::string, char> kArithmetic = {
                    {"+", '+'}, {"-", '-'}, {"*", '*'}, {"/", '/'}, {"%", '%'},
                };
                const auto arithmetic = kArithmetic.find(base);
                const bool isCompare = (base == "==" || base == "!=" || base == "<" ||
                                        base == "<=" || base == ">" || base == ">=");
                const bool isBitwise = (base == "&" || base == "|" || base == "^" ||
                                        base == "<<" || base == ">>");

                if (arithmetic != kArithmetic.end() && hasLeft && hasRight && leftIsNumber &&
                    rightIsNumber)
                {
                    const bool useFloat = type.isFloat || leftIsFloat || rightIsFloat;
                    const double a = leftIsFloat ? leftDouble : static_cast<double>(leftInt);
                    const double b = rightIsFloat ? rightDouble : static_cast<double>(rightInt);
                    if (useFloat)
                    {
                        if ((arithmetic->second != '/' && arithmetic->second != '%') || b != 0.0)
                        {
                            double value = 0.0;
                            switch (arithmetic->second)
                            {
                            case '+': value = a + b; break;
                            case '-': value = a - b; break;
                            case '*': value = a * b; break;
                            case '/': value = a / b; break;
                            default: value = std::fmod(a, b); break;
                            }
                            value = normalizeFloat(value, type.bits);
                            quad.op = ":=";
                            quad.args = {formatFloatLiteral(value), "_", quad.arg(2)};
                            constants[quad.arg(2)] = quad.args[0];
                            folded = true;
                        }
                    }
                    else if ((arithmetic->second != '/' && arithmetic->second != '%') ||
                             (rightInt != 0 && !(leftInt == INT64_MIN && rightInt == -1)))
                    {
                        std::int64_t value = 0;
                        if (type.isUnsigned &&
                            (arithmetic->second == '/' || arithmetic->second == '%'))
                        {
                            const std::uint64_t left = static_cast<std::uint64_t>(leftInt);
                            const std::uint64_t right = static_cast<std::uint64_t>(rightInt);
                            value = static_cast<std::int64_t>(arithmetic->second == '/'
                                                                  ? left / right : left % right);
                        }
                        else
                        {
                            switch (arithmetic->second)
                            {
                            case '+': value = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(leftInt) +
                                static_cast<std::uint64_t>(rightInt)); break;
                            case '-': value = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(leftInt) -
                                static_cast<std::uint64_t>(rightInt)); break;
                            case '*': value = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(leftInt) *
                                static_cast<std::uint64_t>(rightInt)); break;
                            case '/': value = leftInt / rightInt; break;
                            default: value = leftInt % rightInt; break;
                            }
                        }
                        value = normalizeInt(value, type);
                        quad.op = ":=";
                        quad.args = {std::to_string(value), "_", quad.arg(2)};
                        constants[quad.arg(2)] = quad.args[0];
                        folded = true;
                    }
                }
                else if (isCompare && hasLeft && hasRight && leftIsNumber && rightIsNumber)
                {
                    const double a = leftIsFloat ? leftDouble : static_cast<double>(leftInt);
                    const double b = rightIsFloat ? rightDouble : static_cast<double>(rightInt);
                    bool value = false;
                    if (type.isUnsigned && !leftIsFloat && !rightIsFloat)
                    {
                        const std::uint64_t ua = static_cast<std::uint64_t>(normalizeInt(leftInt, type));
                        const std::uint64_t ub = static_cast<std::uint64_t>(normalizeInt(rightInt, type));
                        if (base == "==") value = (ua == ub);
                        else if (base == "!=") value = (ua != ub);
                        else if (base == "<") value = (ua < ub);
                        else if (base == "<=") value = (ua <= ub);
                        else if (base == ">") value = (ua > ub);
                        else value = (ua >= ub);
                    }
                    else
                    {
                        if (base == "==") value = (a == b);
                        else if (base == "!=") value = (a != b);
                        else if (base == "<") value = (a < b);
                        else if (base == "<=") value = (a <= b);
                        else if (base == ">") value = (a > b);
                        else value = (a >= b);
                    }
                    quad.op = ":=";
                    quad.args = {value ? "1" : "0", "_", quad.arg(2)};
                    constants[quad.arg(2)] = quad.args[0];
                    folded = true;
                }
                else if (isBitwise && hasLeft && hasRight && leftIsNumber && rightIsNumber &&
                         !leftIsFloat && !rightIsFloat)
                {
                    std::int64_t value = 0;
                    if (base == "&") value = leftInt & rightInt;
                    else if (base == "|") value = leftInt | rightInt;
                    else if (base == "^") value = leftInt ^ rightInt;
                    else if (base == "<<") value = leftInt << (rightInt & 63);
                    else value = type.isUnsigned
                                     ? static_cast<std::int64_t>(
                                           static_cast<std::uint64_t>(leftInt) >> (rightInt & 63))
                                     : (leftInt >> (rightInt & 63));
                    value = normalizeInt(value, type);
                    quad.op = ":=";
                    quad.args = {std::to_string(value), "_", quad.arg(2)};
                    constants[quad.arg(2)] = quad.args[0];
                    folded = true;
                }
                else if (base == "neg" && hasLeft && leftIsNumber && leftIsFloat)
                {
                    // 浮点取负：必须按 double 折，`std::atoll("2.5")` 会先截成 2 再取负。
                    quad.op = ":=";
                    quad.args = {formatFloatLiteral(normalizeFloat(-leftDouble, type.bits)),
                                 "_", quad.arg(2)};
                    constants[quad.arg(2)] = quad.args[0];
                    folded = true;
                }
                else if ((base == "neg" || base == "~") && hasLeft && leftIsNumber && !leftIsFloat)
                {
                    std::int64_t value = (base == "neg") ? -leftInt : ~leftInt;
                    value = normalizeInt(value, type);
                    quad.op = ":=";
                    quad.args = {std::to_string(value), "_", quad.arg(2)};
                    constants[quad.arg(2)] = quad.args[0];
                    folded = true;
                }
                else if (base == "cvt" && hasLeft && leftIsNumber)
                {
                    if (type.isFloat)
                    {
                        const double value = normalizeFloat(
                            leftIsFloat ? leftDouble : static_cast<double>(leftInt), type.bits);
                        quad.op = ":=";
                        quad.args = {formatFloatLiteral(value), "_", quad.arg(2)};
                    }
                    else
                    {
                        const std::int64_t value = leftIsFloat
                            ? static_cast<std::int64_t>(leftDouble) : leftInt;
                        quad.op = ":=";
                        quad.args = {std::to_string(normalizeInt(
                                         value, type)),
                                     "_", quad.arg(2)};
                    }
                    constants[quad.arg(2)] = quad.args[0];
                    folded = true;
                }
            }
            if (folded)
            {
                ++report.constantsFolded;
                changed = true;
                report.trace.push_back("常量折叠：" + quad.args[2] + " = " + quad.args[0]);
                passOne.push_back(std::move(quad));
                continue;
            }

            // 4. 代数化简：`x+0`、`x*1`、`x<<0` 这类直接变成一次赋值。
            {
                const std::string left = quad.arg(0);
                const std::string right = quad.arg(1);
                std::string keep;
                bool simplified = false;
                bool constantZero = false;

                if ((base == "+" || base == "-" || base == "|" || base == "^") && right == "0")
                {
                    keep = left;
                    simplified = true;
                }
                else if (base == "+" && left == "0")
                {
                    keep = right;
                    simplified = true;
                }
                else if ((base == "*" || base == "/" || base == "<<") && right == "1")
                {
                    keep = left;
                    simplified = true;
                }
                else if (base == "*" && left == "1")
                {
                    keep = right;
                    simplified = true;
                }
                else if ((base == "<<" || base == ">>") && right == "0")
                {
                    keep = left;
                    simplified = true;
                }
                else if (base == "*" && (left == "0" || right == "0"))
                {
                    keep = "0";
                    simplified = true;
                    constantZero = true;
                }

                if (simplified)
                {
                    ++report.algebraicSimplified;
                    changed = true;
                    report.trace.push_back("代数化简：" + quad.op + " " + left + ", " + right +
                                           " → " + keep);
                    quad.op = ":=";
                    quad.args = {keep, "_", quad.arg(2)};
                    if (keep == "0" && constantZero) constants[quad.arg(2)] = "0";
                    else if (keep != quad.arg(2) && constants.count(keep) != 0u)
                    {
                        constants[quad.arg(2)] = constants[keep];
                    }
                    else
                    {
                        constants.erase(quad.arg(2));
                    }
                    passOne.push_back(std::move(quad));
                    continue;
                }
            }

            // 5. 常量条件的跳转：`jz` / `jnz` 直接折成 `j` 或删掉。
            if (base == "jz" || base == "jnz")
            {
                bool isNumber = false, isFloat = false;
                std::int64_t intValue = 0;
                double doubleValue = 0.0;
                std::string text;
                if (readLiteral(quad.arg(0), isNumber, isFloat, intValue, doubleValue, text) &&
                    isNumber)
                {
                    const bool zero = (isFloat ? doubleValue == 0.0 : intValue == 0);
                    const bool take = (base == "jz") ? zero : !zero;
                    ++report.deadBranches;
                    changed = true;
                    if (take)
                    {
                        report.trace.push_back("死分支：" + quad.op + " 条件恒真 → j " + quad.arg(2));
                        quad.op = "j";
                        quad.args = {"_", "_", quad.arg(2)};
                    }
                    else
                    {
                        report.trace.push_back("死分支：" + quad.op + " 条件恒假 → 删除");
                        continue; // 整条丢掉
                    }
                }
            }

            // 6. 其它写入：更新常量表（`:=` 记录，其它类型的写入清掉）。
            const int written = writesTo(base);
            if (written >= 0)
            {
                const std::string name = quad.arg(static_cast<std::size_t>(written));
                if (!name.empty())
                {
                    if (base == ":=")
                    {
                        bool isNumber = false, isFloat = false;
                        std::int64_t intValue = 0;
                        double doubleValue = 0.0;
                        std::string text;
                        if (readLiteral(quad.arg(0), isNumber, isFloat, intValue, doubleValue, text))
                        {
                            constants[name] = quad.arg(0);
                        }
                        else
                        {
                            constants.erase(name);
                        }
                    }
                    else
                    {
                        constants.erase(name);
                    }
                }
            }
            passOne.push_back(std::move(quad));
        }

        // 7. 不可达代码：`j` / `halt` / `ret` 之后到下一个标签之间删掉；紧跟 `j` 的标签
        //    让这条 `j` 变成多余的。
        std::vector<Quad> reachable;
        reachable.reserve(passOne.size());
        for (std::size_t index = 0; index < passOne.size(); ++index)
        {
            std::string typeName;
            const std::string base = splitType(passOne[index].op, typeName);

            // 7.1 跳到紧邻的下一条标签 → 这条跳转没用。
            if (base == "j" && (index + 1u < passOne.size()))
            {
                std::string nextType;
                // 注意要比较名字：`j loop` 后面跟着 `label done` 时这条跳转是有用的（回边）。
                if ((splitType(passOne[index + 1u].op, nextType) == "label") &&
                    (passOne[index + 1u].arg(2) == passOne[index].arg(2)))
                {
                    ++report.deadCode;
                    changed = true;
                    continue;
                }
            }
            reachable.push_back(passOne[index]);

            // 7.2 无条件跳转 / 停机 / 返回之后，直到下一个标签都不可达。
            if (base == "j" || base == "halt" || base == "ret")
            {
                while ((index + 1u < passOne.size()))
                {
                    std::string nextType;
                    if (splitType(passOne[index + 1u].op, nextType) == "label") break;
                    ++index;
                    // `decl` 是**类型元信息**，不是可执行指令：它常常排在函数标签之前
                    // （`simple_ast` 生成的形参声明就是 `(decl …)` 在 `(label …)` 前面，
                    // 前面还隔着引导用的 `(halt …)`）。删了它，`program.types` 就少了
                    // 形参的类型，调用时的实参归一化与 `write` 的格式都会跟着错。
                    if (splitType(passOne[index].op, nextType) == "decl")
                    {
                        reachable.push_back(passOne[index]);
                        continue;
                    }
                    ++report.deadCode;
                    changed = true;
                }
            }
        }

        // 8. 死代码：结果变量从没被读过的 `:=` 删掉。
        std::map<std::string, std::size_t> readCount;
        for (const Quad &quad : reachable)
        {
            for (const std::string &name : readOperands(quad)) ++readCount[name];
        }
        std::vector<Quad> final;
        final.reserve(reachable.size());
        for (const Quad &quad : reachable)
        {
            std::string typeName;
            const std::string base = splitType(quad.op, typeName);
            if (base == ":=")
            {
                const std::string result = quad.arg(2);
                if (!result.empty() && readCount[result] == 0u)
                {
                    ++report.deadCode;
                    changed = true;
                    continue;
                }
            }
            final.push_back(quad);
        }

        file.quads = std::move(final);
        if (!changed) break;
    }

    return report;
}

void Machine::setBreakpoints(std::vector<std::size_t> instructions)
{
    breakpoints_.clear();
    for (const std::size_t instruction : instructions) breakpoints_.insert(instruction);
}

RunResult Machine::run(const Program &program, const std::vector<std::string> &input,
                       std::size_t stepLimit)
{
    RunResult result;
    // 本次运行生效的调试表：调用方临时挂的优先，其次是程序自带的那张。
    debugInfo_ = debugOverride_ != nullptr
                     ? debugOverride_
                     : (program.debug.empty() ? nullptr : &program.debug);
    currentLocation_ = SourceLocation{};
    debugFrames_.clear();
    if (timeCallback_)
    {
        result.timingAvailable = true;
        result.startedNanoseconds = timeCallback_();
    }
    std::vector<Slot> stack;
    std::map<std::string, Slot> globals;
    // 调用帧栈：每一帧都是堆上单独的一块（进函数分配、出函数释放），
    // 递归的每层因此各有各的局部变量存储。
    std::vector<std::unique_ptr<Frame>> frames;
    std::vector<PendingArg> pending;
    // 堆：紧跟在数据区后面长（没有数据区时从地址 1 开始，0 留给空指针）。
    const std::size_t heapBase = (data_ == nullptr || data_->size() == 0u)
                                     ? kRuntimeCellBytes
                                     : data_->size();
    Heap defaultHeap;
    HeapAllocator &heap = heapAllocator_ == nullptr ? static_cast<HeapAllocator &>(defaultHeap)
                                                    : *heapAllocator_;
    heap.reset(heapBase);
    std::size_t inputIndex = 0;
    int bigFloatPrecision = 34;
    bool exitRequested = false;
    int exitCode = 0;
    // 运行期对象表：`bint`（大整数）与 vec / set / map 三种容器的"值"就是这里的句柄。
    // 跟着这次 run 走，一条 `(drop, 句柄, _, _)` 或 `(free, 句柄, _, _)` 就能还回去。
    ObjectStore objects;
    std::size_t pc = 0;
    bool jumped = false;   ///< 本步是否改过 pc（跳转 / 调用 / 返回）
    std::vector<std::string> numbers; // write 的数值：多个值之间一个空格
    std::vector<CoroutineContext> coroutines(1u);
    std::size_t currentCoroutine = 0u;
    coroutines[0].state = CoroutineContext::State::Running;

    auto saveCoroutine = [&](std::size_t resumePc, CoroutineContext::State state) {
        CoroutineContext &context = coroutines[currentCoroutine];
        context.stack = std::move(stack);
        context.frames = std::move(frames);
        context.pending = std::move(pending);
        context.pc = resumePc;
        context.state = state;
    };
    auto loadCoroutine = [&](std::size_t index) {
        currentCoroutine = index;
        CoroutineContext &context = coroutines[index];
        stack = std::move(context.stack);
        frames = std::move(context.frames);
        pending = std::move(context.pending);
        pc = context.pc;
        context.state = CoroutineContext::State::Running;
        jumped = true;
    };
    auto nextRunnable = [&](std::size_t after) -> std::size_t {
        if (coroutines.size() <= 1u) return coroutines.size();
        for (std::size_t offset = 1u; offset <= coroutines.size(); ++offset)
        {
            const std::size_t index = (after + offset) % coroutines.size();
            const CoroutineContext::State state = coroutines[index].state;
            if (state == CoroutineContext::State::Ready ||
                state == CoroutineContext::State::Suspended)
                return index;
        }
        return coroutines.size();
    };

    /**
     * @brief 地址空间：低地址是数据区（编译期布局），再往后是堆。
     *
     * `(load, 地址, …)` / `(store, 地址, …)` 不区分对象在哪一段，按地址落位。
     * @param address 地址。
     * @return 落在堆区返回 true，否则当数据区。
     */
    auto inHeap = [&](std::size_t address) { return heap.contains(address); };

    /**
     * @brief 按地址读 8 个字节。
     * @param address 地址。
     * @param isFloat 是否按 IEEE-754 双精度解释。
     * @param out 结果。
     * @return 成功返回 true。
     */
    auto loadMemory = [&](std::size_t address, bool isFloat, Slot &out) -> bool {
        if (inHeap(address))
        {
            if (isFloat)
            {
                double real = 0.0;
                if (!heap.readDouble(address, real)) return false;
                out = Slot::fromDouble(real);
                return true;
            }
            std::int64_t integer = 0;
            if (!heap.readInt(address, integer)) return false;
            out = Slot::fromInt(integer);
            return true;
        }
        if (data_ == nullptr) return false;
        if (isFloat)
        {
            double real = 0.0;
            if (!data_->readDouble(address, real)) return false;
            out = Slot::fromDouble(real);
            return true;
        }
        std::int64_t integer = 0;
        if (!data_->readInt(address, integer)) return false;
        out = Slot::fromInt(integer);
        return true;
    };

    /**
     * @brief 按地址写 8 个字节。
     * @param address 地址。
     * @param isFloat 是否按 IEEE-754 双精度写。
     * @param value 值。
     * @return 成功返回 true。
     */
    auto storeMemory = [&](std::size_t address, bool isFloat, const Slot &value) -> bool {
        if (inHeap(address))
        {
            return isFloat ? heap.writeDouble(address, value.number())
                           : heap.writeInt(address, static_cast<std::int64_t>(value.number()));
        }
        if (data_ == nullptr) return false;
        return isFloat ? data_->writeDouble(address, value.number())
                       : data_->writeInt(address, static_cast<std::int64_t>(value.number()));
    };

    /**
     * 追加一个数值输出（与 transfer 的示例套件口径一致）。
     */
    auto emitNumber = [&](std::string text) {
        if (!numbers.empty()) result.output += ' ';
        numbers.push_back(std::move(text));
        result.output += numbers.back();
    };

    /**
     * 找变量槽：先看当前帧的局部，再看全局。
     */
    auto findSlot = [&](const std::string &name) -> Slot * {
        if (!frames.empty())
        {
            const auto local = frames.back()->locals.find(name);
            if (local != frames.back()->locals.end()) return &local->second;
        }
        const auto global = globals.find(name);
        return (global == globals.end()) ? nullptr : &global->second;
    };

    /**
     * 写变量：局部优先（存在就写局部），否则写全局。
     */
    auto assign = [&](const std::string &name, const Slot &value) {
        const auto type = program.types.find(name);
        Slot stored = value;
        // 只有"声明过类型"的变量才回绕 / 转换：没有 decl 的临时量保留原本的整数 / 浮点形态，
        // 否则 `(+:f64, d, 2.25, t1)` 里的 t1 会被当成 i16 截断。
        if (!stored.isText && (type != program.types.end()))
        {
            const TypeInfo info = type->second;
            if (info.isFloat) stored = Slot::fromDouble(normalizeFloat(stored.number(), info.bits));
            else stored = Slot::fromInt(normalizeInt(
                stored.isFloat ? static_cast<std::int64_t>(stored.number()) : stored.integer,
                info));
        }
        if (!frames.empty())
        {
            auto local = frames.back()->locals.find(name);
            if (local != frames.back()->locals.end())
            {
                local->second = stored;
                return;
            }
        }
        globals[name] = stored;
    };

    /**
     * @brief 把 VM 的调用帧栈翻译成"源码级调用栈"。
     *
     * 调用者的位置取该帧的**返回地址**（就是调用点后面那条指令），当前帧用 `at`。
     * 没有调试信息时直接给空表 —— 调试是"挂了才有开销"的。
     */
    auto debugFramesOf = [&](std::size_t at) {
        std::vector<DebugFrame> snapshot;
        if (debugInfo_ == nullptr) return snapshot;
        snapshot.reserve(frames.size() + 1u);
        for (const auto &frame : frames)
        {
            DebugFrame entry;
            if (frame)
            {
                /* 帧里存的是**返回地址**（调用指令的下一条），往回退一条才是调用点本身。 */
                const std::size_t callSite = frame->returnIp > 0u ? frame->returnIp - 1u : 0u;
                if (const SourceLocation *caller = debugInfo_->lookup(callSite))
                {
                    entry.function = caller->function;
                    entry.location = *caller;
                }
            }
            snapshot.push_back(std::move(entry));
        }
        DebugFrame current;
        if (const SourceLocation *where = debugInfo_->lookup(at)) current.location = *where;
        current.function = current.location.function;
        snapshot.push_back(std::move(current));
        return snapshot;
    };

    auto fail = [&](const std::string &message) {
        result.ok = false;
        result.error = message;
        if (result.sourceTrace.empty() && debugInfo_ != nullptr)
        {
            if (const SourceLocation *where = debugInfo_->lookup(pc)) result.failureLocation = *where;
            result.sourceTrace = debugFramesOf(pc);
            currentLocation_ = result.failureLocation;
            debugFrames_ = result.sourceTrace;
        }
    };

    /**
     * @brief 进一层函数：绑实参 → 在堆上开一帧 → 跳过去。直接调用与间接调用共用。
     *
     * `positional = false`（直接调用）：实参按**调用点写着的形参名**绑进新帧
     * （前端知道 callee 的形参名，逐个写出来了）。
     * `positional = true`（函数指针）：调用点不知道会调到谁，所以按**位置**绑 ——
     * 表来自 `program.parameters`（组装时从"函数标签前面那段 decl"扫出来的）。
     *
     * @param label 目标函数标签。
     * @param returnDest 返回值写回哪个变量（可空）。
     * @param positional 是否按位置绑实参。
     * @return 成功返回 true。
     */
    auto enterFunction = [&](const std::string &label, const std::string &returnDest,
                             bool positional) -> bool {
        const auto target = program.labels.find(label);
        if (target == program.labels.end())
        {
            fail("函数入口不存在（也不在原生函数表里）：" + label);
            pending.clear();
            return false;
        }
        // 进一层函数 = 在堆上开一帧自己的局部变量存储（等价于 malloc）。
        auto frame = std::make_unique<Frame>();
        frame->returnIp = pc + 1u;
        frame->returnDest = returnDest;

        if (positional)
        {
            const auto names = program.parameters.find(label);
            // 零参数函数不产生 decl，旧字节码的形参表因此没有该入口。
            const std::size_t arity = names == program.parameters.end() ? 0u : names->second.size();
            if (arity != pending.size())
            {
                fail("通过函数指针调用 " + label + " 需要 " +
                     std::to_string(arity) + " 个参数，实到 " +
                     std::to_string(pending.size()) + " 个");
                pending.clear();
                return false;
            }
            for (std::size_t slot = 0; slot < pending.size(); ++slot)
            {
                frame->locals[names->second[slot]] = pending[slot].value;
            }
            pending.clear();
        }
        else
        {
            // 形参按名字绑定进新帧（类型在 callee 的 decl 里声明过）。
            for (const PendingArg &argument : pending)
            {
                const auto type = program.types.find(argument.name);
                Slot value = argument.value;
                if (type != program.types.end() && !value.isText)
                {
                    if (type->second.isFloat)
                        value = Slot::fromDouble(normalizeFloat(value.number(), type->second.bits));
                    else value = Slot::fromInt(normalizeInt(
                             value.isFloat ? static_cast<std::int64_t>(value.number())
                                           : value.integer, type->second));
                }
                frame->locals[argument.name] = value;
            }
            pending.clear();
        }
        frames.push_back(std::move(frame));
        pc = target->second;
        jumped = true;
        return true;
    };

    std::function<bool()> executeOne;
    std::function<bool(const std::string &, const std::string &)> callNative;
    NativeCallContextImpl::Invoker invokeFunction;
    std::size_t nativeCallbackDepth = 0u;

    invokeFunction = [&](std::int64_t functionPointer,
                         const std::vector<NativeValue> &arguments,
                         NativeValue &returnValue) -> bool {
        if (functionPointer == 0 || data_ == nullptr)
        {
            fail(functionPointer == 0 ? "胶水层调用了空函数指针"
                                      : "没有加载数据区，胶水层无法解析函数指针");
            return false;
        }
        const std::string label = data_->readCString(static_cast<std::size_t>(functionPointer));
        if (label.empty())
        {
            fail("胶水层函数指针没有指向有效函数名：" + std::to_string(functionPointer));
            return false;
        }

        const std::size_t callerPc = pc;
        const bool callerJumped = jumped;
        const std::size_t callerFrames = frames.size();
        const std::size_t callerStack = stack.size();
        std::vector<PendingArg> callerPending = std::move(pending);
        pending.clear();
        for (std::size_t index = 0; index < arguments.size(); ++index)
        {
            const NativeValue &argument = arguments[index];
            if (argument.isText())
            {
                fail("胶水层调用 MAGEL 函数时，文本参数必须先由 Codec<string> 编码");
                break;
            }
            pending.push_back(PendingArg{"__magel_p" + std::to_string(index),
                                         argument.isFloat()
                                             ? Slot::fromDouble(argument.floating)
                                             : Slot::fromInt(argument.integer)});
        }

        const std::string destination = "__magel_native_callback_" +
                                        std::to_string(result.steps) + "_" +
                                        std::to_string(callerFrames);
        ++nativeCallbackDepth;
        bool entered = result.error.empty();
        if (entered && callNative(label, destination))
        {
            entered = result.error.empty();
        }
        else if (entered)
        {
            entered = enterFunction(label, destination, true);
            while (entered && result.error.empty() && frames.size() > callerFrames)
            {
                if (!executeOne()) break;
            }
            if (entered && result.error.empty() && frames.size() != callerFrames)
            {
                fail("胶水层调用的 MAGEL 函数没有正常 return");
                entered = false;
            }
        }

        const auto returned = globals.find(destination);
        if (entered && result.error.empty() && returned != globals.end())
        {
            returnValue = returned->second.isFloat
                              ? NativeValue::fromFloat(returned->second.real)
                              : NativeValue::fromInt(returned->second.integer);
        }
        else if (entered && result.error.empty())
        {
            returnValue = NativeValue::fromInt(0);
        }
        globals.erase(destination);
        if (stack.size() > callerStack) stack.resize(callerStack);
        pending = std::move(callerPending);
        pc = callerPc;
        jumped = callerJumped;
        --nativeCallbackDepth;
        return entered && result.error.empty();
    };

    /** 调用 VM 内部运行时或宿主注册表；不建普通 MAGEL 调用帧。 */
    callNative = [&](const std::string &name, const std::string &returnDest) -> bool {
        const bool coroutineCreate = name == "lang::SharedCoroutine::create";
        const bool coroutineWait = name == "lang::SharedCoroutine::wait";
        const bool coroutineYield = name == "lang::SharedCoroutine::yield" ||
                                    name == "lang::SharedCoroutine::yelid";
        const bool coroutineSend = name == "lang::SharedCoroutine::send";
        const bool coroutineReceive = name == "lang::SharedCoroutine::receive";
        const bool coroutineIdentity = name == "lang::SharedCoroutine::current" ||
                                       name == "lang::SharedCoroutine::parent";
        if (coroutineCreate || coroutineWait || coroutineYield || coroutineSend ||
            coroutineReceive || coroutineIdentity)
        {
            if (nativeCallbackDepth != 0u)
            {
                fail("同步胶水回调中不能创建、等待或切换协程");
                pending.clear();
                return true;
            }
            const std::size_t wanted = coroutineSend ? 2u :
                (coroutineYield || coroutineReceive || coroutineIdentity ? 0u : 1u);
            if (pending.size() != wanted)
            {
                fail("协程函数 " + name + " 需要 " + std::to_string(wanted) +
                     " 个参数，实到 " + std::to_string(pending.size()) + " 个");
                pending.clear();
                return true;
            }
            if (coroutineCreate)
            {
                const std::size_t address = static_cast<std::size_t>(pending[0].value.number());
                pending.clear();
                if (address == 0u || data_ == nullptr)
                {
                    fail("lang::SharedCoroutine::create 收到了空函数指针");
                    return true;
                }
                const std::string label = data_->readCString(address);
                const auto target = program.labels.find(label);
                if (label.empty() || target == program.labels.end())
                {
                    fail("lang::SharedCoroutine::create 找不到协程入口：" + label);
                    return true;
                }
                CoroutineContext context;
                context.pc = target->second;
                context.state = CoroutineContext::State::Ready;
                context.parent = currentCoroutine;
                context.frames.push_back(std::make_unique<Frame>());
                coroutines.push_back(std::move(context));
                if (!returnDest.empty())
                    assign(returnDest, Slot::fromInt(static_cast<std::int64_t>(coroutines.size() - 1u)));
                return true;
            }

            if (coroutineIdentity)
            {
                pending.clear();
                const std::size_t id = name == "lang::SharedCoroutine::current"
                                           ? currentCoroutine : coroutines[currentCoroutine].parent;
                if (!returnDest.empty()) assign(returnDest, Slot::fromInt(static_cast<std::int64_t>(id)));
                return true;
            }

            if (coroutineSend)
            {
                const std::int64_t raw = pending[0].value.integer;
                NativeContext context;
                context.heap = &heap;
                context.data = data_;
                std::string message;
                const bool validText = nativeReadCString(context,
                    static_cast<std::size_t>(pending[1].value.integer), &message);
                pending.clear();
                if (raw < 0 || static_cast<std::size_t>(raw) >= coroutines.size() ||
                    coroutines[static_cast<std::size_t>(raw)].state == CoroutineContext::State::Done ||
                    !validText)
                {
                    fail("lang::SharedCoroutine::send 收到了无效句柄或字符串");
                    return true;
                }
                CoroutineContext &target = coroutines[static_cast<std::size_t>(raw)];
                target.inbox.push_back(std::move(message));
                if (target.state == CoroutineContext::State::Receiving)
                    target.state = CoroutineContext::State::Ready;
                return true;
            }

            if (coroutineReceive)
            {
                pending.clear();
                CoroutineContext &self = coroutines[currentCoroutine];
                if (self.inbox.empty())
                {
                    const std::size_t next = nextRunnable(currentCoroutine);
                    if (next == coroutines.size())
                    {
                        fail("协程接收消息形成死锁");
                        return true;
                    }
                    saveCoroutine(pc, CoroutineContext::State::Receiving);
                    loadCoroutine(next);
                    return true;
                }
                std::string message = std::move(self.inbox.front());
                self.inbox.erase(self.inbox.begin());
                NativeContext context;
                context.heap = &heap;
                const std::size_t address = nativeAllocString(context, message);
                if (address == 0u)
                {
                    fail(context.error);
                    return true;
                }
                if (!returnDest.empty()) assign(returnDest, Slot::fromInt(static_cast<std::int64_t>(address)));
                return true;
            }

            if (coroutineWait)
            {
                const std::int64_t raw = pending[0].value.integer;
                pending.clear();
                if (raw <= 0 || static_cast<std::size_t>(raw) >= coroutines.size())
                {
                    fail("lang::SharedCoroutine::wait 收到了无效句柄：" + std::to_string(raw));
                    return true;
                }
                const std::size_t target = static_cast<std::size_t>(raw);
                if (target == currentCoroutine)
                {
                    fail("协程不能等待自己");
                    return true;
                }
                if (!returnDest.empty()) assign(returnDest, Slot::fromInt(0));
                if (coroutines[target].state == CoroutineContext::State::Done) return true;
                coroutines[currentCoroutine].waitingFor = target;
                saveCoroutine(pc + 1u, CoroutineContext::State::Waiting);
                const CoroutineContext::State targetState = coroutines[target].state;
                const std::size_t next =
                    (targetState == CoroutineContext::State::Ready ||
                     targetState == CoroutineContext::State::Suspended)
                        ? target
                        : nextRunnable(currentCoroutine);
                if (next == coroutines.size())
                {
                    fail("协程等待形成死锁");
                    loadCoroutine(currentCoroutine);
                    return true;
                }
                loadCoroutine(next);
                return true;
            }

            pending.clear();
            const std::size_t next = nextRunnable(currentCoroutine);
            if (next == coroutines.size())
            {
                if (!returnDest.empty()) assign(returnDest, Slot::fromInt(0));
                return true;
            }
            if (!returnDest.empty()) assign(returnDest, Slot::fromInt(0));
            saveCoroutine(pc + 1u, CoroutineContext::State::Suspended);
            loadCoroutine(next);
            return true;
        }
        /* 内部 C++ 运行时（字符串 / 容器）：参数与结果都按 Slot 走。 */
        const auto internal = kSlotNatives.find(name);
        if (internal == kSlotNatives.end())
        {
            const NativeRegistry::Entry *external =
                nativeRegistry_ == nullptr ? nullptr : nativeRegistry_->find(name);
            if (external == nullptr) return false;
            if (pending.size() != external->arity)
            {
                fail("宿主函数 " + name + " 需要 " + std::to_string(external->arity) +
                     " 个参数，实到 " + std::to_string(pending.size()) + " 个");
                pending.clear();
                return true;
            }
            std::vector<NativeValue> arguments;
            arguments.reserve(pending.size());
            for (const PendingArg &argument : pending)
                arguments.push_back(argument.value.isFloat
                                        ? NativeValue::fromFloat(argument.value.real)
                                        : NativeValue::fromInt(argument.value.integer));
            pending.clear();

            NativeContext runtime;
            runtime.heap = &heap;
            runtime.data = data_;
            runtime.text = &result.text;
            runtime.input = &input;
            runtime.inputIndex = &inputIndex;
            runtime.programArguments = &programArguments_;
            runtime.objects = &objects;
            runtime.bigFloatPrecision = &bigFloatPrecision;
            runtime.exitRequested = &exitRequested;
            runtime.exitCode = &exitCode;
            NativeCallContextImpl context(runtime, invokeFunction);
            NativeValue value;
            try { value = external->callback(context, arguments); }
            catch (const std::exception &exception)
            { runtime.error = "宿主函数 " + name + " 抛出异常：" + exception.what(); }
            catch (...) { runtime.error = "宿主函数 " + name + " 抛出未知异常"; }
            if (!runtime.error.empty()) { fail(runtime.error); return true; }
            if (!returnDest.empty())
            {
                if (value.isText())
                {
                    const std::size_t address = nativeAllocString(runtime, value.text);
                    if (!runtime.error.empty()) { fail(runtime.error); return true; }
                    assign(returnDest, Slot::fromInt(static_cast<std::int64_t>(address)));
                }
                else assign(returnDest, value.isFloat() ? Slot::fromDouble(value.floating)
                                                        : Slot::fromInt(value.integer));
            }
            return true;
        }
            if (internal->second.arity >= 0 &&
                pending.size() != static_cast<std::size_t>(internal->second.arity))
            {
                fail("原生函数 " + name + " 需要 " + std::to_string(internal->second.arity) +
                     " 个参数，实到 " + std::to_string(pending.size()) + " 个");
                pending.clear();
                return true;   // 已经报错了，算"处理过"
            }
            std::vector<Slot> arguments;
            arguments.reserve(pending.size());
            for (const PendingArg &argument : pending) arguments.push_back(argument.value);
            pending.clear();

            NativeContext context;
            context.heap = &heap;
            context.data = data_;
            context.text = &result.text;   /* printf 和 putc / print 共用这条文本流 */
            context.input = &input;
            context.inputIndex = &inputIndex;
            context.programArguments = &programArguments_;
            context.invoker = &invokeFunction;
            context.objects = &objects;
            context.bigFloatPrecision = &bigFloatPrecision;
            context.exitRequested = &exitRequested;
            context.exitCode = &exitCode;
            context.fileApi = fileApi_;
            const Slot value = internal->second.apply(context, arguments);
            if (!context.error.empty())
            {
                fail(context.error);
                return true;
            }
            if (exitRequested)
            {
                result.ok = true;
                result.exitCode = exitCode;
                pc = program.code.size();
                jumped = true;
                return true;
            }
            if (trace_ && !value.isFloat && isObjectHandle(value.integer))
                result.trace.push_back("    => object " + std::to_string(value.integer));
            if (!returnDest.empty()) assign(returnDest, value);
            return true;
    };

    executeOne = [&]() -> bool {
        if (pc >= program.code.size()) return false;
        if (stepLimit != 0u && result.steps >= stepLimit)
        {
            fail("指令条数超过上限（" + std::to_string(stepLimit) + "），可能是死循环");
            return false;
        }
        const Instruction &instruction = program.code[pc];
        const std::size_t executedPc = pc;
        const std::size_t executedCoroutine = currentCoroutine;

        // 调试：断点命中或单步模式下，**先回调再执行**（所以断点看到的是"即将执行的那条"）。
        // 没挂调试回调时"停下"没有意义，直接跳过，普通运行零开销。
        if (debugHook_ != nullptr && (singleStep_ || breakpoints_.count(pc) != 0u))
        {
            DebugEvent event;
            event.instruction = pc;
            event.steps = result.steps;
            event.coroutine = currentCoroutine;
            event.opcode = instruction.toString();
            if (debugInfo_ != nullptr)
            {
                if (const SourceLocation *where = debugInfo_->lookup(pc)) event.location = *where;
            }
            event.breakpoint = !singleStep_ || breakpoints_.count(pc) != 0u;
            event.frames = debugFramesOf(pc);
            currentLocation_ = event.location;
            debugFrames_ = event.frames;
            const DebugAction action = debugHook_(event);
            if (action == DebugAction::Abort)
            {
                fail("调试器中止在 " + event.location.toString() + "（第 " +
                     std::to_string(pc) + " 条指令）");
                return false;
            }
            singleStep_ = action == DebugAction::StepInto;
        }
        ++result.steps;
        result.maxStackDepth = std::max(result.maxStackDepth, stack.size());
        if (trace_)
        {
            result.trace.push_back("[" + std::to_string(pc) + "] " + instruction.toString());
        }

        // 所有压栈统一经过容量检查；上限按协程分别计算。
        auto push = [&](Slot value) -> bool {
            if (stackLimit_ != 0u && stack.size() >= stackLimit_)
            {
                fail("求值栈超过上限（" + std::to_string(stackLimit_) +
                     " 个槽，第 " + std::to_string(pc) + " 条指令 " +
                     instruction.toString() + "）");
                return false;
            }
            stack.push_back(std::move(value));
            result.maxStackDepth = std::max(result.maxStackDepth, stack.size());
            return true;
        };

        // 需要弹栈的指令统一在这里取。
        auto pop = [&](Slot &out) -> bool {
            if (stack.empty())
            {
                fail("求值栈为空（第 " + std::to_string(pc) + " 条指令 " +
                     instruction.toString() + "）");
                return false;
            }
            out = stack.back();
            stack.pop_back();
            return true;
        };
        jumped = false;
        switch (instruction.op)
        {
        case Op::PushInt: push(Slot::fromInt(instruction.intValue)); break;
        case Op::PushDouble: push(Slot::fromDouble(instruction.doubleValue)); break;
        case Op::PushText: push(Slot::fromText(instruction.textValue)); break;
        case Op::Load:
        {
            const Slot *slot = findSlot(instruction.name);
            if (slot == nullptr)
            {
                fail("读未定义的变量：" + instruction.name);
                break;
            }
            push(*slot);
            break;
        }
        case Op::Store:
        {
            Slot slot;
            if (!pop(slot)) break;
            assign(instruction.name, slot);
            break;
        }
        case Op::Declare:
        {
            // `(decl, T, local, x)`：在当前调用帧的**本帧存储**里占一个槽
            // （进这层函数时刚分配的那一块，初值 0，浮点按浮点）。
            // **已经有就不动**：形参 / `self` 是调用时绑进来的，同一个 `decl` 也可能
            // 被循环再执行一遍，重新清零会把它们抹掉。
            // 没有帧（顶层代码）时落到全局槽，与"函数外的局部变量就是全局"一致。
            std::map<std::string, Slot> &scope =
                frames.empty() ? globals : frames.back()->locals;
            if (scope.find(instruction.name) == scope.end())
            {
                const auto type = program.types.find(instruction.name);
                const bool isFloat = (type != program.types.end()) && type->second.isFloat;
                scope[instruction.name] = isFloat ? Slot::fromDouble(0.0) : Slot::fromInt(0);
            }
            break;
        }
        case Op::Neg:
        {
            Slot slot;
            if (!pop(slot)) break;
            if (slot.isFloat || instruction.isFloat)
                push(Slot::fromDouble(
                    normalizeFloat(-slot.number(), static_cast<int>(instruction.intValue))));
            else
            {
                TypeInfo type;
                type.bits = static_cast<int>(instruction.intValue);
                type.isUnsigned = instruction.unsignedOp;
                push(Slot::fromInt(normalizeInt(-slot.integer, type)));
            }
            break;
        }
        case Op::Not:
        {
            Slot slot;
            if (!pop(slot)) break;
            push(Slot::fromInt(~slot.integer));
            break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            const bool useFloat = left.isFloat || right.isFloat;
            if (useFloat)
            {
                const double a = left.number();
                const double b = right.number();
                if ((instruction.op == Op::Div || instruction.op == Op::Mod) && b == 0.0)
                {
                    fail("浮点除零");
                    break;
                }
                double value = 0.0;
                switch (instruction.op)
                {
                case Op::Add: value = a + b; break;
                case Op::Sub: value = a - b; break;
                case Op::Mul: value = a * b; break;
                case Op::Div: value = a / b; break;
                default: value = std::fmod(a, b); break;
                }
                push(Slot::fromDouble(
                    normalizeFloat(value, static_cast<int>(instruction.intValue))));
                break;
            }
            const std::int64_t a = left.integer;
            const std::int64_t b = right.integer;
            if ((instruction.op == Op::Div || instruction.op == Op::Mod) && b == 0)
            {
                fail("整数除零");
                break;
            }
            std::int64_t value = 0;
            if (instruction.unsignedOp &&
                (instruction.op == Op::Div || instruction.op == Op::Mod))
            {
                const std::uint64_t ua = static_cast<std::uint64_t>(a);
                const std::uint64_t ub = static_cast<std::uint64_t>(b);
                value = static_cast<std::int64_t>(instruction.op == Op::Div ? ua / ub : ua % ub);
            }
            else
            {
                switch (instruction.op)
                {
                case Op::Add: value = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) +
                                                                static_cast<std::uint64_t>(b)); break;
                case Op::Sub: value = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) -
                                                                static_cast<std::uint64_t>(b)); break;
                case Op::Mul: value = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) *
                                                                static_cast<std::uint64_t>(b)); break;
                case Op::Div: value = (a == INT64_MIN && b == -1) ? a : a / b; break;
                default: value = (a == INT64_MIN && b == -1) ? 0 : a % b; break;
                }
            }
            TypeInfo type;
            type.bits = static_cast<int>(instruction.intValue);
            type.isUnsigned = instruction.unsignedOp;
            push(Slot::fromInt(normalizeInt(value, type)));
            break;
        }
        case Op::Shl: case Op::Shr: case Op::ShrU:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            const int shift = static_cast<int>(right.integer & 63);
            if (instruction.op == Op::Shl) push(Slot::fromInt(left.integer << shift));
            else if (instruction.op == Op::Shr) push(Slot::fromInt(left.integer >> shift));
            else
            {
                const std::uint64_t bits = static_cast<std::uint64_t>(left.integer);
                push(Slot::fromInt(static_cast<std::int64_t>(bits >> shift)));
            }
            break;
        }
        case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            std::int64_t value = 0;
            if (instruction.op == Op::BitAnd) value = left.integer & right.integer;
            else if (instruction.op == Op::BitOr) value = left.integer | right.integer;
            else value = left.integer ^ right.integer;
            push(Slot::fromInt(value));
            break;
        }
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::LtU: case Op::LeU: case Op::GtU: case Op::GeU:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            bool value = false;
            if (instruction.op == Op::Eq || instruction.op == Op::Ne)
            {
                const bool equal = (left.isFloat || right.isFloat)
                                       ? (left.number() == right.number())
                                       : (left.integer == right.integer);
                value = (instruction.op == Op::Eq) ? equal : !equal;
            }
            else
            {
                const bool unsignedCompare = (instruction.op == Op::LtU) ||
                                             (instruction.op == Op::LeU) ||
                                             (instruction.op == Op::GtU) ||
                                             (instruction.op == Op::GeU);
                const double a = left.number();
                const double b = right.number();
                if (unsignedCompare)
                {
                    const std::uint64_t ua = static_cast<std::uint64_t>(left.integer);
                    const std::uint64_t ub = static_cast<std::uint64_t>(right.integer);
                    if (instruction.op == Op::LtU) value = ua < ub;
                    else if (instruction.op == Op::LeU) value = ua <= ub;
                    else if (instruction.op == Op::GtU) value = ua > ub;
                    else value = ua >= ub;
                }
                else
                {
                    if (instruction.op == Op::Lt) value = a < b;
                    else if (instruction.op == Op::Le) value = a <= b;
                    else if (instruction.op == Op::Gt) value = a > b;
                    else value = a >= b;
                }
            }
            push(Slot::fromInt(value ? 1 : 0));
            break;
        }
        case Op::ToDouble:
        {
            Slot slot;
            if (!pop(slot)) break;
            push(Slot::fromDouble(
                normalizeFloat(slot.number(), static_cast<int>(instruction.intValue))));
            break;
        }
        case Op::ToInt:
        {
            Slot slot;
            if (!pop(slot)) break;
            TypeInfo type;
            type.bits = static_cast<int>(instruction.intValue);
            type.isUnsigned = instruction.unsignedOp;
            push(Slot::fromInt(
                normalizeInt(slot.isFloat ? static_cast<std::int64_t>(slot.number())
                                          : slot.integer, type)));
            break;
        }
        case Op::MulWide:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            const int bits = (instruction.intValue > 0 && instruction.intValue <= 64)
                                 ? static_cast<int>(instruction.intValue)
                                 : 32;
            const std::uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
            const std::uint64_t a = static_cast<std::uint64_t>(left.integer) & mask;
            const std::uint64_t b = static_cast<std::uint64_t>(right.integer) & mask;
            const std::uint64_t product = a * b;
            push(Slot::fromInt(static_cast<std::int64_t>(product & mask)));          // lo
            push(Slot::fromInt(static_cast<std::int64_t>((product >> bits) & mask))); // hi
            break;
        }
        case Op::DivWide:
        {
            Slot right;
            Slot left;
            if (!pop(right) || !pop(left)) break;
            const int bits = (instruction.intValue > 0 && instruction.intValue <= 64)
                                 ? static_cast<int>(instruction.intValue)
                                 : 32;
            const std::uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
            const std::uint64_t ua = static_cast<std::uint64_t>(left.integer) & mask;
            const std::uint64_t ub = static_cast<std::uint64_t>(right.integer) & mask;

            std::int64_t quotient = 0;
            std::int64_t remainder = 0;
            if (instruction.unsignedOp)
            {
                if (ub == 0u)
                {
                    fail("双字除法除零");
                    break;
                }
                quotient = static_cast<std::int64_t>(ua / ub);
                remainder = static_cast<std::int64_t>(ua % ub);
            }
            else
            {
                TypeInfo signedType = typeOf("i32");
                signedType.bits = bits;
                signedType.isUnsigned = false;
                signedType.isFloat = false;
                const std::int64_t sa = normalizeInt(static_cast<std::int64_t>(ua), signedType);
                const std::int64_t sb = normalizeInt(static_cast<std::int64_t>(ub), signedType);
                if (sb == 0)
                {
                    fail("双字除法除零");
                    break;
                }
                quotient = sa / sb;
                remainder = sa % sb;
            }
            push(Slot::fromInt(quotient));  // q
            push(Slot::fromInt(remainder)); // r
            break;
        }
        case Op::Jmp:
        {
            const auto target = program.labels.find(instruction.name);
            if (target == program.labels.end())
            {
                fail("跳转目标不存在：" + instruction.name);
                break;
            }
            pc = target->second;
            jumped = true;
            break;
        }
        case Op::Jz: case Op::Jnz:
        {
            Slot slot;
            if (!pop(slot)) break;
            const bool zero = (slot.number() == 0.0);
            const bool take = (instruction.op == Op::Jz) ? zero : !zero;
            if (take)
            {
                const auto target = program.labels.find(instruction.name);
                if (target == program.labels.end())
                {
                    fail("跳转目标不存在：" + instruction.name);
                    break;
                }
                pc = target->second;
                jumped = true;
            }
            break;
        }
        case Op::Print:
        {
            Slot slot;
            if (!pop(slot)) break;
            switch (instruction.format)
            {
            case PrintFormat::Unsigned:
                emitNumber(std::to_string(static_cast<std::uint64_t>(slot.integer)));
                break;
            case PrintFormat::Float:
                emitNumber(formatDouble(slot.number()));
                break;
            case PrintFormat::Text:
                result.text += slot.text;
                break;
            default:
                emitNumber(std::to_string(static_cast<std::int64_t>(slot.number())));
                break;
            }
            break;
        }
        case Op::PrintText: result.text += instruction.textValue; break;
        case Op::Putc:
        {
            Slot slot;
            if (!pop(slot)) break;
            result.text += static_cast<char>(static_cast<int64_t>(slot.number()) & 0xFF);
            break;
        }
        case Op::Read: case Op::Getc:
        {
            if (inputIndex >= input.size())
            {
                fail("输入不够（第 " + std::to_string(inputIndex + 1u) + " 项缺失）");
                break;
            }
            const std::string &item = input[inputIndex++];
            if (instruction.op == Op::Getc)
            {
                const int code = item.empty() ? 0 : static_cast<unsigned char>(item[0]);
                assign(instruction.name, Slot::fromInt(code));
                break;
            }
            const TypeInfo info = [&] {
                const auto found = program.types.find(instruction.name);
                return (found == program.types.end()) ? typeOf("") : found->second;
            }();
            bool isFloat = false;
            std::int64_t intValue = 0;
            double doubleValue = 0.0;
            if (!parseNumber(item, isFloat, intValue, doubleValue))
            {
                fail("输入 '" + item + "' 不是数字（变量 " + instruction.name + "）");
                break;
            }
            if (info.isFloat) assign(instruction.name, Slot::fromDouble(normalizeFloat(
                                  isFloat ? doubleValue : static_cast<double>(intValue), info.bits)));
            else assign(instruction.name, Slot::fromInt(isFloat ? static_cast<std::int64_t>(doubleValue)
                                                                : intValue));
            break;
        }
        case Op::Param:
        {
            Slot slot;
            if (!pop(slot)) break;
            pending.push_back(PendingArg{instruction.name, slot});
            break;
        }
        case Op::Alloc:
        {
            Slot size;
            if (!pop(size)) break;
            const std::int64_t bytes = size.isFloat ? 0 : size.integer;
            if (bytes <= 0)
            {
                fail("alloc 的字节数必须是正数（拿到 " + std::to_string(bytes) + "）");
                break;
            }
            const std::size_t address = heap.allocate(static_cast<std::size_t>(bytes));
            if (address == 0u)
            {
                fail("堆内存不足：申请 " + std::to_string(bytes) + " 字节，当前在用 " +
                     std::to_string(heap.liveBytes()) + " 字节");
                break;
            }
            push(Slot::fromInt(static_cast<std::int64_t>(address)));
            break;
        }
        case Op::Free:
        {
            Slot address;
            if (!pop(address)) break;
            /* 句柄区间（`bint` / 容器）走对象表；其余按堆地址严格释放。 */
            const std::int64_t handle = static_cast<std::int64_t>(address.number());
            if (trace_) result.trace.push_back("    release " + std::to_string(handle));
            if (isObjectHandle(handle))
            {
                if (!objects.release(handle))
                {
                    fail("free 失败：这个对象已经释放过了（句柄 " + std::to_string(handle) + "）");
                }
                break;
            }
            std::string reason;
            if (!heap.release(static_cast<std::size_t>(address.number()), &reason))
            {
                fail("free 失败：" + reason);
                break;
            }
            break;
        }
        case Op::Drop:
        {
            /* **软释放**（编译器在语句块出口插的那一条）：只有"地址确实是一块活跃的
             * 堆内存"才还回去。0（空指针）、数据区地址（字符串字面量）、以及**已经还过**
             * 的地址都当没这回事 —— 这就是"作用域所有"能安全重入的原因：
             * 显式 `delete` 之后变量被置 0，块出口再 drop 一次是空操作；
             * 两个变量指向同一块堆内存时（别名），多出来的那次 drop 也不会炸。 */
            Slot address;
            if (!pop(address)) break;
            if (trace_) result.trace.push_back("    drop " +
                                               std::to_string(static_cast<std::int64_t>(address.number())));
            /* 对象句柄（`bint` / 容器）也认：还在就还回去，已经还过就当没这回事
             * （和后面堆那条一个脾气）。 */
            const std::int64_t handle = static_cast<std::int64_t>(address.number());
            if (isObjectHandle(handle))
            {
                objects.release(handle);
                break;
            }
            const std::size_t target = static_cast<std::size_t>(address.number());
            if (target == 0u || !inHeap(target)) break;
            std::string reason;
            heap.release(target, &reason);   /* 还不了（已经不在了）就不管 */
            break;
        }
        case Op::Check:
        {
            // `(check, 下标, 上限, 标签)`：弹栈取上限与下标，越界就终止运行。
            Slot limit;
            Slot index;
            if (!pop(limit) || !pop(index)) break;
            const std::int64_t position = static_cast<std::int64_t>(index.number());
            const std::int64_t length = static_cast<std::int64_t>(limit.number());
            if (position < 0 || position >= length)
            {
                fail("数组下标越界：" + (instruction.name.empty() ? std::string("数组")
                                                                  : instruction.name) +
                     " 的下标 " + std::to_string(position) + "，长度 " + std::to_string(length));
                break;
            }
            break;
        }
        case Op::CheckPointer:
        {
            // `(checkptr, 下标, 基址, 步长, 标签)`：按"基址所在那一块"的容量查这个元素。
            Slot base;
            Slot index;
            if (!pop(base) || !pop(index)) break;
            const std::int64_t position = static_cast<std::int64_t>(index.number());
            const std::int64_t stride =
                (instruction.intValue > 0) ? instruction.intValue : 8;
            const std::size_t address = static_cast<std::size_t>(base.number());
            const std::string label =
                instruction.name.empty() ? std::string("这一块") : instruction.name;

            // 1. 下标不能为负。
            if (position < 0)
            {
                fail("数组下标越界：" + label + " 的下标 " + std::to_string(position) + "（不能为负）");
                break;
            }
            // 2. 找出基址落在哪一块里（堆块按分配大小；数据区按符号 size=）。
            std::size_t capacity = 0;
            bool found = false;
            if (inHeap(address))
            {
                found = heap.capacityAt(address, capacity);   // 没分配 / 已经 free → false
            }
            else if (data_ != nullptr)
            {
                for (const auto &item : data_->symbols)
                {
                    const std::size_t begin = item.second;
                    const auto sizeAt = data_->sizes.find(item.first);
                    const std::size_t bytes =
                        (sizeAt == data_->sizes.end()) ? 0u : sizeAt->second;
                    if (bytes == 0u) continue;
                    if (address >= begin && address < begin + bytes)
                    {
                        capacity = begin + bytes - address;   // 从当前地址到符号末尾
                        found = true;
                        break;
                    }
                }
            }
            if (!found || capacity == 0u)
            {
                fail("数组下标检查失败：" + label + " 的地址 " + std::to_string(address) +
                     " 不在任何一块已分配内存里（没分配、已经 free，或者不是数组的起始地址）");
                break;
            }
            // 3. 这个元素（基址 + 下标 × 步长，共 stride 字节）必须整块落在容量里。
            const std::int64_t offset = position * stride;
            if (static_cast<std::size_t>(offset) + static_cast<std::size_t>(stride) > capacity)
            {
                fail("数组下标越界：" + label + " 的下标 " + std::to_string(position) +
                     "（这一块只有 " + std::to_string(capacity) + " 字节，元素 " +
                     std::to_string(stride) + " 字节）");
                break;
            }
            break;
        }
        case Op::Call:
        {
            if (callNative(instruction.name, instruction.name2)) break;
            if (!enterFunction(instruction.name, instruction.name2, false)) break;
            break;
        }
        case Op::CallIndirect:
        {
            /* 函数指针：值就是"数据区里存着函数标签的那串字节"的地址（0 = 空指针）。 */
            Slot pointer;
            if (!pop(pointer)) break;
            const std::size_t address = static_cast<std::size_t>(pointer.number());
            if (address == 0u)
            {
                fail("调用了空函数指针（null）");
                pending.clear();
                break;
            }
            if (data_ == nullptr)
            {
                fail("没有加载数据区，无法通过函数指针取函数名");
                pending.clear();
                break;
            }
            const std::string label = data_->readCString(address);
            if (label.empty())
            {
                fail("函数指针指向的地址 " + std::to_string(address) + " 上没有函数名");
                pending.clear();
                break;
            }
            if (callNative(label, instruction.name2)) break;
            if (!enterFunction(label, instruction.name2, true)) break;
            break;
        }
        case Op::Ret:
        {
            Slot value;
            if (!pop(value)) break;
            if (currentCoroutine != 0u && frames.size() == 1u)
            {
                frames.clear();
                stack.clear();
                pending.clear();
                coroutines[currentCoroutine].state = CoroutineContext::State::Done;
                std::size_t next = coroutines.size();
                for (std::size_t index = 0; index < coroutines.size(); ++index)
                {
                    CoroutineContext &candidate = coroutines[index];
                    if (candidate.state == CoroutineContext::State::Waiting &&
                        candidate.waitingFor == currentCoroutine)
                    {
                        candidate.state = CoroutineContext::State::Ready;
                        candidate.waitingFor = 0u;
                        if (next == coroutines.size()) next = index;
                    }
                }
                if (next == coroutines.size()) next = nextRunnable(currentCoroutine);
                if (next == coroutines.size())
                {
                    result.ok = true;
                    pc = program.code.size();
                    jumped = true;
                    break;
                }
                loadCoroutine(next);
                break;
            }
            if (frames.empty())
            {
                pc = program.code.size();
                jumped = true;
                break;
            }
            // 出函数 = 这一帧的存储整块还回去（等价于 free）：把所有权从帧栈上摘下来，
            // 帧析构时本帧所有局部变量一起释放。递归的每一层因此互不影响。
            std::unique_ptr<Frame> frame = std::move(frames.back());
            frames.pop_back();
            if (!frame->returnDest.empty()) assign(frame->returnDest, value);
            pc = frame->returnIp;
            jumped = true;
            break;
        }
        case Op::Halt:
            result.ok = true;
            pc = program.code.size();
            jumped = true;
            break;
        case Op::AddrOf:
        {
            if (data_ == nullptr)
            {
                fail("没有加载数据区，无法取符号地址：" + instruction.name);
                break;
            }
            const auto found = data_->symbols.find(instruction.name);
            if (found == data_->symbols.end())
            {
                fail("数据区里没有符号：" + instruction.name);
                break;
            }
            push(Slot::fromInt(static_cast<std::int64_t>(found->second)));
            break;
        }
        case Op::LoadAt:
        {
            Slot address;
            if (!pop(address)) break;
            const std::size_t target = static_cast<std::size_t>(address.number());
            if (data_ == nullptr && !inHeap(target))
            {
                fail("没有加载数据区，无法按地址取值：地址 " + std::to_string(target));
                break;
            }
            Slot value;
            if (!loadMemory(target, instruction.isFloat, value))
            {
                const std::string where = inHeap(target) ? "堆" : "数据区";
                fail(where + "越界读：地址 " + std::to_string(target) +
                     (inHeap(target) ? "（没分配或已经 free 过）" : ""));
                break;
            }
            push(value);
            break;
        }
        case Op::StoreAt:
        {
            Slot value;
            Slot address;
            if (!pop(value) || !pop(address)) break;
            const std::size_t target = static_cast<std::size_t>(address.number());
            if (data_ == nullptr && !inHeap(target))
            {
                fail("没有加载数据区，无法按地址写值：地址 " + std::to_string(target));
                break;
            }
            if (!storeMemory(target, instruction.isFloat, value))
            {
                const std::string where = inHeap(target) ? "堆" : "数据区";
                fail(where + "越界写：地址 " + std::to_string(target) +
                     (inHeap(target) ? "（没分配或已经 free 过）" : ""));
                break;
            }
            break;
        }
        }

        if (hookInterval_ != 0u && instructionHook_ && result.steps % hookInterval_ == 0u)
        {
            const std::string text = instruction.toString();
            const std::size_t separator = text.find(' ');
            instructionHook_(InstructionHookInfo{
                result.steps, executedPc, executedCoroutine, text.substr(0u, separator)});
        }
        if (!result.error.empty()) return false;
        if (!jumped) ++pc;
        return pc < program.code.size();
    };

    while (executeOne()) {}

    if (result.error.empty() && !result.ok)
    {
        // 掉出程序尾也算正常结束（组装时补过 Halt，这里兜底）。
        result.ok = true;
    }
    for (const std::int64_t handle : objects.materializedObjects) objects.release(handle);
    for (const std::size_t address : objects.materializedHeap)
    {
        std::string ignored;
        heap.release(address, &ignored);
    }
    // 堆统计：停机时还有多少字节没 free —— 非 0 就是漏了，交给调用方决定报不报。
    result.heapPeakBytes = heap.peakBytes();
    result.heapLiveBytes = heap.liveBytes();
    result.heapLiveBlocks = heap.liveBlocks();
    result.liveObjects = objects.liveCount();
    if (result.timingAvailable)
    {
        result.finishedNanoseconds = timeCallback_();
        result.elapsedNanoseconds = result.finishedNanoseconds >= result.startedNanoseconds
                                        ? result.finishedNanoseconds - result.startedNanoseconds
                                        : 0u;
    }
    coroutines[currentCoroutine].pc = pc;
    // 收尾：把最后的位置与调用栈留成快照（出错时 fail() 已经填过，这里补成功停机的场合）。
    if (debugInfo_ != nullptr && result.sourceTrace.empty())
    {
        if (const SourceLocation *where = debugInfo_->lookup(pc)) result.failureLocation = *where;
        result.sourceTrace = debugFramesOf(pc);
        currentLocation_ = result.failureLocation;
        debugFrames_ = result.sourceTrace;
    }
    coroutines[currentCoroutine].state = result.ok ? CoroutineContext::State::Done
                                                   : coroutines[currentCoroutine].state;
    coroutines[currentCoroutine].stack = std::move(stack);
    coroutines[currentCoroutine].frames = std::move(frames);
    coroutines[currentCoroutine].pending = std::move(pending);
    const auto stateName = [](CoroutineContext::State state) {
        switch (state)
        {
        case CoroutineContext::State::Running: return "running";
        case CoroutineContext::State::Ready: return "ready";
        case CoroutineContext::State::Suspended: return "suspended";
        case CoroutineContext::State::Waiting: return "waiting";
        case CoroutineContext::State::Receiving: return "receiving";
        case CoroutineContext::State::Done: return "done";
        }
        return "unknown";
    };
    for (std::size_t index = 0; index < coroutines.size(); ++index)
    {
        const CoroutineContext &context = coroutines[index];
        result.coroutines.push_back(CoroutineInfo{index, stateName(context.state), context.pc,
                                                   context.frames.size(), context.stack.size(),
                                                   context.waitingFor});
    }
    return result;
}

} // namespace stackvm
