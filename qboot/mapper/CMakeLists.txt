# libq_mapper：XML 外置 SQL + 动态标签 + 结果映射

find_path(LIBXML2_INCLUDE_DIR NAMES libxml/parser.h
    HINTS /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/libxml2
          /usr/local/opt/libxml2/include/libxml2
          /opt/homebrew/opt/libxml2/include/libxml2
          /usr/include/libxml2)
find_library(LIBXML2_LIB NAMES xml2)

find_path(JANSSON_INCLUDE_DIR NAMES jansson.h
    HINTS /usr/local/include /opt/homebrew/include)
find_library(JANSSON_LIB NAMES jansson
    HINTS /usr/local/lib /opt/homebrew/lib)

if(NOT LIBXML2_INCLUDE_DIR OR NOT LIBXML2_LIB)
    message(STATUS "libxml2 not found -> skip libq_mapper")
    return()
endif()
if(NOT JANSSON_INCLUDE_DIR OR NOT JANSSON_LIB)
    message(STATUS "jansson not found -> skip libq_mapper (brew install jansson)")
    return()
endif()

add_library(q_mapper STATIC
    src/loader.c
    src/expr.c
    src/mapper.c
)

target_include_directories(q_mapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 公共头文件引用了 jansson.h，因此 jansson 的 include 目录也要 PUBLIC
target_include_directories(q_mapper PUBLIC ${JANSSON_INCLUDE_DIR})
target_include_directories(q_mapper PRIVATE ${LIBXML2_INCLUDE_DIR})
target_link_libraries(q_mapper PUBLIC q_db q_log q_core ${LIBXML2_LIB} ${JANSSON_LIB})

message(STATUS "libq_mapper enabled (libxml2 + jansson)")
