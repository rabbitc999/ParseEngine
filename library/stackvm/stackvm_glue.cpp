/**
 * @file stackvm_glue.cpp
 * @brief StackVM 的桌面宿主胶水；嵌入式移植可用自己的 FileApi 替换本实现。
 */

#include "stackvm_glue.hpp"

#include <fstream>

namespace stackvm
{
namespace
{

class DesktopFileHandle final : public FileHandle
{
public:
    DesktopFileHandle(const std::string &path, const std::string &mode)
    {
        std::ios::openmode flags = std::ios::binary;
        readable_ = mode.find('r') != std::string::npos || mode.find('+') != std::string::npos;
        writable_ = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
                    mode.find('+') != std::string::npos;
        if (readable_) flags |= std::ios::in;
        if (mode.find('w') != std::string::npos) flags |= std::ios::out | std::ios::trunc;
        if (mode.find('a') != std::string::npos) flags |= std::ios::out | std::ios::app;
        if (mode.find('+') != std::string::npos) flags |= std::ios::in | std::ios::out;
        stream_.open(path, flags);
    }

    [[nodiscard]] bool isOpen() const override { return stream_.is_open(); }

    bool readLine(std::string &line, std::string &error) override
    {
        if (std::getline(stream_, line)) return true;
        if (stream_.bad())
        {
            error = "读取文件失败";
            return false;
        }
        stream_.clear(stream_.rdstate() & ~std::ios::failbit);
        line.clear();
        return true;
    }

    bool write(const std::string &text, std::string &error) override
    {
        stream_ << text;
        if (!stream_)
        {
            error = "写入文件失败";
            return false;
        }
        stream_.flush();
        return true;
    }

    bool flush(std::string &error) override
    {
        stream_.flush();
        if (stream_) return true;
        error = "刷新文件失败";
        return false;
    }

    bool seek(std::int64_t offset, int origin, std::int64_t &position,
              std::string &error) override
    {
        std::ios::seekdir direction = std::ios::beg;
        if (origin == 1) direction = std::ios::cur;
        else if (origin == 2) direction = std::ios::end;
        else if (origin != 0)
        {
            error = "未知的 seek 起点";
            return false;
        }
        stream_.clear();
        if (readable_) stream_.seekg(static_cast<std::streamoff>(offset), direction);
        if (!stream_)
        {
            error = "文件定位失败";
            return false;
        }
        if (writable_) stream_.seekp(static_cast<std::streamoff>(offset), direction);
        if (!stream_)
        {
            error = "文件定位失败";
            return false;
        }
        const std::streampos readPosition = readable_ ? stream_.tellg() : std::streampos(-1);
        const std::streampos writePosition = writable_ ? stream_.tellp() : std::streampos(-1);
        const std::streampos selected = readPosition != std::streampos(-1)
                                            ? readPosition : writePosition;
        if (selected == std::streampos(-1))
        {
            error = "读取文件位置失败";
            return false;
        }
        position = static_cast<std::int64_t>(selected);
        return true;
    }

    [[nodiscard]] bool eof() const override { return stream_.eof(); }
    void close() override { stream_.close(); }

private:
    std::fstream stream_;
    bool readable_ = false;
    bool writable_ = false;
};

class DesktopFileApi final : public FileApi
{
public:
    std::shared_ptr<FileHandle> open(const std::string &path, const std::string &mode,
                                     std::string &error) override
    {
        auto result = std::make_shared<DesktopFileHandle>(path, mode);
        if (!result->isOpen())
        {
            error = "打不开文件：" + path + "（mode=" + mode + "）";
            return {};
        }
        return result;
    }
};

} // namespace

FileApi &defaultFileApi()
{
    static DesktopFileApi api;
    return api;
}

} // namespace stackvm
