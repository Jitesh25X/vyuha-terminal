"""NSE F&O ban list, imported manually (ADR-019: no scripted downloads from the NSE website).

The operator downloads NSE's daily "Securities in Ban Period" file (fo_secban.csv) in a browser and
runs `vyuha-quant ban import FILE`. Expected shape:

    Securities in Ban For Trade Date 28-OCT-2026:
    1,ABC
    2,XYZ

An empty list ("NIL" or no rows) is valid. The trade date comes from the header and must match
the date being imported, so yesterday's file can't be applied to today by mistake.
"""

from __future__ import annotations

import hashlib
import re
from datetime import date, datetime

from ..db import Conn, alert_admins, audit, connect
from .instruments.store import apply_fo_ban

_HEADER = re.compile(r"trade\s*date\s*[:\-]?\s*(\d{1,2}-[A-Za-z]{3}-\d{4})", re.IGNORECASE)
_SYMBOL = re.compile(r"^[A-Z0-9&\-]{1,20}$")


class BanListError(ValueError):
    pass


def parse(content: bytes) -> tuple[date, list[str]]:
    text = content.decode("utf-8-sig", errors="strict").replace("\r", "")
    lines = [ln.strip() for ln in text.split("\n") if ln.strip()]
    if not lines:
        raise BanListError("empty file")
    m = _HEADER.search(lines[0])
    if not m:
        raise BanListError(
            "first line must name the trade date, e.g. 'Securities in Ban For Trade Date 28-OCT-2026:'"
        )
    trade_date = datetime.strptime(m.group(1).upper(), "%d-%b-%Y").date()  # noqa: DTZ007 (a calendar date)
    symbols: list[str] = []
    for ln in lines[1:]:
        parts = [p.strip().strip('"') for p in ln.split(",")]
        if len(parts) == 1 and parts[0].upper() == "NIL":
            continue
        sym = parts[-1].upper()
        if len(parts) != 2 or not parts[0].isdigit() or not _SYMBOL.match(sym):
            raise BanListError(f"unexpected line {ln!r}: expected 'n,SYMBOL'")
        symbols.append(sym)
    if lines[0].upper().rstrip(" :").endswith("NIL"):
        symbols = []
    return trade_date, sorted(set(symbols))


async def import_file(dsn: str, content: bytes, expected: date) -> dict[str, object]:
    trade_date, symbols = parse(content)
    if trade_date != expected:
        raise BanListError(
            f"file is for trade date {trade_date}, expected {expected} (use --date to override)"
        )
    digest = hashlib.sha256(content).hexdigest()
    conn = await connect(dsn)
    try:
        async with conn.transaction():
            await conn.execute(
                """INSERT INTO fo_ban_import (trade_date, symbols, source_sha256) VALUES ($1, $2, $3)
                   ON CONFLICT (trade_date) DO UPDATE
                   SET symbols = EXCLUDED.symbols, source_sha256 = EXCLUDED.source_sha256, imported_at = now()""",
                trade_date,
                symbols,
                digest,
            )
            _, banned = await apply_fo_ban(conn, trade_date)
            report: dict[str, object] = {
                "trade_date": trade_date.isoformat(),
                "symbols": symbols,
                "contracts_banned": banned,
                "sha256": digest,
            }
            await audit(conn, "fo_ban.import", "fo_ban_import", trade_date.isoformat(), report)
    finally:
        await conn.close()
    return report


async def check(dsn: str, day: date) -> date | None:
    """Alert admins if `day` is a trading day and its ban list hasn't been imported.

    Returns the date of the list currently applied (None if none was ever imported).
    """
    conn = await connect(dsn)
    try:
        latest: date | None = await conn.fetchval(
            "SELECT max(trade_date) FROM fo_ban_import WHERE trade_date <= $1", day
        )
        if latest != day and await _is_trading_day(conn, day):
            await alert_admins(
                conn,
                "warn",
                "F&O ban list not imported",
                {
                    "trade_date": day.isoformat(),
                    "applied_list": latest.isoformat() if latest else None,
                    "action": "download fo_secban.csv from NSE and run: vyuha-quant ban import FILE",
                },
            )
        return latest
    finally:
        await conn.close()


async def _is_trading_day(conn: Conn, day: date) -> bool:
    if day.weekday() >= 5:
        return False
    holiday: bool = await conn.fetchval(
        "SELECT EXISTS (SELECT 1 FROM exchange_holiday WHERE exchange = 'NFO' AND day = $1 AND special_session IS NULL)",
        day,
    )
    return not holiday
