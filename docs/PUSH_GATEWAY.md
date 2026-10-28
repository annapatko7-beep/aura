# Push Gateway для Selfvoy

C++-сервер не умеет TLS/HTTP2/ES256-подпись (нужную для APNs) — поэтому push-уведомления форвардятся через внешний шлюз. Шлюз принимает готовый payload от сервера Selfvoy, подписывает JWT (для APNs) или добавляет OAuth-токен (для FCM) и отправляет в Apple/Google.

## Архитектура

```
C++ сервер  --(HTTP POST /push)-->  Push Gateway  --(HTTP2 + JWT)-->  APNs
                                                   --(HTTP + OAuth)-->  FCM
```

Сервер включает webhook-драйвер через env:

```bash
SELFVOY_PUSH_DRIVER=webhook
SELFVOY_PUSH_WEBHOOK_URL=https://push-gateway.internal/push
```

## Контракт

### Запрос от сервера Selfvoy

```http
POST /push HTTP/1.1
Content-Type: application/json

{
  "platform": "apns",
  "token": "device-token-here",
  "notification_id": 123,
  "apns": {
    "url": "https://api.push.apple.com/3/device/<token>",
    "headers": {
      "apns-topic": "app.selfvoy",
      "apns-push-type": "alert",
      "apns-priority": "10"
    },
    "payload": {
      "aps": {
        "alert": { "title": "Задача", "body": "Через 15 минут: созвон" },
        "sound": "default",
        "badge": 1
      },
      "selfvoy": { "deep_link": "selfvoy://tasks" }
    }
  }
}
```

Для FCM:

```json
{
  "platform": "fcm",
  "token": "fcm-registration-token",
  "notification_id": 124,
  "fcm": {
    "message": {
      "token": "fcm-registration-token",
      "notification": { "title": "Задача", "body": "Через 15 минут: созвон" },
      "data": { "deep_link": "selfvoy://tasks" }
    }
  }
}
```

### Ответ шлюза

```json
{ "ok": true, "notification_id": 123, "provider_status": 200 }
```

При ошибке:

```json
{ "ok": false, "notification_id": 123, "error": "BadDeviceToken", "provider_status": 400 }
```

Сервер использует `ok` для отметки доставки, `error` = `BadDeviceToken`/`Unregistered` — сигнал удалить токен устройства из БД.

## Минимальный шлюз на Python

```python
"""push_gateway.py — минимальный push-шлюз (APNs + FCM).

Зависимости: pip install fastapi uvicorn httpx pyjwt cryptography
Запуск:      uvicorn push_gateway:app --port 8080
"""
import time
import os
import jwt  # PyJWT
import httpx
from fastapi import FastAPI, Request

app = FastAPI()

# --- APNs (ES256 JWT) ---
APNS_KEY_ID = os.environ["APNS_KEY_ID"]        # 10-символьный Key ID
APNS_TEAM_ID = os.environ["APNS_TEAM_ID"]      # 10-символьный Team ID
APNS_KEY = open(os.environ["APNS_KEY_PATH"]).read()  # AuthKey_XXX.p8

_apns_token_cache = {"token": None, "ts": 0}

def apns_jwt() -> str:
    # APNs токен живёт до 60 минут; кэшируем на 50.
    now = int(time.time())
    if _apns_token_cache["token"] and now - _apns_token_cache["ts"] < 3000:
        return _apns_token_cache["token"]
    token = jwt.encode(
        {"iss": APNS_TEAM_ID, "iat": now},
        APNS_KEY,
        algorithm="ES256",
        headers={"kid": APNS_KEY_ID},
    )
    _apns_token_cache.update(token=token, ts=now)
    return token

# --- FCM (OAuth 2.0 service account) ---
# from google.oauth2 import service_account
# import google.auth.transport.requests
# creds = service_account.Credentials.from_service_account_file(
#     os.environ["FCM_SERVICE_ACCOUNT"],
#     scopes=["https://www.googleapis.com/auth/firebase.messaging"],
# )
# def fcm_token() -> str:
#     creds.refresh(google.auth.transport.requests.Request())
#     return creds.token

@app.post("/push")
async def push(request: Request):
    body = await request.json()
    platform = body["platform"]
    nid = body.get("notification_id")

    async with httpx.AsyncClient(http2=True, timeout=10) as client:
        if platform == "apns":
            apns = body["apns"]
            headers = dict(apns.get("headers", {}))
            headers["authorization"] = f"bearer {apns_jwt()}"
            resp = await client.post(apns["url"], headers=headers, json=apns["payload"])
        elif platform == "fcm":
            project = os.environ["FCM_PROJECT_ID"]
            url = f"https://fcm.googleapis.com/v1/projects/{project}/messages:send"
            headers = {"authorization": f"Bearer {fcm_token()}"}
            resp = await client.post(url, headers=headers, json=body["fcm"])
        else:
            return {"ok": False, "notification_id": nid, "error": "unknown_platform"}

    ok = 200 <= resp.status_code < 300
    result = {"ok": ok, "notification_id": nid, "provider_status": resp.status_code}
    if not ok:
        # APNs кладёт reason в JSON, FCM — в error.status
        try:
            result["error"] = resp.json().get("reason") or resp.json().get("error", {}).get("status")
        except Exception:
            result["error"] = resp.text[:200]
    return result
```

## Deployment

- Разверните шлюз во внутренней сети (не публичный) — сервер Selfvoy обращается к нему по `SELFVOY_PUSH_WEBHOOK_URL`.
- Секреты (APNs `.p8`, FCM service account JSON) монтируйте через volume/secrets, **не в образ**.
- Рекомендуется mTLS или сетевая изоляция между сервером и шлюзом.
- Для масштабирования: шлюз stateless (кроме кэша JWT), можно горизонтально масштабировать за load balancer.

## Тестирование (mock)

Для E2E-тестов используется `SELFVOY_E2E_PUSH_MOCK=1` — сервер не обращается к реальному шлюзу, а логирует payload. См. `tools/e2e.py`.
