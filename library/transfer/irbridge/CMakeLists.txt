cmake_minimum_required(VERSION 3.15)
project(IRBridge LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码是 UTF-8（注释里有中文）：MSVC 要显式 /utf-8，GCC / Clang 指定输入输出字符集
if(MSVC)
  add_compile_options(/utf-8)
else()
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

add_library(irbridge STATIC irbridge.cpp)
target_include_directories(irbridge PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../ir
  ${CMAKE_CURRENT_SOURCE_DIR}/../core)
if(MSVC)
  target_compile_options(irbridge PRIVATE /W4 /utf-8)
else()
  target_compile_options(irbridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(irbridge-tool main.cpp)
set_target_properties(irbridge-tool PROPERTIES OUTPUT_NAME irbridge)
# 单模块构建：把 `irbridgeMain` 包成 `main`（合并库 `t1t` 不定义这个宏）
target_compile_definitions(irbridge-tool PRIVATE TRANSFER_MODULE_STANDALONE_MAIN=1)
target_link_libraries(irbridge-tool PRIVATE irbridge)

enable_testing()
add_executable(irbridge-tests test_irbridge.cpp)
target_link_libraries(irbridge-tests PRIVATE irbridge)
add_test(NAME irbridge-tests COMMAND irbridge-tests)
