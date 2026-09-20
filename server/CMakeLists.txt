cmake_minimum_required(VERSION 3.21)

project(aura-server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AURA_WITH_LIBPQ "Собирать DatabaseManager поверх PostgreSQL (libpq)" ON)
option(AURA_BUILD_TESTS "Собирать юнит-тесты" ON)

find_package(Threads REQUIRED)

# --- vendored libargon2 (PHC winner, CC0/Apache-2.0) ------------------------
# Эталонная реализация Argon2id для хэширования паролей. Собирается как
# отдельная статическая библиотека, чтобы не тащить её предупреждения в ядро.
set(ARGON2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2)
add_library(argon2 STATIC
    ${ARGON2_DIR}/src/argon2.c
    ${ARGON2_DIR}/src/core.c
    ${ARGON2_DIR}/src/encoding.c
    ${ARGON2_DIR}/src/ref.c
    ${ARGON2_DIR}/src/thread.c
    ${ARGON2_DIR}/src/blake2/blake2b.c
)
target_include_directories(argon2 PUBLIC ${ARGON2_DIR}/include ${ARGON2_DIR}/src)
target_compile_options(argon2 PRIVATE -w)

set(AURA_SOURCES
    src/agentmanager.cpp
    src/authmanager.cpp
    src/chatmanager.cpp
    src/config.cpp
    src/connectionmanager.cpp
    src/crypto.cpp
    src/database_embedded.cpp
    src/database_pg.cpp
    src/database_records.cpp
    src/databasemanager.cpp
    src/integrationmanager.cpp
    src/json.cpp
    src/jwt.cpp
    src/listener.cpp
    src/log.cpp
    src/mailprovider.cpp
    src/memorymanager.cpp
    src/net.cpp
    src/protocol.cpp
    src/server.cpp
    src/session.cpp
    src/taskmanager.cpp
    src/toolmanager.cpp
    src/totp.cpp
    src/ws.cpp
)

set(AURA_HEADERS
    include/aura/agentmanager.h
    include/aura/authmanager.h
    include/aura/chatmanager.h
    include/aura/config.h
    include/aura/connectionmanager.h
    include/aura/crypto.h
    include/aura/databasemanager.h
    include/aura/idatabase.h
    include/aura/json.h
    include/aura/jwt.h
    include/aura/listener.h
    include/aura/log.h
    include/aura/memorymanager.h
    include/aura/net.h
    include/aura/protocol.h
    include/aura/server.h
    include/aura/session.h
    include/aura/taskmanager.h
    include/aura/toolmanager.h
    include/aura/ws.h
)

add_library(aura-core STATIC ${AURA_SOURCES} ${AURA_HEADERS})
target_include_directories(aura-core PUBLIC include)
target_link_libraries(aura-core PUBLIC Threads::Threads argon2)
target_compile_options(aura-core PRIVATE -Wall -Wextra)

# PostgreSQL: без libpq сервер собирается и работает на встроенном хранилище.
if(AURA_WITH_LIBPQ)
    find_package(PostgreSQL)
    if(PostgreSQL_FOUND)
        target_compile_definitions(aura-core PUBLIC AURA_WITH_LIBPQ)
        target_include_directories(aura-core PUBLIC ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(aura-core PUBLIC PostgreSQL::PostgreSQL)
        message(STATUS "aura-server: PostgreSQL поддержан (${PostgreSQL_VERSION_STRING})")
    else()
        message(WARNING "aura-server: libpq не найден — собираюсь без PostgreSQL")
    endif()
endif()

add_executable(aura-server src/main.cpp)
target_link_libraries(aura-server PRIVATE aura-core)

if(AURA_BUILD_TESTS)
    enable_testing()
    add_executable(aura-tests tests/test_main.cpp)
    target_link_libraries(aura-tests PRIVATE aura-core)
    add_test(NAME aura-tests COMMAND aura-tests)
endif()

install(TARGETS aura-server RUNTIME DESTINATION bin)
