cmake_minimum_required(VERSION 3.15)
project(ParseEngineEmbedStackVm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../stackvm ${CMAKE_CURRENT_BINARY_DIR}/stackvm)

add_executable(embed-stackvm main.cpp)
target_link_libraries(embed-stackvm PRIVATE parseengine_stackvm)

enable_testing()
add_executable(embed-stackvm-tests test_embed_stackvm.cpp)
target_link_libraries(embed-stackvm-tests PRIVATE parseengine_stackvm)
add_test(NAME embed-stackvm-tests COMMAND embed-stackvm-tests)

if(MSVC)
  target_compile_options(embed-stackvm PRIVATE /W4 /utf-8)
  target_compile_options(embed-stackvm-tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(embed-stackvm PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(embed-stackvm-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
