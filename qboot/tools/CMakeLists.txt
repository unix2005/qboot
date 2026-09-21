# 运维小工具

if(TARGET q_reg)
    add_executable(q_upstream_sync q_upstream_sync.c)
    target_link_libraries(q_upstream_sync PRIVATE q_reg q_httpc q_core q_log)
endif()
