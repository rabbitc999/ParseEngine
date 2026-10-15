/**
 * @file benchmark_stackvm.cpp
 * @brief 可复现的 StackVM 整数循环吞吐基准。
 */

#include "stackvm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

std::uint64_t nowNanoseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

stackvm::Program makeProgram(std::uint64_t iterations)
{
    std::ostringstream source;
    source << "0: (decl, i64, global, i)\n"
           << "1: (decl, i64, global, sum)\n"
           << "2: (decl, i64, global, next_sum)\n"
           << "3: (decl, i64, global, next_i)\n"
           << "4: (decl, bool, global, more)\n"
           << "5: (:=, 0, _, i)\n"
           << "6: (:=, 0, _, sum)\n"
           << "7: (label, _, _, loop)\n"
           << "8: (+:i64, sum, i, next_sum)\n"
           << "9: (:=, next_sum, _, sum)\n"
           << "10: (+:i64, i, 1, next_i)\n"
           << "11: (:=, next_i, _, i)\n"
           << "12: (<:i64, i, " << iterations << ", more)\n"
           << "13: (jnz, more, _, loop)\n"
           << "14: (write, sum, _, _)\n"
           << "15: (halt, _, _, _)\n";
    const stackvm::QuadFile file = stackvm::parseQuads(source.str(), "<benchmark>");
    stackvm::Program program;
    if (!stackvm::assemble(file, program))
        throw std::runtime_error("基准程序组装失败");
    return program;
}

} // namespace

int main(int argc, char **argv)
{
    const std::uint64_t iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000u;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 7;
    if (iterations == 0u || repeats < 1)
    {
        std::cerr << "用法：stackvm-benchmark [循环次数>0] [重复次数>0]\n";
        return 2;
    }

    const stackvm::Program program = makeProgram(iterations);
    stackvm::Machine machine;
    machine.setTimeCallback(nowNanoseconds);
    const std::size_t stepLimit = static_cast<std::size_t>(iterations * 24u + 100u);

    const stackvm::RunResult warmup = machine.run(program, {}, stepLimit);
    if (!warmup.ok)
    {
        std::cerr << "预热失败：" << warmup.error << '\n';
        return 1;
    }

    std::vector<std::uint64_t> samples;
    std::size_t steps = 0u;
    std::size_t maxStackDepth = 0u;
    std::string checksum;
    for (int run = 0; run < repeats; ++run)
    {
        const stackvm::RunResult result = machine.run(program, {}, stepLimit);
        if (!result.ok || !result.timingAvailable)
        {
            std::cerr << "测量失败：" << result.error << '\n';
            return 1;
        }
        samples.push_back(result.elapsedNanoseconds);
        steps = result.steps;
        maxStackDepth = std::max(maxStackDepth, result.maxStackDepth);
        checksum = result.output;
    }

    std::sort(samples.begin(), samples.end());
    const std::uint64_t median = samples[samples.size() / 2u];
    const double seconds = static_cast<double>(median) / 1.0e9;
    const double mips = seconds == 0.0 ? 0.0 : static_cast<double>(steps) / seconds / 1.0e6;

    std::cout << "StackVM integer-loop benchmark\n"
              << "iterations=" << iterations << " repeats=" << repeats << '\n'
              << "steps=" << steps << " median_ns=" << median << '\n'
              << std::fixed << std::setprecision(3)
              << "median_ms=" << seconds * 1000.0 << " throughput_mips=" << mips << '\n'
              << "max_stack_slots=" << maxStackDepth << " checksum=" << checksum << '\n';
    return 0;
}
