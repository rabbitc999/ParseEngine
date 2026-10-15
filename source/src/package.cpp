/** @file package.cpp
 * @brief 将 zip 子命令委托给 Python 标准库发布脚本。
 */
#include "ParseEngine/package.hpp"
#include "ParseEngine/i18n.hpp"
#include "ParseEngine/package_tool.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cerrno>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
namespace ParseEngine {
namespace {
/** @brief 获取当前映像，避免 PATH 启动时打包错误文件。
 * @param program argv[0]。 @return UTF-8 路径；失败返回空串。
 */
std::string executablePath(const char *program){
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD count=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(count && count<buffer.size())return std::filesystem::path(std::wstring(buffer.data(),count)).u8string();
#elif defined(__linux__)
    std::error_code ec;auto current=std::filesystem::read_symlink("/proc/self/exe",ec);if(!ec)return current.u8string();
#elif defined(__APPLE__)
    std::uint32_t count=0;_NSGetExecutablePath(nullptr,&count);std::vector<char> buffer(count);
    if(_NSGetExecutablePath(buffer.data(),&count)==0)return buffer.data();
#endif
    std::error_code error;auto path=std::filesystem::absolute(std::filesystem::u8path(program),error);
    return error?std::string():path.u8string();
}
/** @brief 清理本次独占目录内的脚本；不递归处理外来文件。 */
struct ScriptCleanup {
    std::filesystem::path file,directory;
    ~ScriptCleanup(){std::error_code ec;std::filesystem::remove(file,ec);std::filesystem::remove(directory,ec);}
};
}
int packageExecutable(const char *program,const std::string &output){
    namespace fs=std::filesystem;
    const auto compiler=executablePath(program);
    if(compiler.empty()){std::cerr<<PE_TEXT("无法定位 ParseEngine 可执行文件")<<"\n";return 1;}
    std::error_code ec;const auto root=fs::temp_directory_path(ec);
    if(ec){std::cerr<<PE_TEXT("无法创建打包脚本临时目录")<<"\n";return 1;}
    fs::path directory;
    for(unsigned i=0;i<100;++i){
        directory=root/("parseengine-package-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(i));
        if(fs::create_directory(directory,ec))break;
        if(ec || i==99){std::cerr<<PE_TEXT("无法创建打包脚本临时目录")<<"\n";return 1;}
    }
    const auto script=directory/"package_compiler.py";ScriptCleanup cleanup{script,directory};
    std::ofstream stream(script,std::ios::binary);stream<<packageScript;stream.close();
    if(!stream){std::cerr<<PE_TEXT("无法写入打包脚本")<<"\n";return 1;}
    const char *configured=std::getenv("PARSEENGINE_PYTHON");
#ifdef _WIN32
    const std::string python=configured?configured:"python";
#else
    const std::string python=configured?configured:"python3";
#endif
    const auto language=I18n::language();
    std::vector<std::string> args{python,script.u8string(),"ParseEngine","--compiler",compiler,"--ui-lang",language==I18n::Language::Zh?"zh":language==I18n::Language::Ja?"ja":"en"};
    if(!output.empty()){args.emplace_back("--out");args.push_back(output);}
    std::cout.flush();std::cerr.flush();std::fflush(nullptr);
#ifdef _WIN32
    std::vector<std::wstring> wide;for(const auto &arg:args)wide.push_back(tool_console::quoteArgument(fs::u8path(arg).wstring()));
    std::vector<const wchar_t*> values;for(const auto &arg:wide)values.push_back(arg.c_str());values.push_back(nullptr);
    const auto executable=fs::u8path(python).wstring();
    const auto status=_wspawnvp(_P_WAIT,executable.c_str(),values.data());
    if(status>=0)return static_cast<int>(status);
#else
    std::vector<char*> values;for(auto &arg:args)values.push_back(arg.data());values.push_back(nullptr);
    const auto child=fork();
    if(child==0){execvp(values[0],values.data());_exit(127);}
    if(child>0){int status=0;pid_t waited;do {waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
        if(waited>0)return WIFEXITED(status)?WEXITSTATUS(status):1;}
#endif
    std::cerr<<PE_TEXT("打包需要 Python 3.11+；可设置 PARSEENGINE_PYTHON")<<"\n";return 1;
}
}
