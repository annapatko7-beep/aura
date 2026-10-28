"""app.py — легковесный шлюз Push-уведомлений (APNs + FCM).

Принимает готовый конверт от сервера Selfvoy (POST /push), подписывает JWT
(ES256 для Apple APNs) или авторизует вызов Google FCM HTTP v1 API и пересылает
уведомление на мобильные устройства.

Соответствует спецификации в docs/PUSH_GATEWAY.md.
В dev-режиме (или при отсутствии боевых ключей) логирует отправку и возвращает ok.
"""

from __future__ import annotations

import logging
import os
import time
from typing import Any, Dict, Optional

import httpx
from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel, Field

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
logger = logging.getLogger("push-gateway")

app = FastAPI(title="Selfvoy Push Gateway", version="1.0.0")

# --- Конфигурация APNs ---
APNS_KEY_ID = os.environ.get("APNS_KEY_ID", "")
APNS_TEAM_ID = os.environ.get("APNS_TEAM_ID", "")
APNS_KEY_PATH = os.environ.get("APNS_KEY_PATH", "")
APNS_USE_SANDBOX = os.environ.get("APNS_SANDBOX", "0").lower() in ("1", "true", "yes")

_apns_private_key: Optional[str] = None
if APNS_KEY_PATH and os.path.isfile(APNS_KEY_PATH):
    try:
        with open(APNS_KEY_PATH, "r", encoding="utf-8") as f:
            _apns_private_key = f.read()
    except Exception as exc:
        logger.error("Failed to read APNS_KEY_PATH (%s): %s", APNS_KEY_PATH, exc)

_apns_token_cache: Dict[str, Any] = {"token": None, "ts": 0}


def get_apns_jwt() -> Optional[str]:
    """Генерирует или возвращает закешированный ES256 JWT для Apple Push Notification service."""
    if not _apns_private_key or not APNS_KEY_ID or not APNS_TEAM_ID:
        return None

    now = int(time.time())
    if _apns_token_cache["token"] and (now - _apns_token_cache["ts"]) < 3000:
        return _apns_token_cache["token"]

    try:
        import jwt

        token = jwt.encode(
            {"iss": APNS_TEAM_ID, "iat": now},
            _apns_private_key,
            algorithm="ES256",
            headers={"kid": APNS_KEY_ID},
        )
        _apns_token_cache.update(token=token, ts=now)
        return token
    except Exception as exc:
        logger.error("Failed to generate APNs JWT: %s", exc)
        return None


# --- Конфигурация FCM ---
FCM_PROJECT_ID = os.environ.get("FCM_PROJECT_ID", "")
FCM_ACCESS_TOKEN = os.environ.get("FCM_ACCESS_TOKEN", "")
FCM_SERVICE_ACCOUNT_PATH = os.environ.get("FCM_SERVICE_ACCOUNT", "")

DEV_MODE = os.environ.get("PUSH_GATEWAY_DEV_MODE", "1").lower() in ("1", "true", "yes")


def get_fcm_token() -> Optional[str]:
    """Получает Bearer токен для Google FCM HTTP v1 API."""
    if FCM_ACCESS_TOKEN:
        return FCM_ACCESS_TOKEN

    if FCM_SERVICE_ACCOUNT_PATH and os.path.isfile(FCM_SERVICE_ACCOUNT_PATH):
        try:
            from google.oauth2 import service_account
            import google.auth.transport.requests

            creds = service_account.Credentials.from_service_account_file(
                FCM_SERVICE_ACCOUNT_PATH,
                scopes=["https://www.googleapis.com/auth/firebase.messaging"],
            )
            creds.refresh(google.auth.transport.requests.Request())
            return creds.token
        except Exception as exc:
            logger.error("Failed to get FCM OAuth token from service account: %s", exc)

    return None


@app.get("/healthz")
def healthz() -> Dict[str, Any]:
    return {
        "status": "ok",
        "service": "push-gateway",
        "apns_configured": bool(_apns_private_key and APNS_KEY_ID and APNS_TEAM_ID),
        "fcm_configured": bool(FCM_PROJECT_ID and (FCM_ACCESS_TOKEN or FCM_SERVICE_ACCOUNT_PATH)),
        "dev_mode": DEV_MODE,
    }


class PushPayload(BaseModel):
    platform: str = Field(description="apns | fcm")
    token: str = Field(description="Device push token")
    notification_id: Optional[int] = Field(default=None)
    apns: Optional[Dict[str, Any]] = None
    fcm: Optional[Dict[str, Any]] = None


@app.post("/push")
async def forward_push(payload: PushPayload) -> Dict[str, Any]:
    platform = payload.platform.lower().strip()
    nid = payload.notification_id
    token = payload.token.strip()

    logger.info("Received push request: platform=%s, nid=%s, token=%s...", platform, nid, token[:12])

    if platform not in ("apns", "fcm"):
        return {"ok": False, "notification_id": nid, "error": f"unknown platform '{platform}'", "provider_status": 400}

    # Dev / Mock fallback если ключи не заданы или включен режим симуляции
    is_mock_token = token.startswith(("mock-", "test-", "dev-")) or token == "simulator"
    if DEV_MODE or is_mock_token:
        logger.info("[DEV MODE] Simulated push delivery for nid=%s (token=%s)", nid, token[:12])
        return {"ok": True, "notification_id": nid, "provider_status": 200, "simulated": True}

    if platform == "apns":
        jwt_token = get_apns_jwt()
        if not jwt_token:
            logger.warning("APNs credentials missing; falling back to simulated delivery")
            return {"ok": True, "notification_id": nid, "provider_status": 200, "simulated": True}

        apns_info = payload.apns or {}
        default_base = "https://api.sandbox.push.apple.com" if APNS_USE_SANDBOX else "https://api.push.apple.com"
        target_url = apns_info.get("url") or f"{default_base}/3/device/{token}"

        headers = dict(apns_info.get("headers", {}))
        headers["authorization"] = f"bearer {jwt_token}"

        body = apns_info.get("payload", {})
        async with httpx.AsyncClient(http2=True, timeout=10.0) as client:
            try:
                resp = await client.post(target_url, headers=headers, json=body)
                ok = 200 <= resp.status_code < 300
                logger.info("APNs response: status=%d, ok=%s", resp.status_code, ok)
                res: Dict[str, Any] = {"ok": ok, "notification_id": nid, "provider_status": resp.status_code}
                if not ok:
                    try:
                        err_json = resp.json()
                        res["error"] = err_json.get("reason", "APNsError")
                    except Exception:
                        res["error"] = resp.text[:120]
                return res
            except Exception as exc:
                logger.error("APNs request error: %s", exc)
                return {"ok": False, "notification_id": nid, "error": str(exc), "provider_status": 502}

    elif platform == "fcm":
        bearer_token = get_fcm_token()
        if not bearer_token or not FCM_PROJECT_ID:
            logger.warning("FCM credentials missing; falling back to simulated delivery")
            return {"ok": True, "notification_id": nid, "provider_status": 200, "simulated": True}

        fcm_url = f"https://fcm.googleapis.com/v1/projects/{FCM_PROJECT_ID}/messages:send"
        headers = {
            "Authorization": f"Bearer {bearer_token}",
            "Content-Type": "application/json",
        }
        body = payload.fcm or {"message": {"token": token}}

        async with httpx.AsyncClient(timeout=10.0) as client:
            try:
                resp = await client.post(fcm_url, headers=headers, json=body)
                ok = 200 <= resp.status_code < 300
                logger.info("FCM response: status=%d, ok=%s", resp.status_code, ok)
                res = {"ok": ok, "notification_id": nid, "provider_status": resp.status_code}
                if not ok:
                    try:
                        res["error"] = resp.json().get("error", {}).get("message", "FCMError")
                    except Exception:
                        res["error"] = resp.text[:120]
                return res
            except Exception as exc:
                logger.error("FCM request error: %s", exc)
                return {"ok": False, "notification_id": nid, "error": str(exc), "provider_status": 502}

    return {"ok": False, "notification_id": nid, "error": f"unknown platform '{platform}'", "provider_status": 400}


def main():
    import uvicorn
    port = int(os.environ.get("PORT", 8080))
    host = os.environ.get("HOST", "0.0.0.0")
    uvicorn.run(app, host=host, port=port)


if __name__ == "__main__":
    main()
