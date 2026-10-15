/** @file windows_console_test.cpp
 * @brief 在 GBK/日文控制台代码页下验证 Unicode 输出、UTF-8 管道与代码页恢复。
 */
#include "../library/console/utf8.hpp"
#include <cstdlib>
#ifdef _WIN32
/** @brief 按 Windows CRT 规则引用参数。 @param arg UTF-16 参数。 @return 可拼接的命令行参数。 */
std::wstring quote(const std::wstring &arg){
    std::wstring out=L"\"";std::size_t slashes=0;
    for(wchar_t c:arg){if(c==L'\\'){++slashes;continue;}
        out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;
        if(c==L'"')out+=L'\\';out+=c;}
    out.append(slashes*2,L'\\');return out+L'"';
}
#endif
/** @brief 输出夹具或运行真实工具并读取 UTF-16 屏幕缓冲区。 @return 验收退出码。 */
int main(int argc,char **argv){
    if(argc==2 && std::string(argv[1])=="--bytes"){
        tool_console::Session console;
        std::cout<<"C++ 中文 / 日本語\n"<<std::flush;
        std::fprintf(stderr,"C stdio 中文 / 日本語\n");return 0;
    }
#ifdef _WIN32
    auto args=tool_console::arguments(argc,argv);
    if(args.size()<3)return 2;
    const UINT codepage=static_cast<UINT>(std::strtoul(args[1].c_str(),nullptr,10));
    bool allocated=false;
    if(!GetConsoleOutputCP()){if(!AllocConsole())return 3;allocated=true;ShowWindow(GetConsoleWindow(),SW_HIDE);}
    const UINT oldOut=GetConsoleOutputCP(),oldIn=GetConsoleCP();
    SetConsoleOutputCP(codepage);SetConsoleCP(codepage);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE buffer=CreateConsoleScreenBuffer(GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CONSOLE_TEXTMODE_BUFFER,nullptr);
    if(buffer==INVALID_HANDLE_VALUE)return 4;
    COORD size{160,300};SetConsoleScreenBufferSize(buffer,size);
    std::wstring command;
    for(std::size_t i=2;i<args.size();++i){const auto &arg=args[i];
        int count=MultiByteToWideChar(CP_UTF8,0,arg.data(),static_cast<int>(arg.size()),nullptr,0);
        std::wstring wide(static_cast<std::size_t>(count),L'\0');MultiByteToWideChar(CP_UTF8,0,arg.data(),static_cast<int>(arg.size()),&wide[0],count);
        if(!command.empty())command+=L' ';command+=quote(wide);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;startup.hStdOutput=buffer;startup.hStdError=buffer;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process)){CloseHandle(buffer);return 5;}
    if(WaitForSingleObject(process.hProcess,60000)!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,9);return 6;}
    DWORD status=0;GetExitCodeProcess(process.hProcess,&status);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    const bool restored=GetConsoleOutputCP()==codepage && GetConsoleCP()==codepage;
    std::wstring screen(static_cast<std::size_t>(size.X)*size.Y,L'\0');DWORD read=0;
    const bool ok=ReadConsoleOutputCharacterW(buffer,&screen[0],static_cast<DWORD>(screen.size()),COORD{0,0},&read)!=0;
    CloseHandle(buffer);SetConsoleOutputCP(oldOut);SetConsoleCP(oldIn);if(allocated)FreeConsole();
    if(!ok || !restored)return 7;
    screen.resize(read);int count=WideCharToMultiByte(CP_UTF8,0,screen.data(),static_cast<int>(screen.size()),nullptr,0,nullptr,nullptr);
    std::string utf8(static_cast<std::size_t>(count),'\0');WideCharToMultiByte(CP_UTF8,0,screen.data(),static_cast<int>(screen.size()),&utf8[0],count,nullptr,nullptr);
    std::fwrite(utf8.data(),1,utf8.size(),stdout);return static_cast<int>(status);
#else
    return 0;
#endif
}
