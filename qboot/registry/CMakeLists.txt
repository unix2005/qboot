# libq_reg：服务注册与发现（Consul）

find_path(JANSSON_INCLUDE_DIR NAMES jansson.h
    HINTS /usr/local/include /opt/homebrew/include)
find_library(JANSSON_LIB NAMES jansson
    HINTS /usr/local/lib /opt/homebrew/lib)

if(NOT TARGET q_httpc)
    message(STATUS "libq_httpc missing -> skip libq_reg")
    return()
endif()
if(NOT JANSSON_INCLUDE_DIR OR NOT JANSSON_LIB)
    message(STATUS "jansson not found -> skip libq_reg")
    return()
endif()

add_library(q_reg STATIC src/q_reg.c)
target_include_directories(q_reg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(q_reg PRIVATE ${JANSSON_INCLUDE_DIR})
target_link_libraries(q_reg PUBLIC q_httpc q_core q_log ${JANSSON_LIB} Threads::Threads)

message(STATUS "libq_reg enabled (consul)")
