cmake_minimum_required(VERSION 3.15)
project(Dataflow LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码是 UTF-8（注释里有中文）：MSVC 要显式 /utf-8，GCC / Clang 指定输入输出字符集
if(MSVC)
  add_compile_options(/utf-8)
else()
  add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# 共享 IR（`../ir/typed_ir.cpp`）在合并库里只有一份，单模块构建也要编进来
add_library(dataflow STATIC dataflow.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../ir/typed_ir.cpp)
target_include_directories(dataflow PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../ir
  ${CMAKE_CURRENT_SOURCE_DIR}/../core)
if(MSVC)
  target_compile_options(dataflow PRIVATE /W4 /utf-8)
else()
  target_compile_options(dataflow PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(dataflow-tool main.cpp)
set_target_properties(dataflow-tool PROPERTIES OUTPUT_NAME dataflow)
# 单模块构建：把 `dataflowMain` 包成 `main`（合并库 `t1t` 不定义这个宏）
target_compile_definitions(dataflow-tool PRIVATE TRANSFER_MODULE_STANDALONE_MAIN=1)
target_link_libraries(dataflow-tool PRIVATE dataflow)

add_executable(dataflow-examples example_suite.cpp)
target_link_libraries(dataflow-examples PRIVATE dataflow)

enable_testing()
add_executable(dataflow-tests test_dataflow.cpp)
target_link_libraries(dataflow-tests PRIVATE dataflow)
add_test(NAME dataflow-tests COMMAND dataflow-tests)
add_test(NAME dataflow-examples COMMAND dataflow-examples ${CMAKE_CURRENT_SOURCE_DIR}/examples)
