"""Canonical instrument rows shared by the parsers, the merge and the store."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from typing import Literal

Exchange = Literal["NSE", "BSE", "NFO", "BFO"]
Segment = Literal["EQ", "FUT", "OPT", "IDX"]
OptionType = Literal["CE", "PE"]

# Derivatives trade on NFO/BFO; their cash/index underlyings live on NSE/BSE.
CASH_EXCHANGE: dict[str, Exchange] = {"NFO": "NSE", "BFO": "BSE", "NSE": "NSE", "BSE": "BSE"}


@dataclass(frozen=True, slots=True)
class SourceRow:
    """One broker's view of an instrument, already in canonical units (rupees, IST dates)."""

    exchange: Exchange
    segment: Segment
    exchange_token: str
    tradingsymbol: str
    symbol: str  # underlying symbol for derivatives, the security itself otherwise
    name: str | None
    expiry: date | None
    strike: Decimal | None
    option_type: OptionType | None
    lot_size: int
    tick_size: Decimal  # rupees
    broker_id: str  # kite instrument_token or upstox instrument_key
    isin: str | None = None
    freeze_qty: int | None = None
    underlying_key: str | None = None  # upstox only


@dataclass(frozen=True, slots=True)
class Instrument:
    exchange: Exchange
    segment: Segment
    symbol: str
    tradingsymbol: str
    name: str | None
    isin: str | None
    expiry: date | None
    strike: Decimal | None
    option_type: OptionType | None
    lot_size: int
    tick_size: Decimal
    freeze_qty: int | None
    kite_token: int | None
    upstox_key: str | None
    underlying: tuple[Exchange, str] | None  # (exchange, tradingsymbol) of the underlying instrument

    @property
    def partial_mapping(self) -> bool:
        return self.kite_token is None or self.upstox_key is None
