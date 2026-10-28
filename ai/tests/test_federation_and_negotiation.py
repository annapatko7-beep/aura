"""Тесты для модулей Федерации (federation.py), Переговоров (negotiator.py) и Календаря (caldav_sync.py)."""

from datetime import datetime, timezone
from selfvoy_ai import caldav_sync, federation, negotiator
from selfvoy_ai.models import AgentContext, CalendarEvent, Preferences, Slot


def test_federation_sign_and_verify():
    priv, pub = federation._generate_ed25519_keypair()
    payload = {"topic": "стартап", "duration": 60}

    envelope = federation.sign_envelope(
        sender="@alex:aura.me",
        recipient="@elena:aura.org",
        intent="schedule_meeting",
        payload=payload,
        private_key_hex=priv,
    )
    assert envelope["sender"] == "@alex:aura.me"
    assert envelope["signature"]

    valid, reason = federation.verify_envelope(envelope, pub)
    assert valid is True

    # Проверка защиты от подделки (tampering)
    tampered = dict(envelope)
    tampered["payload"] = {"topic": "поддельная тема"}
    valid_tampered, _ = federation.verify_envelope(tampered, pub)
    assert valid_tampered is False


def test_federation_replay_attack_prevention():
    priv, pub = federation._generate_ed25519_keypair()
    envelope = federation.sign_envelope(
        sender="@alice:aura.me",
        recipient="@bob:aura.me",
        intent="ping",
        payload={"msg": "hello"},
        private_key_hex=priv,
    )

    valid1, _ = federation.verify_envelope(envelope, pub)
    assert valid1 is True

    # Повторная посылка того же envelope с тем же nonce
    valid2, reason2 = federation.verify_envelope(envelope, pub)
    assert valid2 is False
    assert "replay" in reason2


def test_negotiation_direct_agreement():
    now = datetime(2026, 10, 24, 10, 0, tzinfo=timezone.utc)
    init_ctx = AgentContext(
        message="хочу встретиться",
        now=now,
        preferences=Preferences(city="Керкраде"),
        calendar=[
            CalendarEvent(title="Встреча 1", start=datetime(2026, 10, 24, 11, 0, tzinfo=timezone.utc), end=datetime(2026, 10, 24, 12, 0, tzinfo=timezone.utc))
        ],
    )
    resp_ctx = AgentContext(
        message="",
        now=now,
        preferences=Preferences(city="Керкраде"),
        calendar=[
            CalendarEvent(title="Встреча 2", start=datetime(2026, 10, 24, 14, 0, tzinfo=timezone.utc), end=datetime(2026, 10, 24, 15, 0, tzinfo=timezone.utc))
        ],
    )

    session = negotiator.NegotiationSession(
        id="neg-1",
        topic="обсудить стартап",
        initiator_email="alex@aura.me",
        responder_email="elena@aura.me",
        duration_minutes=60,
    )

    result = negotiator.run_negotiation_round(session, init_ctx, resp_ctx)
    assert result.status == "AGREED"
    assert result.selected_slot is not None
    assert len(result.history) == 1


def test_caldav_ics_parsing():
    sample_ics = """BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//Aura Calendar//EN
BEGIN:VEVENT
SUMMARY:Обсуждение архитектуры
DTSTART:20261024T140000Z
DTEND:20261024T150000Z
END:VEVENT
BEGIN:VEVENT
SUMMARY:Обед с инвестором
DTSTART:20261025T120000Z
DTEND:20261025T130000Z
END:VEVENT
END:VCALENDAR"""

    events = caldav_sync.parse_vcalendar(sample_ics)
    assert len(events) == 2
    assert events[0].title == "Обсуждение архитектуры"
    assert events[0].start.year == 2026
    assert events[0].start.hour == 14
    assert events[1].title == "Обед с инвестором"
    assert events[1].start.day == 25
