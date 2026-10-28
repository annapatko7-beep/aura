cmake_minimum_required(VERSION 3.21)

project(selfvoy-server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SELFVOY_WITH_LIBPQ "Собирать DatabaseManager поверх PostgreSQL (libpq)" ON)
option(SELFVOY_BUILD_TESTS "Собирать юнит-тесты" ON)

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

set(SELFVOY_SOURCES
    src/adminmanager.cpp
    src/invitemanager.cpp
    src/bookingmanager.cpp
    src/calendarfeed.cpp
    src/slotspost.cpp
    src/payments.cpp
    src/packages.cpp
    src/waitlist.cpp
    src/tax.cpp
    src/referralmanager.cpp
    src/schedulelock.cpp
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
    src/smtp.cpp
    src/tls.cpp
    src/plan.cpp
    src/telegrambot.cpp
    src/telegramadmin.cpp
    src/memorymanager.cpp
    src/net.cpp
    src/notificationsmanager.cpp
    src/oauthauthmanager.cpp
    src/protocol.cpp
    src/server.cpp
    src/session.cpp
    src/taskmanager.cpp
    src/toolmanager.cpp
    src/totp.cpp
    src/ws.cpp
    src/federation/address.cpp
)

set(SELFVOY_HEADERS
    include/selfvoy/adminmanager.h
    include/selfvoy/invitemanager.h
    include/selfvoy/bookingmanager.h
    include/selfvoy/calendarfeed.h
    include/selfvoy/slotspost.h
    include/selfvoy/referralmanager.h
    include/selfvoy/schedulelock.h
    include/selfvoy/agentmanager.h
    include/selfvoy/authmanager.h
    include/selfvoy/chatmanager.h
    include/selfvoy/config.h
    include/selfvoy/connectionmanager.h
    include/selfvoy/crypto.h
    include/selfvoy/databasemanager.h
    include/selfvoy/idatabase.h
    include/selfvoy/json.h
    include/selfvoy/jwt.h
    include/selfvoy/listener.h
    include/selfvoy/log.h
    include/selfvoy/memorymanager.h
    include/selfvoy/net.h
    include/selfvoy/protocol.h
    include/selfvoy/server.h
    include/selfvoy/session.h
    include/selfvoy/taskmanager.h
    include/selfvoy/toolmanager.h
    include/selfvoy/ws.h
    include/selfvoy/tls.h
    include/selfvoy/plan.h
    include/selfvoy/telegrambot.h
    include/selfvoy/telegramadmin.h
)

add_library(selfvoy-core STATIC ${SELFVOY_SOURCES} ${SELFVOY_HEADERS})
target_include_directories(selfvoy-core PUBLIC include)
target_link_libraries(selfvoy-core PUBLIC Threads::Threads argon2)
target_compile_options(selfvoy-core PRIVATE -Wall -Wextra)

# PostgreSQL: без libpq сервер собирается и работает на встроенном хранилище.
if(SELFVOY_WITH_LIBPQ)
    find_package(PostgreSQL)
    if(PostgreSQL_FOUND)
        target_compile_definitions(selfvoy-core PUBLIC SELFVOY_WITH_LIBPQ)
        target_include_directories(selfvoy-core PUBLIC ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(selfvoy-core PUBLIC PostgreSQL::PostgreSQL)
        message(STATUS "selfvoy-server: PostgreSQL поддержан (${PostgreSQL_VERSION_STRING})")
    else()
        message(WARNING "selfvoy-server: libpq не найден — собираюсь без PostgreSQL")
    endif()
endif()

# OpenSSL: TLS для SMTP (SELFVOY_MAIL_DRIVER=smtp) и https-запросов (Telegram Bot API,
# Google). Без него сервер собирается, но письма можно слать только через локальный
# ретранслятор (SELFVOY_SMTP_TLS=none), а https-запросы возвращают ошибку «сборка без TLS».
find_package(OpenSSL)
if(OpenSSL_FOUND)
    target_compile_definitions(selfvoy-core PUBLIC SELFVOY_HAVE_OPENSSL)
    target_link_libraries(selfvoy-core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "selfvoy-server: TLS (SMTP, https) — OpenSSL ${OPENSSL_VERSION}")
else()
    message(WARNING "selfvoy-server: OpenSSL не найден — SMTP и https без TLS")
endif()

add_executable(selfvoy-server src/main.cpp)
target_link_libraries(selfvoy-server PRIVATE selfvoy-core)

if(SELFVOY_BUILD_TESTS)
    enable_testing()
    add_executable(selfvoy-tests tests/test_main.cpp)
    target_link_libraries(selfvoy-tests PRIVATE selfvoy-core)
    add_test(NAME selfvoy-tests COMMAND selfvoy-tests)
endif()

install(TARGETS selfvoy-server RUNTIME DESTINATION bin)
