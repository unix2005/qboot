# libq_http：libevent（IO 多路复用）+ llhttp（HTTP 解析，源码随仓库自带）
#
# 注意：llhttp 是 q_http 的实现细节，不单独成库、不安装、不导出。
# 它的三个 .c 直接编进 q_http / q_http_shared，
# 这样使用方只需链接 -lq_http，不用知道 llhttp 的存在。

find_path(LIBEVENT_INCLUDE_DIR NAMES event2/event.h
    HINTS /usr/local/include /opt/homebrew/include $ENV{LIBEVENT_ROOT}/include)
find_library(LIBEVENT_LIB NAMES event
    HINTS /usr/local/lib /opt/homebrew/lib $ENV{LIBEVENT_ROOT}/lib)
find_library(LIBEVENT_PTHREADS_LIB NAMES event_pthreads
    HINTS /usr/local/lib /opt/homebrew/lib $ENV{LIBEVENT_ROOT}/lib)

if(NOT LIBEVENT_INCLUDE_DIR OR NOT LIBEVENT_LIB)
    message(STATUS "libevent not found -> skip libq_http (install: apt install libevent-dev)")
    return()
endif()

set(Q_LLHTTP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps/llhttp)
set(Q_LLHTTP_SRC
    ${Q_LLHTTP_DIR}/llhttp.c
    ${Q_LLHTTP_DIR}/api.c
    ${Q_LLHTTP_DIR}/http.c
)

# llhttp 是第三方生成的机器代码，警告全是噪音，只对这三个文件关掉
set_source_files_properties(${Q_LLHTTP_SRC} PROPERTIES COMPILE_OPTIONS "-w")

add_library(q_http STATIC
    src/q_http.c
    ${Q_LLHTTP_SRC}
)
target_include_directories(q_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(q_http PRIVATE ${LIBEVENT_INCLUDE_DIR} ${Q_LLHTTP_DIR})
target_link_libraries(q_http PUBLIC
    q_core q_log
    ${LIBEVENT_LIB} ${LIBEVENT_PTHREADS_LIB}
    Threads::Threads
)

message(STATUS "libq_http enabled, libevent=${LIBEVENT_LIB}")
