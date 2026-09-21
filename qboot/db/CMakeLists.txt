# libq_db：统一数据库访问抽象 + 各数据库驱动

add_library(q_db STATIC
    src/db.c
)

target_include_directories(q_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(q_db PUBLIC q_core q_log Threads::Threads)

# ---- 内存假驱动：无真实数据库时的自测 / 演示用，无外部依赖，始终构建 ----

add_library(q_db_mock STATIC driver/mock/q_db_mock.c)
target_link_libraries(q_db_mock PUBLIC q_db)

message(STATUS "libq_db_mock enabled (in-memory fake driver)")

# ---- MySQL 驱动（MariaDB Connector/C）----

find_path(MARIA_INCLUDE NAMES mysql.h
    PATH_SUFFIXES mariadb mysql
    HINTS /usr/local/opt/mariadb-connector-c/include
          /opt/homebrew/opt/mariadb-connector-c/include
          /usr/local/include
          $ENV{MARIA_ROOT}/include)

find_library(MARIA_LIB NAMES mariadb mysqlclient
    HINTS /usr/local/opt/mariadb-connector-c/lib
          /opt/homebrew/opt/mariadb-connector-c/lib
          /usr/local/lib
          $ENV{MARIA_ROOT}/lib)

if(NOT MARIA_INCLUDE OR NOT MARIA_LIB)
    message(STATUS "mariadb connector not found -> skip libq_db_mysql (brew install mariadb-connector-c)")
    return()
endif()

add_library(q_db_mysql STATIC driver/mysql/q_db_mysql.c)
target_include_directories(q_db_mysql PRIVATE ${MARIA_INCLUDE})
target_link_libraries(q_db_mysql PUBLIC q_db ${MARIA_LIB})

message(STATUS "libq_db_mysql enabled, mariadb=${MARIA_LIB}")
