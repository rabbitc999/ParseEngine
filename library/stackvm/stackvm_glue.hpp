#ifndef PARSE_ENGINE_STACKVM_GLUE_HPP
#define PARSE_ENGINE_STACKVM_GLUE_HPP

#include <memory>
#include <cstdint>
#include <string>

namespace stackvm
{

/**
 * 宿主文件句柄接口。VM 只依赖这组操作，不依赖 std::fstream、POSIX 或 RTOS。
 */
class FileHandle
{
public:
    virtual ~FileHandle() = default;
    [[nodiscard]] virtual bool isOpen() const = 0;
    virtual bool readLine(std::string &line, std::string &error) = 0;
    virtual bool write(const std::string &text, std::string &error) = 0;
    virtual bool flush(std::string &error)
    {
        error = "这个文件后端不支持 flush";
        return false;
    }
    virtual bool seek(std::int64_t, int, std::int64_t &, std::string &error)
    {
        error = "这个文件后端不支持 seek";
        return false;
    }
    [[nodiscard]] virtual bool eof() const = 0;
    virtual void close() = 0;
};

/**
 * 可注入的宿主文件 API。嵌入式端可在单独翻译单元中对接 Flash、SD 卡或 RTOS VFS。
 */
class FileApi
{
public:
    virtual ~FileApi() = default;
    virtual std::shared_ptr<FileHandle> open(const std::string &path,
                                             const std::string &mode,
                                             std::string &error) = 0;
};

/**
 * 桌面端默认胶水。嵌入式程序应通过 Machine::setFileApi() 注入自己的实现。
 */
FileApi &defaultFileApi();

} // namespace stackvm

#endif // PARSE_ENGINE_STACKVM_GLUE_HPP
