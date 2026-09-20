# Python AI Service
FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
# В контейнере память работает через PostgreSQL (AURA_MEMORY_BACKEND в
# docker-compose) — драйвер ставим в образ, а не в dev-requirements.
RUN pip install --no-cache-dir "psycopg[binary]>=3.1"

COPY aura_ai ./aura_ai

EXPOSE 8000
CMD ["uvicorn", "aura_ai.app:app", "--host", "0.0.0.0", "--port", "8000"]
