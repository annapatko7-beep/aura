#!/usr/bin/env bash
# Управление установленным Selfvoy (ставится как команда «selfvoy» скриптом install.sh).
#
#   selfvoy status            — состояние контейнеров и модели LLM
#   selfvoy logs [сервис]     — последние строки логов (server | ai | postgres | caddy)
#   selfvoy verify EMAIL      — подтвердить email вручную (если почта не настроена)
#   selfvoy pro EMAIL DAYS    — выдать тариф Pro на DAYS дней (0 — снять Pro)
#   selfvoy llm-key           — вставить/сменить API-ключ LLM и проверить модель
#   selfvoy llm-check         — проверить, что отвечает настоящая модель
#   selfvoy backup            — резервная копия базы в backups/ (хранятся 14 последних)
#   selfvoy update            — скачать обновление и пересобрать
#   selfvoy restart [сервис]  — перезапустить
#   selfvoy migrate           — перевести установку прежней версии на имена Selfvoy
set -euo pipefail

SELFVOY_DIR=${SELFVOY_DIR:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}
cd "$SELFVOY_DIR"

env_get() { grep -E "^$1=" .env 2>/dev/null | tail -1 | cut -d= -f2- || true; }
env_set() {
    local key=$1 value=$2
    if grep -qE "^$key=" .env; then
        awk -v k="$key" -v v="$value" 'BEGIN{FS=OFS="="} $1==k {print k "=" v; next} {print}' .env > .env.tmp
        mv .env.tmp .env
    else
        printf '%s=%s\n' "$key" "$value" >> .env
    fi
    chmod 600 .env
}

# Переход с прежнего имени продукта (Aura), один релиз: ключи AURA_* в .env
# переименовываются в SELFVOY_*; уже заданный SELFVOY_* не перезаписывается.
migrate_env() {
    [ -f .env ] && grep -qE '^AURA_' .env || return 0
    awk -F= 'NR == FNR { if ($1 ~ /^SELFVOY_/) have[$1] = 1; next }
             /^AURA_/ { key = $1; sub(/^AURA_/, "SELFVOY_", key); if (key in have) next; sub(/^AURA_/, "SELFVOY_") }
             { print }' .env .env > .env.tmp
    mv .env.tmp .env
    chmod 600 .env
    echo "В .env переменные AURA_* переименованы в SELFVOY_*."
}
migrate_env

compose=(docker compose -f docker-compose.yml)
[ -n "$(env_get SELFVOY_DOMAIN)" ] && compose+=(-f deploy/compose.tls.yml)

pg_query() { "${compose[@]}" exec -T postgres psql -v ON_ERROR_STOP=1 -U postgres -d postgres -tAc "$1"; }
pg_has() { [ "$(pg_query "SELECT 1 FROM $1 WHERE $2 = '$3'" | tr -d '[:space:]')" = 1 ]; }

# Том PostgreSQL, созданный до переименования: база aura → selfvoy, роли
# aura_app/aura_ai → selfvoy_app/selfvoy_ai (SCRAM-пароли при этом сохраняются).
migrate_db() {
    "${compose[@]}" up -d postgres >/dev/null
    for _ in $(seq 1 30); do
        "${compose[@]}" exec -T postgres pg_isready -U postgres -d postgres >/dev/null 2>&1 && break
        sleep 2
    done
    if pg_has pg_database datname aura && ! pg_has pg_database datname selfvoy; then
        echo "Переименовываю базу aura → selfvoy"
        "${compose[@]}" stop server ai >/dev/null 2>&1 || true
        pg_query "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = 'aura' AND pid <> pg_backend_pid()" >/dev/null
        pg_query "ALTER DATABASE aura RENAME TO selfvoy" >/dev/null
    fi
    for role in app ai; do
        if pg_has pg_roles rolname "aura_$role" && ! pg_has pg_roles rolname "selfvoy_$role"; then
            echo "Переименовываю роль aura_$role → selfvoy_$role"
            pg_query "ALTER ROLE aura_$role RENAME TO selfvoy_$role" >/dev/null
        fi
    done
}

llm_check() {
    # Проверка изнутри контейнера AI: там есть SELFVOY_AI_TOKEN и сеть до провайдера.
    "${compose[@]}" exec -T ai python - http://127.0.0.1:8000 < tools/llm_check.py
}

cmd=${1:-status}
case "$cmd" in
    status)
        "${compose[@]}" ps
        echo
        "${compose[@]}" exec -T ai python -c \
            "import json,urllib.request;print('LLM:', json.load(urllib.request.urlopen('http://127.0.0.1:8000/healthz'))['llm'])" \
            2>/dev/null || echo "AI-сервис не отвечает"
        ;;
    logs)
        "${compose[@]}" logs --tail 100 "${@:2}"
        ;;
    verify)
        email=${2:-}
        [ -n "$email" ] || { echo "использование: selfvoy verify ваш@email"; exit 1; }
        # Значение — переменной psql (:'email' экранирует как литерал), не подстановкой в SQL.
        # psql подставляет переменные только в командах со stdin, не в -c.
        if echo "UPDATE users SET email_verified = true WHERE lower(email) = lower(:'email');" \
            | "${compose[@]}" exec -T postgres psql -v ON_ERROR_STOP=1 -U postgres -d selfvoy -v email="$email" \
            | grep -q "UPDATE 1"; then
            echo "Email $email подтверждён — входите в приложении."
        else
            echo "Пользователь $email не найден — сначала зарегистрируйтесь в приложении."
            exit 1
        fi
        ;;
    pro)
        email=${2:-}
        days=${3:-}
        [ -n "$email" ] && [ -n "$days" ] || { echo "использование: selfvoy pro ваш@email ДНЕЙ (0 — снять Pro)"; exit 1; }
        case "$days" in
            ''|*[!0-9]*) echo "ДНЕЙ — целое число от 0 до 3650"; exit 1 ;;
        esac
        [ "${#days}" -le 4 ] && [ "$days" -le 3650 ] || { echo "ДНЕЙ — целое число от 0 до 3650"; exit 1; }
        # Значения — переменными psql через stdin (:'email', :'days' — литералы, не подстановка в SQL).
        if echo "UPDATE users SET plan_until = CASE WHEN :'days'::int = 0 THEN NULL
                  ELSE now() + make_interval(days => :'days'::int) END
                  WHERE lower(email) = lower(:'email');" \
            | "${compose[@]}" exec -T postgres psql -v ON_ERROR_STOP=1 -U postgres -d selfvoy \
                -v email="$email" -v days="$days" \
            | grep -q "UPDATE 1"; then
            if [ "$days" -eq 0 ]; then
                echo "Тариф Pro у $email снят."
            else
                echo "Тариф Pro для $email — на $days дн."
            fi
        else
            echo "Пользователь $email не найден — сначала зарегистрируйтесь в приложении."
            exit 1
        fi
        ;;
    llm-key)
        printf 'API-ключ LLM: '
        read -r -s key </dev/tty; echo
        [ -n "$key" ] || { echo "Ключ пустой — ничего не изменено."; exit 1; }
        printf 'Адрес API (Enter — OpenAI%s): ' "$( [ -n "$(env_get OPENAI_BASE_URL)" ] && echo ", сейчас $(env_get OPENAI_BASE_URL)")"
        read -r base </dev/tty
        printf 'Модель (Enter — %s): ' "$(env_get SELFVOY_LLM_MODEL)"
        read -r model </dev/tty
        env_set OPENAI_API_KEY "$key"
        env_set SELFVOY_LLM_PROVIDER openai
        [ -n "$base" ] && env_set OPENAI_BASE_URL "$base"
        [ -n "$model" ] && env_set SELFVOY_LLM_MODEL "$model"
        "${compose[@]}" up -d ai
        sleep 5
        llm_check
        ;;
    llm-check)
        llm_check
        ;;
    backup)
        mkdir -p backups && chmod 700 backups
        file="backups/selfvoy-$(date -u +%Y%m%d-%H%M%S).sql.gz"
        "${compose[@]}" exec -T postgres pg_dump -U postgres selfvoy | gzip > "$file"
        chmod 600 "$file"
        ls -1t backups/selfvoy-*.sql.gz | tail -n +15 | xargs -r rm -f
        echo "Резервная копия: $SELFVOY_DIR/$file"
        ;;
    update)
        git pull --ff-only
        migrate_db
        "${compose[@]}" up -d --build
        ;;
    migrate)
        migrate_db
        ;;
    restart)
        "${compose[@]}" restart "${@:2}"
        ;;
    *)
        sed -n '2,13p' "$0"
        exit 1
        ;;
esac
