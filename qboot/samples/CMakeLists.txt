add_executable(demo_svc demo_svc.c)
target_link_libraries(demo_svc PRIVATE q_core q_log q_conf)

if(TARGET q_http)
    add_executable(http_demo http_demo.c)
    target_link_libraries(http_demo PRIVATE q_http q_log q_conf q_core)
endif()

if(TARGET q_mapper AND TARGET q_http AND TARGET q_reg)
    add_executable(user_svc user_svc.c)
    target_link_libraries(user_svc PRIVATE
        q_http q_mapper q_db q_db_mock q_reg q_httpc q_conf q_log q_core)
    # MySQL 驱动是可选的：没装 connector-c 就只注册 mock 驱动
    if(TARGET q_db_mysql)
        target_link_libraries(user_svc PRIVATE q_db_mysql)
        target_compile_definitions(user_svc PRIVATE Q_HAVE_MYSQL=1)
    endif()
endif()
