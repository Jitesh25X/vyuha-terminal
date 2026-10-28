"""Exchange holiday calendar seed from `exchange_calendars` (XBOM; NSE and BSE share the
equity/derivatives holiday list). Existing rows win, so desk overrides in `exchange_holiday`
(special sessions, late circulars) are never overwritten.
"""

from __future__ import annotations

from datetime import date, timedelta

import exchange_calendars as xc

from ..db import alert_admins, connect

EXCHANGES = ("NSE", "BSE", "NFO", "BFO")
# Warn admins when the library's calendar ends sooner than this (next year's list not in it yet).
COVERAGE_WARN_DAYS = 90


def holidays(start: date, end: date) -> tuple[list[date], date]:
    """Weekday non-sessions of XBOM in [start, end], and the last day the calendar covers."""
    cal = xc.get_calendar("XBOM")
    last = cal.last_session.date()
    end = min(end, last)
    sessions = {d.date() for d in cal.sessions_in_range(start.isoformat(), end.isoformat())}
    out: list[date] = []
    d = start
    while d <= end:
        if d.weekday() < 5 and d not in sessions:
            out.append(d)
        d += timedelta(days=1)
    return out, last


async def seed(dsn: str, today: date) -> dict[str, object]:
    days, last = holidays(date(today.year - 1, 1, 1), date(today.year + 1, 12, 31))
    conn = await connect(dsn)
    try:
        async with conn.transaction():
            status: str = await conn.execute(
                """INSERT INTO exchange_holiday (exchange, day, description)
                   SELECT e::exchange_code, d, 'exchange_calendars XBOM'
                   FROM unnest($1::text[]) e CROSS JOIN unnest($2::date[]) d
                   ON CONFLICT (exchange, day) DO NOTHING""",
                list(EXCHANGES),
                days,
            )
            if (last - today).days < COVERAGE_WARN_DAYS:
                await alert_admins(
                    conn,
                    "warn",
                    "Holiday calendar coverage ending",
                    {
                        "covered_until": last.isoformat(),
                        "action": "add next year's NSE holidays to exchange_holiday or upgrade exchange_calendars",
                    },
                )
    finally:
        await conn.close()
    return {"holidays": len(days), "inserted": int(status.split()[-1]), "covered_until": last.isoformat()}
