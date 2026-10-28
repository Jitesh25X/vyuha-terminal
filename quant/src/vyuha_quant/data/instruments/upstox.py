"""Upstox BOD instrument files (gzipped JSON array, one file per exchange).

Source: https://upstox.com/developer/api-documentation/instruments/ (verified 2026-10-28).
Undocumented units, verified against the 2026-10-28 files and Kite's dump for all 34,883 NFO
contracts: `tick_size` is in **paise**; `expiry` is epoch **milliseconds** (23:59:59 IST on the
expiry day); `exchange_token` is a string; `freeze_quantity` is units (float).
"""

from __future__ import annotations

from datetime import date, datetime
from decimal import Decimal
from typing import Any

from pydantic import BaseModel, ConfigDict, ValidationError

from ...config import IST
from .model import Exchange, OptionType, Segment, SourceRow

URL = "https://assets.upstox.com/market-quote/instruments/exchange/{file}.json.gz"
FILES = ("NSE", "BSE")

_SEGMENTS: dict[str, Exchange] = {
    "NSE_EQ": "NSE",
    "BSE_EQ": "BSE",
    "NSE_INDEX": "NSE",
    "BSE_INDEX": "BSE",
    "NSE_FO": "NFO",
    "BSE_FO": "BFO",
}


class UpstoxFormatError(ValueError):
    pass


class _Row(BaseModel):
    model_config = ConfigDict(extra="ignore", frozen=True)

    segment: str
    instrument_type: str
    instrument_key: str
    exchange_token: str | int
    trading_symbol: str
    name: str | None = None
    isin: str | None = None
    expiry: int | None = None
    strike_price: float | None = None
    lot_size: int | None = None
    tick_size: float | None = None
    freeze_quantity: float | None = None
    underlying_symbol: str | None = None
    underlying_key: str | None = None


def parse(items: list[Any]) -> list[SourceRow]:
    rows: list[SourceRow] = []
    for raw in items:
        if not isinstance(raw, dict) or raw.get("segment") not in _SEGMENTS:
            continue
        try:
            r = _Row.model_validate(raw)
        except ValidationError as e:
            raise UpstoxFormatError(f"upstox {raw.get('instrument_key')}: {e.errors()[0]['msg']}") from e
        rows.append(_row(r))
    return rows


def expiry_date(ms: int) -> date:
    return datetime.fromtimestamp(ms / 1000, IST).date()


def _row(r: _Row) -> SourceRow:
    exchange = _SEGMENTS[r.segment]
    segment: Segment
    option_type: OptionType | None = None
    if r.segment.endswith("_INDEX"):
        segment = "IDX"
    elif r.segment.endswith("_EQ"):
        segment = "EQ"
    elif r.instrument_type == "FUT":
        segment = "FUT"
    elif r.instrument_type in ("CE", "PE"):
        segment = "OPT"
        option_type = r.instrument_type  # type: ignore[assignment]
    else:
        raise UpstoxFormatError(f"upstox {r.instrument_key}: unknown derivative type {r.instrument_type!r}")
    derivative = segment in ("FUT", "OPT")
    if derivative and r.expiry is None:
        raise UpstoxFormatError(f"upstox {r.instrument_key}: derivative without expiry")
    if segment == "OPT" and r.strike_price is None:
        raise UpstoxFormatError(f"upstox {r.instrument_key}: option without strike")
    tick = (Decimal(str(r.tick_size)) / 100) if r.tick_size else Decimal("0.05")
    lot = r.lot_size or 1
    if tick <= 0 or lot <= 0:
        raise UpstoxFormatError(f"upstox {r.instrument_key}: tick_size/lot_size must be positive")
    ts = r.trading_symbol.strip()
    return SourceRow(
        exchange=exchange,
        segment=segment,
        exchange_token=str(r.exchange_token),
        tradingsymbol=ts,
        symbol=(r.underlying_symbol or ts) if derivative else ts,
        name=r.name,
        expiry=expiry_date(r.expiry) if derivative and r.expiry is not None else None,
        strike=Decimal(str(r.strike_price)).normalize()
        if segment == "OPT" and r.strike_price is not None
        else None,
        option_type=option_type,
        lot_size=lot,
        tick_size=tick.normalize(),
        broker_id=r.instrument_key,
        isin=r.isin,
        freeze_qty=int(r.freeze_quantity) if r.freeze_quantity else None,
        underlying_key=r.underlying_key,
    )
