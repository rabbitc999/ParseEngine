cmake_minimum_required(VERSION 3.16)
project(big_number LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码是 UTF-8（注释里有中文）：MSVC 要显式 /utf-8，GCC/Clang 指定输入输出字符集。
if(MSVC)
    add_compile_options(/utf-8 /W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

add_library(big_number STATIC bigint.cpp bigfloat.cpp)
target_include_directories(big_number PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(big_number-demo main.cpp)
target_link_libraries(big_number-demo PRIVATE big_number)

add_executable(bigint-tests test_bigint.cpp)
target_link_libraries(bigint-tests PRIVATE big_number)

add_executable(bigfloat-tests test_bigfloat.cpp)
target_link_libraries(bigfloat-tests PRIVATE big_number)

enable_testing()
add_test(NAME bigint-tests COMMAND bigint-tests)
add_test(NAME bigfloat-tests COMMAND bigfloat-tests)
