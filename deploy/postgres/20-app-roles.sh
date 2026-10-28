#!/bin/sh
# Роли приложения для docker-compose (выполняется postgres-образом один раз,
# при инициализации пустого тома, после 10-schema.sql).
#
#   selfvoy_app — C++-сервер: только DML (SELECT/INSERT/UPDATE/DELETE) и
#              последовательности; не суперпользователь, без DDL.
#   selfvoy_ai  — AI-сервис: только своя память (user_memory) и users(id, email)
#              для поиска владельца записи.
#
# Пароли — из окружения контейнера (SELFVOY_DB_PASSWORD, SELFVOY_AI_DB_PASSWORD),
# в SQL передаются psql-переменными (:'var' экранирует как литерал).
# Без «set -u»: если файл не исполняемый, entrypoint его source-ит.
set -e

if [ -z "${SELFVOY_DB_PASSWORD:-}" ] || [ -z "${SELFVOY_AI_DB_PASSWORD:-}" ]; then
    echo "20-app-roles.sh: SELFVOY_DB_PASSWORD и SELFVOY_AI_DB_PASSWORD обязательны" >&2
    exit 1
fi

psql -v ON_ERROR_STOP=1 \
     --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
     -v app_password="$SELFVOY_DB_PASSWORD" \
     -v ai_password="$SELFVOY_AI_DB_PASSWORD" \
     -v db_name="$POSTGRES_DB" \
     -v owner="${POSTGRES_USER:-postgres}" <<'SQL'
CREATE ROLE selfvoy_app LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION PASSWORD :'app_password';
CREATE ROLE selfvoy_ai  LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION PASSWORD :'ai_password';

REVOKE ALL ON DATABASE :"db_name" FROM PUBLIC;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
GRANT CONNECT ON DATABASE :"db_name" TO selfvoy_app, selfvoy_ai;
GRANT USAGE ON SCHEMA public TO selfvoy_app, selfvoy_ai;

-- C++-сервер: данные всех таблиц, без DDL.
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO selfvoy_app;
GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO selfvoy_app;
-- Таблицы из будущих миграций (их применяет владелец схемы, POSTGRES_USER).
ALTER DEFAULT PRIVILEGES FOR ROLE :"owner" IN SCHEMA public
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO selfvoy_app;
ALTER DEFAULT PRIVILEGES FOR ROLE :"owner" IN SCHEMA public
    GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO selfvoy_app;

-- AI-сервис: только память и сопоставление владельца (без хэшей паролей).
GRANT SELECT (id, email) ON users TO selfvoy_ai;
GRANT SELECT, INSERT, UPDATE ON user_memory TO selfvoy_ai;
GRANT USAGE ON SEQUENCE user_memory_id_seq TO selfvoy_ai;
SQL
