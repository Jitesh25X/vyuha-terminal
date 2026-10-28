# ruff: noqa: S608 -- SQL here is assembled only from this module's constant column lists; all values are bind parameters.
"""Apply a merged instrument master to Postgres (one transaction; the caller commits or rolls back).

Row lifecycle (`valid_from` / `valid_to`, `valid_to` exclusive):
- a contract keeps its `instrument.id` while it stays listed; mutable fields (lot size, tick size,
  freeze qty, broker mappings, names) are updated in place;
- a contract missing from today's files is closed with `valid_to = sync date`;
- a contract whose identity changes under the same trading symbol (segment, expiry, strike, option
  type) is closed and re-inserted, so a re-used symbol never inherits another contract's id;
- re-running the sync on the same day is idempotent.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date

from ...db import Conn
from .model import Instrument

_COLUMNS = (
    "exchange", "segment", "symbol", "tradingsymbol", "name", "isin", "expiry", "strike", "option_type",
    "lot_size", "tick_size", "freeze_qty", "kite_token", "upstox_key", "partial_mapping", "u_exchange",
    "u_tradingsymbol",
)  # fmt: skip

_KEY = "i.exchange = s.exchange AND i.tradingsymbol = s.tradingsymbol"
_IDENTITY = "(i.segment, i.expiry, i.strike, i.option_type) IS DISTINCT FROM (s.segment, s.expiry, s.strike, s.option_type)"
_MUTABLE = (
    "symbol",
    "name",
    "isin",
    "lot_size",
    "tick_size",
    "freeze_qty",
    "kite_token",
    "upstox_key",
    "partial_mapping",
)


@dataclass(frozen=True, slots=True)
class ApplyStats:
    active_before: int
    inserted: int
    updated: int
    closed: int
    reopened: int
    active_after: int


def _record(i: Instrument) -> tuple[object, ...]:
    u_ex, u_ts = i.underlying if i.underlying else (None, None)
    return (
        i.exchange, i.segment, i.symbol, i.tradingsymbol, i.name, i.isin, i.expiry, i.strike, i.option_type,
        i.lot_size, i.tick_size, i.freeze_qty, i.kite_token, i.upstox_key, i.partial_mapping, u_ex, u_ts,
    )  # fmt: skip


def _count(status: str) -> int:
    return int(status.split()[-1])


async def active_count(conn: Conn) -> int:
    n: int = await conn.fetchval("SELECT count(*) FROM instrument WHERE valid_to IS NULL")
    return n


async def apply(conn: Conn, instruments: list[Instrument], day: date) -> ApplyStats:
    before = await active_count(conn)
    await conn.execute(
        """CREATE TEMP TABLE stg (
             exchange exchange_code NOT NULL, segment segment_code NOT NULL, symbol text NOT NULL,
             tradingsymbol text NOT NULL, name text, isin text, expiry date, strike numeric(18,4),
             option_type option_type, lot_size int NOT NULL, tick_size numeric(18,4) NOT NULL, freeze_qty int,
             kite_token bigint, upstox_key text, partial_mapping boolean NOT NULL,
             u_exchange exchange_code, u_tradingsymbol text,
             PRIMARY KEY (exchange, tradingsymbol)
           ) ON COMMIT DROP"""
    )
    await conn.copy_records_to_table("stg", records=[_record(i) for i in instruments], columns=_COLUMNS)

    # Listed again after being closed earlier today (same-day re-run): reopen instead of duplicating.
    reopened = _count(
        await conn.execute(
            f"""UPDATE instrument i SET valid_to = NULL FROM stg s
                WHERE {_KEY} AND i.valid_to = $1 AND NOT {_IDENTITY}
                  AND NOT EXISTS (SELECT 1 FROM instrument a WHERE a.exchange = i.exchange
                                  AND a.tradingsymbol = i.tradingsymbol AND a.valid_to IS NULL)""",
            day,
        )
    )
    # Delisted/expired, or same symbol now naming a different contract: close.
    closed = _count(
        await conn.execute(
            f"""UPDATE instrument i SET valid_to = $1
                WHERE i.valid_to IS NULL AND i.valid_from <= $1 AND (
                  NOT EXISTS (SELECT 1 FROM stg s WHERE {_KEY})
                  OR EXISTS (SELECT 1 FROM stg s WHERE {_KEY} AND {_IDENTITY}))""",
            day,
        )
    )
    sets = ", ".join(f"{c} = s.{c}" for c in _MUTABLE)
    updated = _count(
        await conn.execute(
            f"""UPDATE instrument i SET {sets} FROM stg s
                WHERE {_KEY} AND i.valid_to IS NULL
                  AND ({", ".join(f"i.{c}" for c in _MUTABLE)}) IS DISTINCT FROM ({", ".join(f"s.{c}" for c in _MUTABLE)})"""
        )
    )
    cols = ", ".join(_COLUMNS[:15])
    inserted = _count(
        await conn.execute(
            f"""INSERT INTO instrument ({cols}, valid_from)
                SELECT {", ".join(f"s.{c}" for c in _COLUMNS[:15])}, $1 FROM stg s
                WHERE NOT EXISTS (SELECT 1 FROM instrument i WHERE {_KEY} AND i.valid_to IS NULL)""",
            day,
        )
    )
    await conn.execute(
        f"""UPDATE instrument i SET underlying_id = u.id
            FROM stg s LEFT JOIN instrument u
              ON u.exchange = s.u_exchange AND u.tradingsymbol = s.u_tradingsymbol AND u.valid_to IS NULL
            WHERE {_KEY} AND i.valid_to IS NULL AND i.underlying_id IS DISTINCT FROM u.id"""
    )
    return ApplyStats(
        active_before=before,
        inserted=inserted,
        updated=updated,
        closed=closed,
        reopened=reopened,
        active_after=await active_count(conn),
    )


async def apply_fo_ban(conn: Conn, day: date) -> tuple[date | None, int]:
    """Set `fo_ban` on active NFO derivatives from the latest imported list on or before `day`.

    Returns (list date used, contracts banned). With no list at all, flags are left unchanged.
    """
    row = await conn.fetchrow(
        "SELECT trade_date, symbols FROM fo_ban_import WHERE trade_date <= $1 ORDER BY trade_date DESC LIMIT 1",
        day,
    )
    if row is None:
        return None, 0
    await conn.execute(
        """UPDATE instrument SET fo_ban = (exchange = 'NFO' AND segment IN ('FUT','OPT') AND symbol = ANY($1::text[]))
           WHERE valid_to IS NULL
             AND fo_ban IS DISTINCT FROM (exchange = 'NFO' AND segment IN ('FUT','OPT') AND symbol = ANY($1::text[]))""",
        row["symbols"],
    )
    banned: int = await conn.fetchval("SELECT count(*) FROM instrument WHERE valid_to IS NULL AND fo_ban")
    trade_date: date = row["trade_date"]
    return trade_date, banned
