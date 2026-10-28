#!/usr/bin/env bash
# ==============================================================================
# Selfvoy PostgreSQL Automated Backup Script
# ==============================================================================
# Резервное копирование базы данных PostgreSQL с gzip сжатием и ротацией.
# Настройка в cron (например, каждый день в 03:00):
#   0 3 * * * /opt/selfvoy/deploy/backup.sh >> /var/log/selfvoy-backup.log 2>&1
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BACKUP_DIR="${ROOT_DIR}/backups"
TIMESTAMP="$(date +'%Y%m%d_%H%M%S')"
BACKUP_FILE="${BACKUP_DIR}/selfvoy_db_${TIMESTAMP}.sql.gz"
RETENTION_DAYS=14

mkdir -p "$BACKUP_DIR"

echo "=== [$(date --iso-8601=seconds)] Начало резервного копирования Selfvoy DB ==="

# Запуск pg_dump внутри контейнера postgres
if docker compose -f "${ROOT_DIR}/docker-compose.yml" ps --services --filter "status=running" | grep -q "postgres"; then
    docker compose -f "${ROOT_DIR}/docker-compose.yml" exec -T postgres \
        pg_dump -U postgres -d selfvoy --clean --if-exists | gzip -9 > "$BACKUP_FILE"
    
    SIZE="$(du -h "$BACKUP_FILE" | cut -f1)"
    echo "✓ Бэкап успешно сохранён: $BACKUP_FILE (размер: $SIZE)"
else
    echo "ОШИБКА: Контейнер postgres не запущен!" >&2
    exit 1
fi

# Удаление бэкапов старше RETENTION_DAYS дней
echo "Очистка старых резервных копий (старше ${RETENTION_DAYS} дней)..."
find "$BACKUP_DIR" -type f -name "selfvoy_db_*.sql.gz" -mtime +"$RETENTION_DAYS" -exec rm -v {} \;

echo "=== [$(date --iso-8601=seconds)] Резервное копирование завершено успешно ==="
