"""caldav_sync.py — синхронизация внешних календарей по протоколу CalDAV / iCalendar (RFC 5545).

Позволяет Ауре напрямую подключаться к Apple iCloud, Яндекс Календарю,
Nextcloud, Google CalDAV или Fastmail без сторонних прокси, извлекая занятые интервалы.
"""

from __future__ import annotations

import logging
import re
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from .models import CalendarEvent

logger = logging.getLogger("selfvoy.caldav")

_DT_RE = re.compile(r"^(?:TZID=[^:]+:)?(\d{4})(\d{2})(\d{2})T?(\d{2})?(\d{2})?(\d{2})?(Z)?$")


def parse_ical_datetime(val: str) -> Optional[datetime]:
    """Разбирает формат даты-времени iCalendar (например, 20261024T143000Z или 20261024)."""
    raw = val.strip()
    match = _DT_RE.match(raw)
    if not match:
        try:
            return datetime.fromisoformat(raw.replace("Z", "+00:00"))
        except Exception:
            return None

    year, month, day, hour, minute, second, is_z = match.groups()
    try:
        dt = datetime(
            int(year),
            int(month),
            int(day),
            int(hour or 0),
            int(minute or 0),
            int(second or 0),
            tzinfo=timezone.utc if is_z else timezone.utc,
        )
        return dt
    except ValueError:
        return None


def parse_vcalendar(vcal_text: str) -> List[CalendarEvent]:
    """Извлекает список событий (VEVENT) из сырого текста iCalendar (.ics)."""
    events: List[CalendarEvent] = []
    lines = vcal_text.replace("\r\n", "\n").replace("\r", "\n").split("\n")

    # Склеиваем сложенные строки RFC 5545 (начинающиеся с пробела или табуляции)
    unfolded: List[str] = []
    for line in lines:
        if line.startswith((" ", "\t")) and unfolded:
            unfolded[-1] += line[1:]
        else:
            unfolded.append(line)

    in_vevent = False
    current_event: Dict[str, str] = {}

    for line in unfolded:
        line_clean = line.strip()
        if line_clean == "BEGIN:VEVENT":
            in_vevent = True
            current_event = {}
            continue
        elif line_clean == "END:VEVENT":
            in_vevent = False
            summary = current_event.get("SUMMARY", "(Занято)")
            dtstart_raw = current_event.get("DTSTART", "")
            dtend_raw = current_event.get("DTEND", "")

            dtstart = parse_ical_datetime(dtstart_raw)
            dtend = parse_ical_datetime(dtend_raw)

            if dtstart:
                if not dtend or dtend <= dtstart:
                    from datetime import timedelta
                    dtend = dtstart + timedelta(minutes=60)
                events.append(CalendarEvent(title=summary, start=dtstart, end=dtend))
            continue

        if in_vevent and ":" in line:
            prop_part, val_part = line.split(":", 1)
            prop_name = prop_part.split(";")[0].upper().strip()
            current_event[prop_name] = val_part.strip()

    return events


async def fetch_caldav_events(
    caldav_url: str,
    username: str = "",
    password: str = "",
    time_min: Optional[datetime] = None,
    time_max: Optional[datetime] = None,
) -> List[CalendarEvent]:
    """Делает CalDAV REPORT-запрос или скачивает .ics-поток календаря."""
    import httpx

    auth = (username, password) if username and password else None
    events: List[CalendarEvent] = []

    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            # Сначала пробуем обычный GET (для .ics ссылок и публичных календарей)
            resp = await client.get(caldav_url, auth=auth)
            if resp.status_code == 200 and "BEGIN:VCALENDAR" in resp.text:
                return parse_vcalendar(resp.text)

            # Если сервер требует CalDAV REPORT
            report_body = """<?xml version="1.0" encoding="utf-8" ?>
            <C:calendar-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">
              <D:prop>
                <D:getetag/>
                <C:calendar-data/>
              </D:prop>
              <C:filter>
                <C:comp-filter name="VCALENDAR">
                  <C:comp-filter name="VEVENT" />
                </C:comp-filter>
              </C:filter>
            </C:calendar-query>"""

            headers = {"Depth": "1", "Content-Type": "application/xml; charset=utf-8"}
            resp_report = await client.request("REPORT", caldav_url, content=report_body, headers=headers, auth=auth)
            if resp_report.status_code in (200, 207):
                return parse_vcalendar(resp_report.text)

    except Exception as exc:
        logger.warning("CalDAV sync failed for %s: %s", caldav_url, exc)

    return events
