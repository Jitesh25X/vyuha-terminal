# ruff: noqa: S608 -- SQL here is assembled only from this module's constants; all values are bind parameters.
"""Freeze quantities the day's file does not supply (D-52).

The store keeps a contract's last known value. This module fills an F&O contract that has none from another
contract of the same underlying, and tells admins about every value they should check against the exchange.

Every active F&O contract is in at most one category:
- inherited: copied from another contract (`freeze_qty_from_id` set);
- carried: its own value, but not from this file;
- unknown: no value at all.

Trading days are weekdays without an `exchange_holiday` row for the exchange (a row with a special session
counts as a holiday too). The table has no coverage marker: a year with no rows for an exchange is taken as not
covered, its weekdays count as trading days, and the carried alert says so.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Any

from ...db import Conn, alert_admins, connect

log = logging.getLogger(__name__)

# A carried value is alerted once this many trading days have passed since its file date.
CARRIED_ALERT_DAYS = 2
MAX_LISTED = 50
ACTION = "Check the NSE circular for the current freeze quantity limits."

_FNO = "segment IN ('FUT','OPT')"


async def inherit(conn: Conn) -> None:
    """Give each active F&O contract without its own value the value of a donor: an active contract on the same
    exchange and underlying that has its OWN value (never an inherited one). Re-derived on every sync; with no
    donor left, what the contract already has is kept.

    Donors that disagree: take the SMALLEST. It is the conservative choice, since too small a slice only means
    more orders, while too large a one is rejected by the exchange.
    """
    await conn.execute(
        f"""UPDATE instrument t
            SET freeze_qty = d.freeze_qty, freeze_qty_as_of = d.freeze_qty_as_of, freeze_qty_from_id = d.id
            FROM (SELECT DISTINCT ON (exchange, symbol) exchange, symbol, id, freeze_qty, freeze_qty_as_of
                  FROM instrument
                  WHERE valid_to IS NULL AND {_FNO} AND freeze_qty > 0 AND freeze_qty_from_id IS NULL
                  ORDER BY exchange, symbol, freeze_qty, freeze_qty_as_of DESC NULLS LAST, id) d
            WHERE t.valid_to IS NULL AND t.{_FNO} AND t.exchange = d.exchange AND t.symbol = d.symbol
              AND (t.freeze_qty IS NULL OR t.freeze_qty_from_id IS NOT NULL)
              AND (t.freeze_qty, t.freeze_qty_as_of, t.freeze_qty_from_id)
                  IS DISTINCT FROM (d.freeze_qty, d.freeze_qty_as_of, d.id)"""
    )


@dataclass(frozen=True, slots=True)
class Contract:
    exchange: str
    tradingsymbol: str
    underlying: str
    value: int | None
    as_of: date | None
    donor: str | None  # trading symbol an inherited value came from


@dataclass(frozen=True, slots=True)
class Status:
    """Active, unexpired F&O contracts whose value did not come from the file of `day`."""

    day: date
    carried: list[Contract]  # any age
    stale: list[Contract]  # the carried ones old enough to alert
    inherited: list[Contract]
    unknown: list[Contract]
    uncovered: list[str]  # "NFO 2027": holiday calendar years that were needed and have no rows


async def status(conn: Conn, day: date) -> Status:
    rows = await conn.fetch(
        f"""SELECT t.exchange, t.tradingsymbol, t.symbol, t.freeze_qty, t.freeze_qty_as_of,
                   t.freeze_qty_from_id, d.tradingsymbol AS donor
            FROM instrument t LEFT JOIN instrument d ON d.id = t.freeze_qty_from_id
            WHERE t.valid_to IS NULL AND t.{_FNO} AND t.expiry >= $1
              AND (t.freeze_qty IS NULL OR t.freeze_qty_from_id IS NOT NULL
                   OR t.freeze_qty_as_of IS DISTINCT FROM $1)
            ORDER BY t.exchange, t.tradingsymbol""",
        day,
    )
    carried: list[Contract] = []
    inherited: list[Contract] = []
    unknown: list[Contract] = []
    for r in rows:
        c = Contract(r["exchange"], r["tradingsymbol"], r["symbol"], r["freeze_qty"], r["freeze_qty_as_of"], r["donor"])  # fmt: skip
        if c.value is None:
            unknown.append(c)
        elif r["freeze_qty_from_id"] is not None:
            inherited.append(c)
        else:
            carried.append(c)

    holidays: set[tuple[str, date]] = {
        (r["exchange"], r["day"]) for r in await conn.fetch("SELECT exchange, day FROM exchange_holiday")
    }
    covered = {(exchange, d.year) for exchange, d in holidays}
    uncovered: set[str] = set()
    stale: list[Contract] = []
    for c in carried:
        if c.as_of is None:  # from before the date was recorded: old enough
            stale.append(c)
            continue
        elapsed = 0
        d = c.as_of + timedelta(days=1)
        while d <= day and elapsed < CARRIED_ALERT_DAYS:
            if d.weekday() < 5:
                if (c.exchange, d.year) not in covered:
                    uncovered.add(f"{c.exchange} {d.year}")
                if (c.exchange, d) not in holidays:
                    elapsed += 1
            d += timedelta(days=1)
        if elapsed >= CARRIED_ALERT_DAYS:
            stale.append(c)
    return Status(day, carried, stale, inherited, unknown, sorted(uncovered))


def _body(day: date, contracts: list[Contract]) -> dict[str, Any]:
    return {
        "file_date": day.isoformat(),
        "count": len(contracts),
        "underlyings": sorted({c.underlying for c in contracts}),
        "contracts": [
            {
                "exchange": c.exchange,
                "symbol": c.tradingsymbol,
                "value": c.value,
                "as_of": c.as_of.isoformat() if c.as_of else None,
                "donor": c.donor,
            }
            for c in contracts[:MAX_LISTED]
        ],
        "action": ACTION,
    }


async def raise_alerts(dsn: str, found: Status) -> None:
    """At most one alert per category. Never raises: the sync has already committed."""
    if found.uncovered:
        log.warning("no exchange_holiday rows for %s: weekdays counted as trading days", found.uncovered)
    for title, contracts in (
        ("Freeze quantity carried forward", found.stale),
        ("Freeze quantity inherited", found.inherited),
        ("Freeze quantity unknown", found.unknown),
    ):
        if not contracts:
            continue
        body = _body(found.day, contracts)
        if contracts is found.stale and found.uncovered:
            body["calendar_note"] = (
                f"no exchange_holiday rows for {', '.join(found.uncovered)}: weekdays counted as trading days"
            )
        try:
            conn = await connect(dsn)
            try:
                if await alert_admins(conn, "warn", title, body) == 0:
                    log.error("no active admin to alert: %s", title)
            finally:
                await conn.close()
        except Exception:
            log.exception("could not raise the admin alert: %s", title)
