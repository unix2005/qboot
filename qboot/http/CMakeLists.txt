# libq_http：libevent（IO 多路复用）+ llhttp（HTTP 解析，源码随仓库自带）

find_path(LIBEVENT_INCLUDE_DIR NAMES event2/event.h
    HINTS /usr/local/include /opt/homebrew/include $ENV{LIBEVENT_ROOT}/include)
find_library(LIBEVENT_LIB NAMES event
    HINTS /usr/local/lib /opt/homebrew/lib $ENV{LIBEVENT_ROOT}/lib)
find_library(LIBEVENT_PTHREADS_LIB NAMES event_pthreads
    HINTS /usr/local/lib /opt/homebrew/lib $ENV{LIBEVENT_ROOT}/lib)

if(NOT LIBEVENT_INCLUDE_DIR OR NOT LIBEVENT_LIB)
    message(STATUS "libevent not found -> skip libq_http (install: brew install libevent)")
    return()
endif()

# llhttp 是第三方生成代码，关掉警告避免噪音
add_library(q_llhttp STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/llhttp/llhttp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/llhttp/api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/llhttp/http.c
)
target_include_directories(q_llhttp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../deps/llhttp)
target_compile_options(q_llhttp PRIVATE -w)

add_library(q_http STATIC src/q_http.c)
target_include_directories(q_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(q_http PRIVATE ${LIBEVENT_INCLUDE_DIR})
target_link_libraries(q_http PUBLIC
    q_core q_log q_llhttp
    ${LIBEVENT_LIB} ${LIBEVENT_PTHREADS_LIB}
    Threads::Threads
)

message(STATUS "libq_http enabled, libevent=${LIBEVENT_LIB}")
