#pragma once
// 各模块自己的命令行入口（原来叫 main，合并进 transfer-one-thread 之后改名，
// 由 app/main.cpp 按子命令分发）。五个模块各自只依赖标准库，互不干扰。
//
//   t1t irbridge …   三元式 ↔ 四元式
//   t1t dataflow …   通用数据流框架（RD / LIVE / AE / 常量传播）
//   t1t ssa      …   SSA 构造 / SCCP / GVN / φ 消除
//   t1t regalloc …   图着色 / 线性扫描 + 溢出重写
//   t1t transfer …   四元式 → 目标代码 → 模拟执行

/**
 * irbridge 命令行入口（三元式 ↔ 四元式、导出 transfer 文本）。由编译后的 t1t 按子命令分发调用。。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 进程退出码
 */
int irbridgeMain(int argc, char** argv);
/**
 * dataflow 命令行入口（live / reach / avail / const 四种分析与四种输出模式）。由 t1t 分发调用。。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 进程退出码
 */
int dataflowMain(int argc, char** argv);
/**
 * ssa 命令行入口（构造 / SCCP / GVN / φ 消除 / 支配树）。由 t1t 分发调用。。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 进程退出码
 */
int ssaMain(int argc, char** argv);
/**
 * regalloc 命令行入口（图着色 / 线性扫描 + 溢出重写）。由 t1t 分发调用。。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 进程退出码
 */
int regallocMain(int argc, char** argv);
/**
 * irbuild 命令行入口（用构建器造四元式 / 三元式并展示三种形态）。由 t1t 分发调用。。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 进程退出码
 */
int irbuildMain(int argc, char** argv);
// typebuild 子命令的入口在独立库 library/typebuild 的 typebuild.hpp 里（t1t::typebuildMain）。

/**
 * `t1t transfer`：读写四元式、优化、生成目标代码、模拟执行。
 */
int transferMain(int argc, char** argv);
/**
 * `t1t pipeline`：一条流水线跑完五块（含跨模块的语义对照）。
 */
int pipelineMain(int argc, char** argv);
