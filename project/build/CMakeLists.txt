# CMakeLists.txt —— custom 运行时，由 Script-Engine 自动生成（C++17，__MODE_NAME__）
#
# 与同目录的 Makefile 等价，二者任选其一：
#   cmake -S . -B build
#   cmake --build build
#   printf '<你的输入>' | ./build/parser_demo     # 演示程序从标准输入读一行
#
# 目标：
#   parser        静态库（libparser.a）
#   parser_demo   演示程序
#
# 语义值类型 ParserValue_T 由文法（%value_type / %union / %{{...}}%）给出；
# 内存全部由标准库管理，不支持自定义分配器。
# %copy 拷进来的源文件会自动追加到 source 列表。

cmake_minimum_required(VERSION 3.10)

project(parser_demo C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(parser STATIC
    pl0_parser.cpp
)

target_include_directories(parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(parser_demo pl0_main.cpp)
target_link_libraries(parser_demo PRIVATE parser)

# libm 提供数学函数；Windows 上没有独立的 libm，跳过即可
if(NOT WIN32)
    target_link_libraries(parser_demo PRIVATE m)
endif()

# 产物源码是 UTF-8（含中文注释与字符串）：MSVC 不加 /utf-8 会按当前代码页
# 解码源文件，简体中文环境（GBK/936）下中文字符串字面量会被读坏（C2001）
if(MSVC)
    target_compile_options(parser PRIVATE /utf-8)
    target_compile_options(parser_demo PRIVATE /utf-8)
endif()
