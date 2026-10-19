# ParseEngine —— Yacc 风格解析器生成器（C++17 重写版，仅用标准库）
#
# 用法：
#   cmake -S source -B build
#   cmake --build build -j
# 发布模式（CMake >= 3.21）：
#   cd source && cmake --preset release
#   cmake --build --preset release --parallel  # still in source/
# 产物：build/ParseEngine

cmake_minimum_required(VERSION 3.15)
project(ParseEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()
if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

file(GLOB SCRIPT_ENGINE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# 内嵌模板的解压用 library/lztext（不在 src/ 下，所以单独列出，GLOB 抓不到）。
# library/lsp 的三份源码同理：`.gram` 语言服务器内嵌进生成器（`ParseEngine lsp`），
# 独立可执行文件 lsp-server 由 library/lsp/CMakeLists.txt 单独构建。
add_executable(ParseEngine ${SCRIPT_ENGINE_SOURCES}
                           "${CMAKE_CURRENT_SOURCE_DIR}/../library/lztext/lztext.cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../library/lsp/json.cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../library/lsp/grammar.cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../library/lsp/lsp.cpp")
target_include_directories(ParseEngine
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../library")

# 源码是 UTF-8（含中文注释与提示）。Windows 简体中文环境默认代码页是 GBK：
# MSVC 不加 /utf-8 会按 GBK 解码源文件，编译期就把中文串读坏；GCC 则显式
# 钉死输入/执行字符集，避免换编译器或语言环境后产物里的中文变乱码。
if(MSVC)
    target_compile_options(ParseEngine PRIVATE /W4 /utf-8)
else()
    target_compile_options(ParseEngine PRIVATE -Wall -Wextra)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(ParseEngine PRIVATE
                               -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
    # GCC 8 及更早版本需要显式链接 stdc++fs 才能用 <filesystem>
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(ParseEngine PRIVATE stdc++fs)
    endif()
endif()

# 模板内联：source/templates/*.tmpl 由 tools/embed_templates.py 生成到
# source/src/templates_data.cpp，仓库里已包含生成结果；改模板后重新跑一次即可。
# 手册内联同理：doc/ 的中英正文由 tools/embed_manual.py 生成到
# source/src/manual_data.cpp，供 `ParseEngine man` 离线查询。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/tool_console.hpp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/sync_tool_console.py"
        DEPENDS ../library/console/utf8.hpp ../tools/sync_tool_console.py)
    add_custom_command(OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/i18n_messages.hpp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_parseengine_i18n.py"
        DEPENDS i18n/messages.tsv ../tools/embed_parseengine_i18n.py ../tools/embed_i18n.py)
    add_custom_command(OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/package_tool.hpp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_package_tool.py"
        DEPENDS ../tools/package_compiler.py ../tools/embed_package_tool.py)
    add_custom_target(embed_tool_assets DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/tool_console.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/i18n_messages.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ParseEngine/package_tool.hpp")
    add_dependencies(ParseEngine embed_tool_assets)
    add_custom_target(embed_templates
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_templates.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        COMMENT "把 source/templates/*.tmpl 内联成 source/src/templates_data.cpp")
    add_custom_target(embed_manual
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed_manual.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        COMMENT "把 doc/ 的手册正文内联成 source/src/manual_data.cpp")
endif()

find_package(Threads REQUIRED)
target_link_libraries(ParseEngine PRIVATE Threads::Threads)
