"""Exchange holiday calendar seed from `exchange_calendars` (XBOM; NSE and BSE share the
equity/derivatives holiday list). Existing rows win, so desk overrides in `exchange_holiday`
(special sessions, late circulars) are never overwritten. Muhurat windows fill a row's empty
`special_session` and leave a desk's own value alone.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date, timedelta

import exchange_calendars as xc

from ..db import alert_admins, connect

EXCHANGES = ("NSE", "BSE", "NFO", "BFO")
# The first job downloads daily bars from 2015, and a trading day with no bars counts as a gap there.
CALENDAR_FROM = date(2015, 1, 1)
_CIRCULARS = "https://archives.nseindia.com/content/circulars/"


@dataclass(frozen=True, slots=True)
class SpecialSession:
    day: date
    open: str  # IST
    close: str
    circular: str


# Normal market windows of the capital market segment, read from each NSE circular. In 2026, closing-auction
# securities stop continuous trading at 15:15; the rest run to 15:30.
MUHURAT = (
    SpecialSession(date(2022, 10, 24), "18:15", "19:15", "CMTR54023.pdf"),
    SpecialSession(date(2023, 11, 12), "18:15", "19:15", "CMTR59124.pdf"),
    SpecialSession(date(2024, 11, 1), "18:00", "19:00", "CMTR64628.pdf"),
    SpecialSession(date(2025, 10, 21), "13:45", "14:45", "CMTR70319.pdf"),
    SpecialSession(date(2026, 11, 8), "14:20", "15:30", "CMTR76941.pdf"),
)
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
    days, last = holidays(CALENDAR_FROM, date(today.year + 1, 12, 31))
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
            for s in MUHURAT:
                await conn.execute(
                    """INSERT INTO exchange_holiday (exchange, day, description, special_session)
                       VALUES ('NSE', $1, $2, $3)
                       ON CONFLICT (exchange, day) DO UPDATE
                       SET special_session = EXCLUDED.special_session, description = EXCLUDED.description
                       WHERE exchange_holiday.special_session IS NULL""",
                    s.day,
                    f"Muhurat trading, {_CIRCULARS}{s.circular}",
                    [{"open": s.open, "close": s.close}],
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
    return {
        "holidays": len(days),
        "muhurat_sessions": len(MUHURAT),
        "inserted": int(status.split()[-1]),
        "covered_until": last.isoformat(),
    }
