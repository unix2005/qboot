add_library(q_conf STATIC
    src/q_conf.c
)

target_include_directories(q_conf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(q_conf PUBLIC q_core)
