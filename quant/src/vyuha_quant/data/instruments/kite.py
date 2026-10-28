"""Kite Connect instruments dump (gzipped CSV, one file per exchange).

Source: https://kite.trade/docs/connect/v3/market-quotes/#instruments (verified 2026-10-28).
Columns: instrument_token, exchange_token, tradingsymbol, name, last_price, expiry (YYYY-MM-DD),
strike, tick_size (rupees), lot_size, instrument_type (EQ/FUT/CE/PE), segment, exchange.
Index rows have segment INDICES with tick_size/lot_size 0.
"""

from __future__ import annotations

import csv
import io
from datetime import date
from decimal import Decimal

from .model import Exchange, OptionType, Segment, SourceRow

URL = "https://api.kite.trade/instruments/{exchange}"
EXCHANGES: tuple[Exchange, ...] = ("NSE", "BSE", "NFO", "BFO")

_SEGMENTS: dict[str, Segment] = {
    "NSE": "EQ",
    "BSE": "EQ",
    "INDICES": "IDX",
    "NFO-FUT": "FUT",
    "NFO-OPT": "OPT",
    "BFO-FUT": "FUT",
    "BFO-OPT": "OPT",
}
_COLUMNS = ["instrument_token", "exchange_token", "tradingsymbol", "name", "expiry", "strike", "tick_size",
            "lot_size", "instrument_type", "segment", "exchange"]  # fmt: skip


class KiteFormatError(ValueError):
    pass


def parse(text: str) -> list[SourceRow]:
    reader = csv.DictReader(io.StringIO(text))
    missing = set(_COLUMNS) - set(reader.fieldnames or [])
    if missing:
        raise KiteFormatError(f"kite instruments: missing columns {sorted(missing)}")
    rows: list[SourceRow] = []
    for r in reader:
        exchange = r["exchange"]
        segment = _SEGMENTS.get(r["segment"])
        if exchange not in EXCHANGES or segment is None:
            continue
        rows.append(_row(r, exchange, segment))
    return rows


def _row(r: dict[str, str], exchange: Exchange, segment: Segment) -> SourceRow:
    ts = r["tradingsymbol"].strip()
    option_type: OptionType | None = None
    if segment == "OPT":
        if r["instrument_type"] not in ("CE", "PE"):
            raise KiteFormatError(f"kite {ts}: option with instrument_type {r['instrument_type']!r}")
        option_type = r["instrument_type"]  # type: ignore[assignment]
    derivative = segment in ("FUT", "OPT")
    name = r["name"].strip() or None
    if derivative:
        symbol = name or ts
    elif segment == "EQ":
        symbol = ts.split("-")[0]  # "GOLDSTAR-ST" → series suffix dropped
    else:
        symbol = ts
    tick = Decimal(r["tick_size"] or "0")
    lot = int(r["lot_size"] or "0")
    if segment == "IDX":  # not tradable: the dump carries zeros
        tick = tick if tick > 0 else Decimal("0.05")
        lot = lot if lot > 0 else 1
    if tick <= 0 or lot <= 0:
        raise KiteFormatError(f"kite {exchange}:{ts}: tick_size {tick} / lot_size {lot} must be positive")
    return SourceRow(
        exchange=exchange,
        segment=segment,
        exchange_token=r["exchange_token"].strip(),
        tradingsymbol=ts,
        symbol=symbol,
        name=name,
        expiry=date.fromisoformat(r["expiry"]) if derivative else None,
        strike=Decimal(r["strike"]) if segment == "OPT" else None,
        option_type=option_type,
        lot_size=lot,
        tick_size=tick,
        broker_id=r["instrument_token"].strip(),
    )
