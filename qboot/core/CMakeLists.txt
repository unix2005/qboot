add_library(q_core STATIC
    src/mem.c
    src/str.c
    src/ds.c
    src/time.c
    src/threadpool.c
    src/types.c
)

target_include_directories(q_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(q_core PUBLIC Threads::Threads)
