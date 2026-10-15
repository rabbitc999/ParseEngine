// transfer-one-thread —— 单线程后端工具链：一个可执行文件，五个模块 + 一条流水线。
//
//   t1t irbridge …   三元式 ↔ 四元式互转（library/transfer-one-thread/irbridge）
//   t1t dataflow …   通用数据流框架：活跃变量 / 到达定值 / 可用表达式 / 常量传播
//   t1t ssa      …   SSA 构造、SCCP、GVN、φ 消除
//   t1t regalloc …   图着色 / 线性扫描 + 溢出重写
//   t1t transfer …   四元式 → 目标代码 → 模拟执行
//   t1t pipeline …   一条流水线跑完上面五块，并做跨模块的语义对照
#include "module_main.hpp"

#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void usage() {
    std::cout << "用法：t1t <子命令> [参数…]\n"
                 "\n"
                 "子命令：\n"
                 "  irbridge   三元式 ↔ 四元式互转，并导出 transfer 可读的四元式\n"
                 "  dataflow   数据流框架（live / reach / avail / const）\n"
                 "  ssa        SSA 构造 / SCCP / GVN / φ 消除\n"
                 "  regalloc   寄存器分配（图着色 / 线性扫描 + 溢出重写）\n"
                 "  irbuild    用构建器造四元式 / 三元式\n"
                 "  transfer   四元式 → 目标代码（MIPS / 8086 / ARM）→ 模拟执行\n"
                 "  pipeline   一条流水线跑完五块，并做跨模块的语义对照\n"
                 "\n"
                 "每个子命令都支持 -h：`t1t ssa -h`、`t1t pipeline -h` …\n"
                 "`t1t --version` 打印版本。\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        usage();
        return 0;
    }
    if (command == "--version" || command == "-v") {
        std::cout << "transfer-one-thread 1.0.0\n";
        return 0;
    }
    if (command == "irbridge") return irbridgeMain(argc - 1, argv + 1);
    if (command == "dataflow") return dataflowMain(argc - 1, argv + 1);
    if (command == "ssa") return ssaMain(argc - 1, argv + 1);
    if (command == "regalloc") return regallocMain(argc - 1, argv + 1);
    if (command == "irbuild") return irbuildMain(argc - 1, argv + 1);
    if (command == "transfer") return transferMain(argc - 1, argv + 1);
    if (command == "pipeline") return pipelineMain(argc - 1, argv + 1);
    std::cerr << "t1t: 不认识的子命令 " << command << "\n";
    usage();
    return 2;
}
