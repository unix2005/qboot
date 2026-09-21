add_library(q_log STATIC
    src/q_log.c
)

target_include_directories(q_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(q_log PUBLIC q_core Threads::Threads)
