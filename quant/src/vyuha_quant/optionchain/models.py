"""Wire models for POST /internal/optionchain (gateway → quant). Prices are decimal strings (rupees)."""

from __future__ import annotations

from datetime import date
from decimal import Decimal
from typing import Literal

from pydantic import AwareDatetime, BaseModel, Field


class SideQuote(BaseModel):
    """Latest market fields of one contract, from a feed the requesting user may see (ADR-020)."""

    ltp: Decimal | None = None
    bid: Decimal | None = None
    ask: Decimal | None = None
    oi: int = Field(default=0, ge=0)
    traded_at: AwareDatetime | None = None  # last time the gateway saw this contract trade (volume rose)


class StrikeIn(BaseModel):
    strike: Decimal = Field(gt=0)
    ce: SideQuote | None = None
    pe: SideQuote | None = None


class OiStrike(BaseModel):
    """Open interest of one strike across the whole expiry (PCR and max pain)."""

    strike: Decimal = Field(gt=0)
    ce_oi: int = Field(default=0, ge=0)
    pe_oi: int = Field(default=0, ge=0)


class Dividend(BaseModel):
    ex_date: date
    amount: Decimal = Field(gt=0)


class ChainRequest(BaseModel):
    kind: Literal["index", "stock"]
    now: AwareDatetime
    expiry: date
    tick_size: Decimal = Field(gt=0)
    spot: Decimal | None = None
    future: Decimal | None = None  # same-expiry futures LTP (index options)
    rate: float | None = Field(default=None, ge=-0.05, le=0.5)  # default: RISK_FREE_RATE
    dividends: list[Dividend] = Field(default_factory=list, max_length=50)
    strikes: list[StrikeIn] = Field(max_length=1000)  # display window: IV/Greeks
    oi: list[OiStrike] = Field(default_factory=list, max_length=2000)  # full expiry: PCR, max pain


IvSource = Literal["mid", "ltp"]


class SideOut(BaseModel):
    """Per-unit analytics; None where undefined (shown as —). Conventions: pricing.Greeks."""

    iv: float | None
    iv_source: IvSource | None
    delta: float | None
    gamma: float | None
    theta: float | None
    vega: float | None


class RowOut(BaseModel):
    strike: Decimal
    ce: SideOut | None
    pe: SideOut | None


ForwardSource = Literal["future", "synthetic_mid", "synthetic_ltp", "spot"]


class ChainResult(BaseModel):
    model: Literal["black76", "bsm"]
    underlying_price: float | None  # forward (Black-76) or dividend-adjusted spot (BSM)
    forward_source: ForwardSource | None
    t_years: float
    rate: float
    rate_as_of: date | None  # date the configured rate was set; None when the request overrode it
    atm_strike: Decimal | None
    pcr: float | None  # over the full expiry (`oi`)
    max_pain: Decimal | None
    oi_strikes: int  # strikes with OI behind PCR / max pain
    rows: list[RowOut]  # same order as the request's strikes
