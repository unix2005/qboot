cmake_minimum_required(VERSION 3.20)
project(qboot VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------- 开关 ----------------
# option() 有缓存：改默认值后必须显式传 -Dxxx=ON/OFF 才会生效

option(Q_BUILD_SHARED  "同时产出动态库（.so / .dylib）"        ON)
option(Q_BUILD_HTTP    "构建 libq_http（需要 libevent）"       ON)
option(Q_BUILD_SAMPLES "构建样例服务"                          ON)
option(Q_BUILD_TOOLS   "构建运维工具 q_upstream_sync"          ON)
option(Q_BUILD_TESTS   "构建单元测试"                          ON)

option(Q_ENABLE_ASAN   "AddressSanitizer"                      OFF)
option(Q_ENABLE_TSAN   "ThreadSanitizer"                       OFF)
option(Q_ENABLE_UBSAN  "UndefinedBehaviorSanitizer"            OFF)

# ---------------- 全局编译设置 ----------------

# 静态库也要 -fPIC，否则链不进动态库
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_C_FLAGS_DEBUG   "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2 -g -DNDEBUG")

add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fno-omit-frame-pointer)
endif()

if(Q_ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()
if(Q_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()
if(Q_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# 产物集中放置，脚本和链接命令引用起来简单
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 动态库装到系统目录后，还要能找到同目录的兄弟库
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(Threads REQUIRED)

# ---------------- 动态库派生 ----------------
#
# 每个静态库自动派生一个同名动态库：
#   q_core (STATIC) -> q_core_shared (SHARED)，两者 OUTPUT_NAME 都是 q_core
# 于是磁盘上同时有 libq_core.a 和 libq_core.so（macOS 上是 libq_core.dylib）。
#
# 依赖必须跟着换成 *_shared：否则 libq_log.so 会把 libq_core.a 静态链进去，
# 应用再链 libq_core.so 就出现两套符号。
function(q_derive_shared tgt)
    if(NOT TARGET ${tgt})
        return()
    endif()
    if(TARGET ${tgt}_shared)
        return()
    endif()

    get_target_property(srcs ${tgt} SOURCES)
    if(NOT srcs)
        return()
    endif()

    # SOURCES 给的是相对路径（相对于定义该 target 的目录），
    # 在顶层直接用会解析不到，这里按 target 自己的源目录补全成绝对路径。
    get_target_property(src_dir ${tgt} SOURCE_DIR)
    set(abs_srcs "")
    foreach(s IN LISTS srcs)
        if(IS_ABSOLUTE "${s}")
            list(APPEND abs_srcs "${s}")
        else()
            list(APPEND abs_srcs "${src_dir}/${s}")
        endif()
    endforeach()

    set(new_deps "")
    get_target_property(deps ${tgt} LINK_LIBRARIES)
    if(deps)
        foreach(d IN LISTS deps)
            if(TARGET ${d} AND TARGET ${d}_shared AND NOT "${d}" STREQUAL "${tgt}")
                list(APPEND new_deps ${d}_shared)
            else()
                list(APPEND new_deps ${d})
            endif()
        endforeach()
    endif()

    add_library(${tgt}_shared SHARED ${abs_srcs})
    set_target_properties(${tgt}_shared PROPERTIES
        OUTPUT_NAME   ${tgt}
        VERSION       ${PROJECT_VERSION}
        SOVERSION     ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON)

    target_include_directories(${tgt}_shared PUBLIC  $<TARGET_PROPERTY:${tgt},INTERFACE_INCLUDE_DIRECTORIES>)
    target_include_directories(${tgt}_shared PRIVATE $<TARGET_PROPERTY:${tgt},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${tgt}_shared PUBLIC  $<TARGET_PROPERTY:${tgt},INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_definitions(${tgt}_shared PRIVATE $<TARGET_PROPERTY:${tgt},COMPILE_DEFINITIONS>)
    target_link_libraries(${tgt}_shared PUBLIC ${new_deps})
endfunction()

# ---------------- 子模块 ----------------

add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(conf)
add_subdirectory(db)
add_subdirectory(mapper)
add_subdirectory(httpc)
add_subdirectory(registry)
if(Q_BUILD_HTTP)
    add_subdirectory(http)
endif()
if(Q_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(Q_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(Q_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------- 动态库统一派生 ----------------

set(QBOOT_ALL_LIBS
    q_core q_log q_conf q_db q_db_mysql q_db_mock
    q_mapper q_httpc q_reg q_http)

if(Q_BUILD_SHARED)
    foreach(t IN LISTS QBOOT_ALL_LIBS)
        q_derive_shared(${t})
    endforeach()
else()
    message(STATUS "qboot: 只构建静态库（Q_BUILD_SHARED=OFF）")
endif()

# ---------------- 安装 ----------------
# cmake --install build  ->  ${CMAKE_INSTALL_PREFIX}/{include,lib,bin,share/qboot}

set(QBOOT_INSTALL_TARGETS "")
foreach(t IN LISTS QBOOT_ALL_LIBS)
    if(TARGET ${t})
        list(APPEND QBOOT_INSTALL_TARGETS ${t})
    endif()
    if(TARGET ${t}_shared)
        list(APPEND QBOOT_INSTALL_TARGETS ${t}_shared)
    endif()
endforeach()

install(TARGETS ${QBOOT_INSTALL_TARGETS}
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# 各模块的 include/q/... 合并装到同一个 include/q/ 下
foreach(m core log conf db mapper httpc registry http)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${m}/include)
        install(DIRECTORY ${m}/include/ DESTINATION include)
    endif()
endforeach()

if(TARGET q_upstream_sync)
    install(TARGETS q_upstream_sync RUNTIME DESTINATION bin)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deploy/
        DESTINATION share/qboot/deploy
        FILES_MATCHING PATTERN "*.conf")

# ---------------- 生成 pkg-config / CMake config 需要的库清单 ----------------

# 只列真正构建出来的库，顺序是"依赖者在前、被依赖者在后"
set(QBOOT_PC_LIBS "")
foreach(n q_http q_mapper q_reg q_httpc q_db_mysql q_db_mock q_db q_conf q_log q_core)
    if(TARGET ${n})
        string(APPEND QBOOT_PC_LIBS " -l${n}")
    endif()
endforeach()

# 标准搜索路径。不在这些目录下的依赖（典型是 brew 的 keg-only 包，
# 比如 /usr/local/opt/mariadb-connector-c/lib）必须额外写 -L，
# 否则做静态链接时 ld 找不到。ld 遇到不存在的 -L 目录只忽略、不报错，
# 所以把构建机的 -L 写进 Libs.private 是安全的（只影响静态链接）。
set(_qboot_std_dirs "/usr/lib" "/usr/lib64" "/lib" "/lib64" "/usr/local/lib"
                    "/usr/lib/x86_64-linux-gnu" "/usr/lib/aarch64-linux-gnu")

set(_qboot_sys_files "")
set(_qboot_sys_flags "")
set(_qboot_sys_ldirs "")
foreach(v LIBEVENT_LIB LIBEVENT_PTHREADS_LIB CURL_LIB LIBXML2_LIB JANSSON_LIB MARIA_LIB)
    if(${v})
        list(APPEND _qboot_sys_files "${${v}}")
        get_filename_component(_n "${${v}}" NAME_WE)
        string(REGEX REPLACE "^lib" "" _n "${_n}")
        list(APPEND _qboot_sys_flags "-l${_n}")
        get_filename_component(_d "${${v}}" DIRECTORY)
        # Xcode SDK 里的路径（macOS 上 LibXml2 常指向 SDK）不用写，
        # 那里只有 .tbd，ld 自己会处理，写进去反而污染 .pc。
        if(_d AND NOT _d IN_LIST _qboot_std_dirs AND NOT _d MATCHES "/[^/]+\\.sdk/")
            list(APPEND _qboot_sys_ldirs "-L${_d}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES _qboot_sys_ldirs)
list(APPEND _qboot_sys_flags "-lpthread")
if(UNIX AND NOT APPLE)
    list(APPEND _qboot_sys_flags "-lm" "-ldl")
endif()
list(REMOVE_DUPLICATES _qboot_sys_flags)
list(REMOVE_DUPLICATES _qboot_sys_files)

string(REPLACE ";" " " QBOOT_PC_PRIVATE_LIBS "${_qboot_sys_ldirs} ${_qboot_sys_flags}")
set(QBOOT_CONFIG_SYS_LIBS "${_qboot_sys_files}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/qboot.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/qboot.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qboot.pc
        DESTINATION lib/pkgconfig)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/qboot-config.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/qboot-config.cmake @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qboot-config.cmake
        DESTINATION lib/cmake/qboot)

message(STATUS "qboot ${PROJECT_VERSION}: build=${CMAKE_BUILD_TYPE} "
               "shared=${Q_BUILD_SHARED} http=${Q_BUILD_HTTP} "
               "asan=${Q_ENABLE_ASAN} tsan=${Q_ENABLE_TSAN}")
