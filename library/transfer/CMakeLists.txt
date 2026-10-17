cmake_minimum_required(VERSION 3.15)
project(TransferOneThread LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(T1T_HYDRA86_ROOT "" CACHE PATH
    "Hydra86 source tree (standalone repo); leave missing to build assembly generation only")
set(T1T_HAVE_HYDRA86 OFF)
# Hydra86 已拆分为独立仓库。查找顺序：
#   1. 显式 T1T_HYDRA86_ROOT（-DT1T_HYDRA86_ROOT=... 或环境变量）
#   2. 本仓库同级目录 ../hydra-86（克隆 ParseEngine 时旁边放着 hydra-86 的常见布局）
# 都找不到时按原降级行为：hydra86 汇编生成仍然可用，只是不嵌入执行。
if(NOT T1T_HYDRA86_ROOT AND DEFINED ENV{T1T_HYDRA86_ROOT})
  set(T1T_HYDRA86_ROOT "$ENV{T1T_HYDRA86_ROOT}")
endif()
if(NOT T1T_HYDRA86_ROOT)
  set(_t1t_hydra86_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../../hydra-86"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../hydra-86")
  foreach(_t1t_hydra86_dir ${_t1t_hydra86_candidates})
    if(EXISTS "${_t1t_hydra86_dir}/CMakeLists.txt")
      set(T1T_HYDRA86_ROOT "${_t1t_hydra86_dir}")
      break()
    endif()
  endforeach()
endif()
if(T1T_HYDRA86_ROOT AND EXISTS "${T1T_HYDRA86_ROOT}/CMakeLists.txt")
  set(_t1t_build_testing "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  add_subdirectory("${T1T_HYDRA86_ROOT}" "${CMAKE_BINARY_DIR}/hydra86" EXCLUDE_FROM_ALL)
  if(MSVC AND TARGET pvm_flags)
    target_compile_options(pvm_flags INTERFACE /utf-8)
  endif()
  set(BUILD_TESTING "${_t1t_build_testing}")
  set(T1T_HAVE_HYDRA86 ON)
  message(STATUS "transfer: Hydra86 embedded from ${T1T_HYDRA86_ROOT}")
else()
  message(STATUS "transfer: Hydra86 source not found; hydra86 assembly generation remains available")
endif()

set(T1T_INCLUDES core ir irbridge irbuild dataflow ssa regalloc app)

add_library(transfer-one-thread STATIC
  core/types.cpp core/transfer.cpp core/optimizer.cpp core/config.cpp core/hydra86.cpp core/llvm.cpp
  core/llvm_hydra.cpp
  irbridge/irbridge.cpp dataflow/dataflow.cpp ssa/ssa.cpp regalloc/regalloc.cpp
  ir/typed_ir.cpp irbuild/irbuild.cpp app/pipeline.cpp)
foreach(dir ${T1T_INCLUDES})
  target_include_directories(transfer-one-thread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
endforeach()
target_link_libraries(transfer-one-thread PUBLIC Threads::Threads)
if(T1T_HAVE_HYDRA86)
  target_compile_definitions(transfer-one-thread PRIVATE T1T_WITH_HYDRA86=1)
  target_link_libraries(transfer-one-thread PUBLIC pvm_cli)
endif()
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
if(MSVC)
  target_compile_options(t1t PRIVATE /utf-8)
endif()

# 各模块的单元测试与示例集
function(t1t_add_tool name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE transfer-one-thread)
  if(MSVC)
    target_compile_options(${name} PRIVATE /utf-8)
  endif()
endfunction()

t1t_add_tool(t1t-tests-irbridge irbridge/test_irbridge.cpp)
t1t_add_tool(t1t-tests-dataflow dataflow/test_dataflow.cpp)
t1t_add_tool(t1t-tests-ssa ssa/test_ssa.cpp)
t1t_add_tool(t1t-tests-regalloc regalloc/test_regalloc.cpp)
t1t_add_tool(t1t-tests-irbuild irbuild/test_irbuild.cpp)
t1t_add_tool(t1t-tests-hydra86 core/hydra86_test.cpp)
t1t_add_tool(t1t-tests-llvm core/llvm_test.cpp)
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
add_test(NAME hydra86-tests COMMAND t1t-tests-hydra86)
add_test(NAME llvm-tests COMMAND t1t-tests-llvm)
add_test(NAME dataflow-examples COMMAND t1t-dataflow-suite ${CMAKE_CURRENT_SOURCE_DIR}/dataflow/examples)
add_test(NAME ssa-examples COMMAND t1t-ssa-suite ${CMAKE_CURRENT_SOURCE_DIR}/ssa/examples)
add_test(NAME pipeline-examples COMMAND t1t-pipeline-suite ${CMAKE_CURRENT_SOURCE_DIR}/examples)

find_program(T1T_CLANG NAMES clang clang.exe HINTS "C:/Program Files/LLVM/bin")
if(T1T_CLANG)
  add_test(NAME llvm-clang-e2e
    COMMAND ${CMAKE_COMMAND}
      "-DT1T=$<TARGET_FILE:t1t>"
      "-DCLANG=${T1T_CLANG}"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/01_arith.quad"
      "-DRECURSION_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm_recursion.quad"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/llvm-e2e"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/core/llvm_e2e.cmake)
endif()
