cmake_minimum_required(VERSION 3.15)
project(ParseEngineTemplate LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 纯头文件库：include 目录就是本目录（用的是 <> 之外的相对路径写法）。
add_library(parseengine_template INTERFACE)
target_include_directories(parseengine_template INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(template-demo main.cpp)
target_link_libraries(template-demo PRIVATE parseengine_template)

add_executable(template-tests test_template.cpp)
target_link_libraries(template-tests PRIVATE parseengine_template)

if(MSVC)
  target_compile_options(template-demo PRIVATE /W4 /utf-8)
  target_compile_options(template-tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(template-demo PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(template-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME template-tests COMMAND template-tests)
