add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE q_core)

add_executable(test_log test_log.c)
target_link_libraries(test_log PRIVATE q_log q_core)

add_executable(test_conf test_conf.c)
target_link_libraries(test_conf PRIVATE q_conf q_core)

if(TARGET q_reg AND TARGET q_http)
    add_executable(test_reg test_reg.c)
    target_link_libraries(test_reg PRIVATE q_reg q_httpc q_http q_core q_log)
endif()

if(TARGET q_mapper)
    add_executable(test_mapper test_mapper.c)
    target_link_libraries(test_mapper PRIVATE q_mapper q_db_mock q_db q_log q_core)
    target_compile_definitions(test_mapper PRIVATE
        Q_DEFAULT_XML="${CMAKE_CURRENT_SOURCE_DIR}/mapper/User.xml")
endif()
