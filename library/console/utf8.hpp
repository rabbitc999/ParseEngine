/**
 * @file utf8.hpp
 * @brief ParseEngine 的命令行 UTF-8 支持：Windows 实参解析、控制台 UTF-8 输出与代码页恢复。
 *
 * 这份头文件是 ParseEngine 侧的源文件，`tools/sync_tool_console.py` 把它逐字节复制到
 *   * `source/include/ParseEngine/tool_console.hpp`（按 ParseEngine 自己的 include 根命名）
 * 改这里再跑同步脚本（`source/Makefile` 与 `source/CMakeLists.txt` 也会在构建时自动同步）。
 *
 * Magel 已剥离成独立项目，自带一份同名头（那边的 master 是 `magel/console/utf8.hpp`，
 * 由它自己的 `tools/sync_tool_console.py` 同步到板端副本）；改一边不再自动同步另一边。
 *
 * 为什么需要它：Windows 上 `main(int, char**)` 拿到的实参是系统 ANSI 代码页而不是
 * UTF-8，控制台默认也不是 UTF-8 输出，中文路径 / 中文输出会变成乱码。这里用 CRT 的
 * 宽字符实参重新编码成 UTF-8，并在 `Session` 的生命周期内把控制台输入输出代码页切到
 * UTF-8，析构时恢复原值。
 */
#ifndef PARSEENGINE_TOOL_CONSOLE_HPP
#define PARSEENGINE_TOOL_CONSOLE_HPP

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef _MSC_VER
#include <corecrt_startup.h>
#else
/* MinGW 没有 `<corecrt_startup.h>`，但它的 CRT 里同样有宽字符版的 `__getmainargs`；
 * 这里自己声明一份，签名与 MS CRT 的内部符号一致。 */
extern "C" int __cdecl __wgetmainargs(int *, wchar_t ***, wchar_t ***, int, void *);
#endif
#endif

namespace tool_console {

#ifdef _WIN32
/**
 * @brief 按 Windows CRT 的规则给参数加引号，供 `_wspawnvp` 之类的参数数组使用。
 *
 * 规则与 MSVC 运行库一致：整串包在双引号里；串内的 `"` 用反斜线转义；`"` 之前
 * 连续的反斜线数量翻倍；结尾连续的反斜线也翻倍（否则会把收尾的引号转义掉）。
 *
 * @param value UTF-16 参数。
 * @return 可安全交给子进程参数数组的字符串（含首尾引号）。
 */
inline std::wstring quoteArgument(const std::wstring &value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;   /* 已经攒下、还没决定怎么提交的反斜线个数 */
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;   /* 攒着：要看它后面是不是引号 / 串尾 */
        }
        /* 反斜线只在两种情况有意义：转义引号、或位于串尾。落在别处按原样提交，
         * 所以这里先把攒下的补齐（后面紧跟引号就翻倍），再单独处理引号本身。 */
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');   /* 串尾反斜线翻倍，别把收尾引号吃掉 */
    return result + L'"';
}

/**
 * @brief 把 C++ 的 UTF-8 输出写进 Windows 控制台（UTF-16）。
 *
 * 作为 `std::cout` / `std::cerr` 的缓冲区装到流上：
 *   * 目标**是控制台**时，攒够一批 UTF-8 字节就用 `WriteConsoleW` 写宽字符。
 *     `std::cout` / `printf` 交出来的多字节序列经常在字符中间被切开，所以攒的
 *     时候必须留出"半截字符"（见 `xsputn`）。
 *   * 目标**被重定向**（管道、文件）时直接透传原始字节，编码保持 UTF-8 不变。
 */
class ConsoleBuffer final : public std::streambuf {
    HANDLE handle_;              ///< 控制台句柄（`STD_OUTPUT_HANDLE` / `STD_ERROR_HANDLE`）。
    std::streambuf *original_;   ///< 原来挂在流上的缓冲区，重定向时透传给它。
    std::string pending_;        ///< 已收到、但还没提交给控制台的 UTF-8 字节。
    bool console_;               ///< 目标是控制台（而不是管道 / 文件）。

    /**
     * @brief 把 `pending_` 里的 UTF-8 文本转成 UTF-16 写进控制台。
     * @return 成功返回 0，失败返回 -1（交回给流的错误处理）。
     */
    int commit() {
        if (pending_.empty()) return 0;
        const int count = MultiByteToWideChar(CP_UTF8, 0, pending_.data(),
                                              static_cast<int>(pending_.size()), nullptr, 0);
        if (count <= 0) return -1;
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, pending_.data(), static_cast<int>(pending_.size()),
                            &wide[0], count);
        std::size_t offset = 0;
        while (offset < wide.size()) {
            /* 一次最多写 16 KB；并且不能把代理对（surrogate pair）从中间切开，
             * 否则 `WriteConsoleW` 会写出坏字符。 */
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>((std::min)(wide.size() - offset, std::size_t(16384)));
            if (offset + chunk < wide.size() && wide[offset + chunk - 1] >= 0xd800 &&
                wide[offset + chunk - 1] <= 0xdbff) {
                --chunk;   /* 末尾刚好是高位代理：这一批少写一个，留到下一批 */
            }
            if (!WriteConsoleW(handle_, wide.data() + offset, chunk, &written, nullptr) || !written) {
                return -1;
            }
            offset += written;
        }
        pending_.clear();
        return 0;
    }

protected:
    /**
     * @brief 写入一批 UTF-8 字节；控制台模式下攒着，等攒够或同步时再提交。
     * @param data 字节。
     * @param size 字节数。
     * @return 实际写入的字节数（沿用流的语义，一律按全部写入算）。
     */
    std::streamsize xsputn(const char *data, std::streamsize size) override {
        if (!console_) return original_->sputn(data, size);
        pending_.append(data, static_cast<std::size_t>(size));
        if (pending_.size() >= 8192) {
            /* 到阈值了：把末尾那个"半截字符"留下，其余提交掉。从末尾往回跳过
             * 续字节（10xxxxxx）找到末尾字符的首字节，按首字节算出它本来有几个
             * 字节，不够就整个留到下一批。 */
            std::size_t end = pending_.size(), start = end;
            while (start > 0 && (static_cast<unsigned char>(pending_[start - 1]) & 0xc0) == 0x80) {
                --start;
            }
            if (start > 0) {
                const auto lead = static_cast<unsigned char>(pending_[start - 1]);
                const std::size_t expected =
                    lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
                if (end - (start - 1) < expected) end = start - 1;
            }
            const std::string tail = pending_.substr(end);
            pending_.resize(end);
            if (commit() != 0) return 0;
            pending_ = tail;
        }
        return size;
    }

    /**
     * @brief 写入单个字节。
     * @param value 字节（也可能是 `eof`）。
     * @return 成功返回写入的字节（`eof` 时返回"非 eof"），失败返回 `eof`。
     */
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return sync() == 0 ? traits_type::not_eof(value) : traits_type::eof();
        }
        char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }

    /**
     * @brief 同步：控制台模式下把攒着的字节提交掉，否则同步底层流。
     * @return 成功返回 0。
     */
    int sync() override { return console_ ? commit() : original_->pubsync(); }

public:
    /**
     * @brief 绑定控制台句柄与原来挂在流上的缓冲区。
     * @param handle 标准输出 / 标准错误句柄。
     * @param original 原缓冲区（重定向时透传的目标）。
     */
    ConsoleBuffer(HANDLE handle, std::streambuf *original) : handle_(handle), original_(original) {
        /* 句柄能查到控制台模式说明它连着控制台；被重定向（管道 / 文件）时查不到。 */
        DWORD mode = 0;
        console_ = GetConsoleMode(handle, &mode) != 0;
    }
};
#endif   /* _WIN32 */

/**
 * @brief 进程级控制台守卫：把控制台切到 UTF-8，退出时恢复原代码页。
 *
 * 用法是在 `main` 开头构造一个局部对象（`tool_console::Session console;`），靠析构
 * 恢复代码页。要点：
 *   * 代码页查得到才切，查不到（无控制台 / 被重定向）就原样不动；
 *   * 重定向时的**字节**编码不受影响（`ConsoleBuffer` 直接透传）；
 *   * 顺带把 `PYTHONIOENCODING` 设成 `utf-8`，让开发子进程（`magel test/fmt/lsp`
 *     这类 Python 工具）的输出也是 UTF-8。
 */
class Session {
#ifdef _WIN32
    UINT output_ = GetConsoleOutputCP(), input_ = GetConsoleCP();   ///< 原代码页，析构时恢复。
    std::streambuf *cout_ = std::cout.rdbuf(), *cerr_ = std::cerr.rdbuf();   ///< 原流缓冲区。
    ConsoleBuffer out_{GetStdHandle(STD_OUTPUT_HANDLE), cout_};   ///< 标准输出接管。
    ConsoleBuffer err_{GetStdHandle(STD_ERROR_HANDLE), cerr_};    ///< 标准错误接管。
#endif
public:
    /// @brief 切到 UTF-8 控制台，并把 `std::cout` / `std::cerr` 接到 `ConsoleBuffer` 上。
    Session() {
#ifdef _WIN32
        /* 不依赖 `GetConsoleWindow`：ConPTY / 重定向时仍可能有有效的控制台代码页。 */
        if (output_) SetConsoleOutputCP(CP_UTF8);
        if (input_) SetConsoleCP(CP_UTF8);
        std::cout.rdbuf(&out_);
        std::cerr.rdbuf(&err_);
        _putenv_s("PYTHONIOENCODING", "utf-8");
#endif
    }

    /// @brief 先冲掉 C / C++ 标准流（此时缓冲区还挂着），再恢复原代码页与缓冲区。
    ~Session() {
#ifdef _WIN32
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);   /* C 的 stdout / stderr，以及直接写句柄的库 */
        std::cout.rdbuf(cout_);
        std::cerr.rdbuf(cerr_);
        if (output_) SetConsoleOutputCP(output_);
        if (input_) SetConsoleCP(input_);
#endif
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

/**
 * @brief 取命令行实参，一律按 UTF-8 返回。
 *
 * Windows 上 `argv` 是 ANSI 代码页的，中文参数会坏掉：这里改用 CRT 的宽字符实参
 * （MSVC 走 `_configure_wide_argv(_crt_argv_unexpanded_arguments)` + `__wargv`，
 * MinGW 走 `__wgetmainargs`），逐条转成 UTF-8。拿不到宽字符实参、或条数与 `argc`
 * 对不上时，退回原样的 `argv`。
 *
 * @param argc 参数个数。
 * @param argv 原始参数（`main` 里的那份）。
 * @return 拥有型 UTF-8 参数。
 */
inline std::vector<std::string> arguments(int argc, char **argv) {
    std::vector<std::string> result;
#ifdef _WIN32
    int count = 0;
    wchar_t **wideArguments = nullptr;
#ifdef _MSC_VER
    if (_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0) {
        count = __argc;
        wideArguments = __wargv;
    }
#else
    wchar_t **environment = nullptr;
    int mode = 0;
    if (__wgetmainargs(&count, &wideArguments, &environment, 0, &mode) != 0) {
        wideArguments = nullptr;
    }
#endif
    if (wideArguments && count == argc) {
        for (int i = 0; i < count; ++i) {
            const int size =
                WideCharToMultiByte(CP_UTF8, 0, wideArguments[i], -1, nullptr, 0, nullptr, nullptr);
            if (size <= 0) {
                result.clear();   /* 转换失败：整份丢弃，退回 argv */
                break;
            }
            std::string value(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wideArguments[i], -1, &value[0], size, nullptr, nullptr);
            value.pop_back();   /* 末尾那个 '\0' 不算内容 */
            result.push_back(std::move(value));
        }
        if (result.size() == static_cast<std::size_t>(argc)) return result;
    }
#endif
    for (int i = 0; i < argc; ++i) result.emplace_back(argv[i]);
    return result;
}

}   // namespace tool_console

#endif   // PARSEENGINE_TOOL_CONSOLE_HPP
