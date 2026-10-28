# Python AI Service
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
# В контейнере память работает через PostgreSQL (SELFVOY_MEMORY_BACKEND в
# docker-compose) — драйвер ставим в образ, а не в dev-requirements.
RUN pip install --no-cache-dir "psycopg[binary]>=3.1"

COPY selfvoy_ai ./selfvoy_ai

# Непривилегированный пользователь: сервис не пишет в /app (память — в
# PostgreSQL), код принадлежит root и только читается.
RUN useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin selfvoy
USER 10001:10001

EXPOSE 8000
# 0.0.0.0 — внутри сети compose (порт наружу не публикуется). Без
# SELFVOY_AI_TOKEN сервис отвечает 503 на всё, кроме /healthz.
CMD ["uvicorn", "selfvoy_ai.app:app", "--host", "0.0.0.0", "--port", "8000"]
