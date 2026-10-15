cmake_minimum_required(VERSION 3.15)
project(ParseEngineToml LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(parseengine_toml STATIC toml.cpp)
target_include_directories(parseengine_toml PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(parseengine_toml PRIVATE /W4 /utf-8)
else()
  target_compile_options(parseengine_toml PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(toml-cli main.cpp)
target_link_libraries(toml-cli PRIVATE parseengine_toml)

add_executable(model-demo examples/model_demo.cpp)
target_link_libraries(model-demo PRIVATE parseengine_toml)

enable_testing()
add_executable(toml-tests test_toml.cpp)
target_link_libraries(toml-tests PRIVATE parseengine_toml)
add_test(NAME toml-tests COMMAND toml-tests)
