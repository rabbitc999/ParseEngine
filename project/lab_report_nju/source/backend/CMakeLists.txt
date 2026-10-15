cmake_minimum_required(VERSION 3.15)
project(TransferOneThread LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(T1T_INCLUDES core ir irbridge irbuild dataflow ssa regalloc app)

add_library(transfer-one-thread STATIC
  core/types.cpp core/transfer.cpp core/optimizer.cpp core/config.cpp
  irbridge/irbridge.cpp dataflow/dataflow.cpp ssa/ssa.cpp regalloc/regalloc.cpp
  ir/typed_ir.cpp irbuild/irbuild.cpp app/pipeline.cpp)
foreach(dir ${T1T_INCLUDES})
  target_include_directories(transfer-one-thread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
endforeach()
target_link_libraries(transfer-one-thread PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(transfer-one-thread PRIVATE /W4 /utf-8)
else()
  target_compile_options(transfer-one-thread PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(t1t
  app/main.cpp app/transfer_main.cpp
  irbridge/main.cpp dataflow/main.cpp ssa/main.cpp regalloc/main.cpp irbuild/main.cpp)

# typebuild / astbuild（类型构造库与 AST 构建库）已于 2026-09-19 删除，
# 这里只剩本库一个静态库（命令行 t1t 的 transfer 子命令）。
target_link_libraries(t1t PRIVATE transfer-one-thread)

# 各模块的单元测试与示例集
function(t1t_add_tool name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE transfer-one-thread)
endfunction()

t1t_add_tool(t1t-tests-irbridge irbridge/test_irbridge.cpp)
t1t_add_tool(t1t-tests-dataflow dataflow/test_dataflow.cpp)
t1t_add_tool(t1t-tests-ssa ssa/test_ssa.cpp)
t1t_add_tool(t1t-tests-regalloc regalloc/test_regalloc.cpp)
t1t_add_tool(t1t-tests-irbuild irbuild/test_irbuild.cpp)
t1t_add_tool(t1t-example core/example.cpp)
t1t_add_tool(t1t-example-config core/example_config.cpp)
t1t_add_tool(t1t-example-operators core/example_operators.cpp)
t1t_add_tool(t1t-example-suite core/example_suite.cpp)
t1t_add_tool(t1t-dataflow-suite dataflow/example_suite.cpp)
t1t_add_tool(t1t-ssa-suite ssa/example_suite.cpp)
t1t_add_tool(t1t-pipeline-suite examples/pipeline_suite.cpp)

enable_testing()
add_test(NAME irbridge-tests COMMAND t1t-tests-irbridge)
add_test(NAME dataflow-tests COMMAND t1t-tests-dataflow)
add_test(NAME ssa-tests COMMAND t1t-tests-ssa)
add_test(NAME regalloc-tests COMMAND t1t-tests-regalloc)
add_test(NAME irbuild-tests COMMAND t1t-tests-irbuild)
add_test(NAME dataflow-examples COMMAND t1t-dataflow-suite ${CMAKE_CURRENT_SOURCE_DIR}/dataflow/examples)
add_test(NAME ssa-examples COMMAND t1t-ssa-suite ${CMAKE_CURRENT_SOURCE_DIR}/ssa/examples)
add_test(NAME pipeline-examples COMMAND t1t-pipeline-suite ${CMAKE_CURRENT_SOURCE_DIR}/examples)
