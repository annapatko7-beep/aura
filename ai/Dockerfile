# Python AI Service
FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY aura_ai ./aura_ai

EXPOSE 8000
CMD ["uvicorn", "aura_ai.app:app", "--host", "0.0.0.0", "--port", "8000"]
