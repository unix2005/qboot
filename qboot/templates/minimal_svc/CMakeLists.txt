# 最小服务模板 —— 用 find_package(qboot) 接入
#
# 静态链接（默认）：
#   cmake -S . -B build -Dqboot_DIR=/usr/local/lib/cmake/qboot
#   cmake --build build
#
# 动态链接：
#   cmake -S . -B build -Dqboot_DIR=/usr/local/lib/cmake/qboot -DQBOOT_USE_SHARED=ON

cmake_minimum_required(VERSION 3.20)
project(minimal_svc C)

option(QBOOT_USE_SHARED "链接 qboot 动态库（OFF=静态 .a，ON=动态 .so）" OFF)

find_package(qboot REQUIRED)

# 只链接这一组就够了：q_http 会自己把 q_log / q_core 带上，
# 但显式写出来可读性更好，也不怕以后依赖变了。
set(QBOOT_LIBS q_http q_conf q_log q_core)

if(QBOOT_USE_SHARED)
    set(_suffix "_shared")
else()
    set(_suffix "")
endif()

set(_link "")
foreach(lib IN LISTS QBOOT_LIBS)
    list(APPEND _link "qboot::${lib}${_suffix}")
endforeach()

add_executable(minimal_svc minimal_svc.c)
target_link_libraries(minimal_svc PRIVATE ${_link})

if(QBOOT_USE_SHARED)
    # 动态库部署后要能找到 .so，写死 rpath 到安装目录
    set_target_properties(minimal_svc PROPERTIES
        BUILD_RPATH "${QBOOT_LIBRARY_DIR}"
        INSTALL_RPATH "${QBOOT_LIBRARY_DIR}")
endif()
