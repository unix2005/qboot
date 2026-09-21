# libq_httpc：出站 HTTP 客户端（服务注册 / 服务间调用共用）

find_path(CURL_INCLUDE_DIR NAMES curl/curl.h
    HINTS /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include
          /usr/local/opt/curl/include
          /opt/homebrew/opt/curl/include
          /usr/include)
find_library(CURL_LIB NAMES curl
    HINTS /usr/local/opt/curl/lib /opt/homebrew/opt/curl/lib /usr/lib)

if(NOT CURL_INCLUDE_DIR OR NOT CURL_LIB)
    message(STATUS "libcurl not found -> skip libq_httpc")
    return()
endif()

add_library(q_httpc STATIC src/q_httpc.c)
target_include_directories(q_httpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(q_httpc PRIVATE ${CURL_INCLUDE_DIR})
target_link_libraries(q_httpc PUBLIC q_core ${CURL_LIB})

message(STATUS "libq_httpc enabled, curl=${CURL_LIB}")
