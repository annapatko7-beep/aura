#!/bin/bash
# PostgreSQL backup script для Selfvoy
#
# Использование:
#   ./tools/backup.sh                    # бэкап в ./backups/
#   BACKUP_DIR=/mnt/s3 ./tools/backup.sh # кастомная директория
#
# Для автоматических бэкапов добавьте в crontab:
#   0 2 * * * /path/to/selfvoy-main/tools/backup.sh >> /var/log/selfvoy-backup.log 2>&1

set -e

BACKUP_DIR=${BACKUP_DIR:-./backups}
DATABASE_URL=${SELFVOY_DATABASE_URL:-${PG_URI:-postgresql://postgres:postgres@localhost/selfvoy}}

mkdir -p "$BACKUP_DIR"

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
FILENAME="selfvoy-backup-${TIMESTAMP}.sql.gz"
FILEPATH="$BACKUP_DIR/$FILENAME"

echo "[$(date)] Начало бэкапа: $DATABASE_URL -> $FILEPATH"

if ! command -v pg_dump &> /dev/null; then
    echo "❌ ОШИБКА: pg_dump не найден. Установите PostgreSQL client."
    exit 1
fi

# Создаём бэкап (с сжатием gzip)
pg_dump "$DATABASE_URL" | gzip > "$FILEPATH"

SIZE=$(du -h "$FILEPATH" | cut -f1)
echo "✅ Бэкап сохранён: $FILEPATH (размер: $SIZE)"

# Опционально: загрузить в S3/MinIO
# if [ -n "$AWS_S3_BUCKET" ]; then
#     aws s3 cp "$FILEPATH" "s3://${AWS_S3_BUCKET}/selfvoy-backups/"
#     echo "☁️  Загружено в S3: s3://${AWS_S3_BUCKET}/selfvoy-backups/$FILENAME"
# fi

# Опционально: удалить старые бэкапы (старше 7 дней)
# find "$BACKUP_DIR" -name 'selfvoy-backup-*.sql.gz' -mtime +7 -delete

echo "[$(date)] Бэкап завершён успешно"
