"""Option-chain analytics (F-MD-3, RISK_ENGINE §6.1).

- Index options: Black-76 on the forward = same-expiry futures LTP if given, else the synthetic forward
  from put-call parity at the strike where |C − P| is smallest: F = K + e^{rT}·(C − P), using mid
  prices (falling back to LTPs when no strike has two-sided quotes on both legs).
- Stock options: BSM on spot (q = 0) less the present value of dividends going ex before expiry
  (escrowed dividend).
- Premium used for IV (ChainPolicy): the mid when both sides are quoted and the spread is at most
  `mid_max_spread` of the mid (10 %); else the LTP when the contract traded within `ltp_max_age_s`
  (60 s); else none, and IV/Greeks are undefined (shown as —) rather than fitted to a stale price.
- PCR = Σ PE OI / Σ CE OI and max pain = strike K* minimising Σ CE_oi·max(K* − K, 0) +
  Σ PE_oi·max(K − K*, 0), both over the full expiry (`req.oi`: every strike with OI), independent of
  the displayed window (`req.strikes`).
- ATM = strike nearest spot (nearest forward when spot is unknown); ties go to the lower strike.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

import numpy as np
from numpy.typing import NDArray

from ..config import today_ist
from ..pricing import gbs_greeks, implied_vol, year_fraction
from .models import ChainRequest, ChainResult, ForwardSource, IvSource, RowOut, SideOut, SideQuote

F64 = NDArray[np.float64]


@dataclass(frozen=True, slots=True)
class ChainPolicy:
    """Desk settings for chain analytics (config: RISK_FREE_RATE[_AS_OF], IV_MID_MAX_SPREAD, IV_LTP_MAX_AGE_S)."""

    rate: float = 0.0
    rate_as_of: date | None = None
    mid_max_spread: float = 0.10  # spread / mid
    ltp_max_age_s: float = 60.0


def _pos(x: Decimal | None) -> float | None:
    return float(x) if x is not None and x > 0 else None


def _mid(q: SideQuote | None) -> float | None:
    if q is None:
        return None
    bid, ask = _pos(q.bid), _pos(q.ask)
    return 0.5 * (bid + ask) if bid is not None and ask is not None and ask >= bid else None


def _ltp(q: SideQuote | None) -> float | None:
    return _pos(q.ltp) if q is not None else None


def _premium(q: SideQuote | None, req: ChainRequest, policy: ChainPolicy) -> tuple[float, IvSource | None]:
    """The price IV is solved from, and where it came from (module docstring)."""
    if q is None:
        return math.nan, None
    bid, ask = _pos(q.bid), _pos(q.ask)
    if bid is not None and ask is not None and ask >= bid:
        mid = 0.5 * (bid + ask)
        if ask - bid <= policy.mid_max_spread * mid:
            return mid, "mid"
    ltp = _pos(q.ltp)
    if (
        ltp is not None
        and q.traded_at is not None
        and (req.now - q.traded_at).total_seconds() <= policy.ltp_max_age_s
    ):
        return ltp, "ltp"
    return math.nan, None


def pcr(ce_oi: NDArray[np.number], pe_oi: NDArray[np.number]) -> float | None:
    """Put-call ratio by open interest; None when there is no call OI."""
    calls = float(np.sum(ce_oi))
    return float(np.sum(pe_oi)) / calls if calls > 0 else None


def max_pain(strikes: F64, ce_oi: NDArray[np.number], pe_oi: NDArray[np.number]) -> int | None:
    """Index of the max-pain strike (formula in the module docstring); None without any OI."""
    if strikes.size == 0 or float(np.sum(ce_oi) + np.sum(pe_oi)) <= 0:
        return None
    settle = strikes[:, None]  # candidate settlement prices × contract strikes
    payout = (np.maximum(settle - strikes[None, :], 0.0) * ce_oi[None, :]).sum(axis=1) + (
        np.maximum(strikes[None, :] - settle, 0.0) * pe_oi[None, :]
    ).sum(axis=1)
    return int(np.argmin(payout))  # first minimum: ties go to the lower strike


def _nearest(strikes: F64, x: float | None) -> int | None:
    if x is None or strikes.size == 0:
        return None
    return int(np.argmin(np.abs(strikes - x)))  # first minimum: ties go to the lower strike


def _synthetic_forward(
    req: ChainRequest, strikes: F64, r: float, t: float
) -> tuple[float, ForwardSource] | None:
    sources: tuple[tuple[ForwardSource, Callable[[SideQuote | None], float | None]], ...] = (
        ("synthetic_mid", _mid),
        ("synthetic_ltp", _ltp),
    )
    for source, price in sources:
        best: tuple[float, float] | None = None  # (|C − P|, forward)
        for k, row in zip(strikes, req.strikes, strict=True):
            c, p = price(row.ce), price(row.pe)
            if c is None or p is None:
                continue
            gap = abs(c - p)
            if best is None or gap < best[0]:
                best = (gap, float(k) + math.exp(r * t) * (c - p))
        if best is not None:
            return best[1], source
    return None


def _side(src: IvSource | None, iv: float, delta: float, gamma: float, theta: float, vega: float) -> SideOut:
    def v(x: float) -> float | None:
        return x if math.isfinite(x) else None

    if not math.isfinite(iv):
        return SideOut(iv=None, iv_source=None, delta=None, gamma=None, theta=None, vega=None)
    return SideOut(iv=iv, iv_source=src, delta=v(delta), gamma=v(gamma), theta=v(theta), vega=v(vega))


def compute_chain(req: ChainRequest, policy: ChainPolicy = ChainPolicy()) -> ChainResult:  # noqa: B008
    """IV and Greeks per contract plus chain-level PCR, max pain and ATM (see module docstring)."""
    r = req.rate if req.rate is not None else policy.rate
    rate_as_of = policy.rate_as_of if req.rate is None else None
    t = year_fraction(req.now, req.expiry)
    strikes = np.array([float(s.strike) for s in req.strikes], dtype=np.float64)
    spot = _pos(req.spot)

    underlying: float | None
    source: ForwardSource | None
    if req.kind == "index":
        model, b = "black76", 0.0
        fut = _pos(req.future)
        if fut is not None:
            underlying, source = fut, "future"
        else:
            syn = _synthetic_forward(req, strikes, r, t)
            underlying, source = syn if syn else (None, None)
    else:
        model, b = "bsm", r
        today = today_ist(req.now)
        pv_div = sum(
            float(d.amount) * math.exp(-r * year_fraction(req.now, d.ex_date))
            for d in req.dividends
            if today < d.ex_date <= req.expiry
        )
        underlying = spot - pv_div if spot is not None and spot > pv_div else None
        source = "spot" if underlying is not None else None

    n = len(req.strikes)
    is_call = np.repeat(np.array([[True, False]]), n, axis=0).ravel()
    k2 = np.repeat(strikes, 2)
    picked = [_premium(q, req, policy) for s in req.strikes for q in (s.ce, s.pe)]
    prem = np.array([p for p, _ in picked], dtype=np.float64)
    if underlying is not None and n:
        iv = implied_vol(is_call, prem, underlying, k2, t, r, b, tick=float(req.tick_size))
        with np.errstate(invalid="ignore", divide="ignore"):
            g = gbs_greeks(is_call, underlying, k2, t, r, b, iv)
        cols = (iv, g.delta, g.gamma, g.theta, g.vega)
    else:
        nan = np.full(2 * n, np.nan)
        cols = (nan, nan, nan, nan, nan)

    rows: list[RowOut] = []
    for i, s in enumerate(req.strikes):
        out = [_side(picked[2 * i + j][1], *(float(c[2 * i + j]) for c in cols)) for j in (0, 1)]
        rows.append(RowOut(strike=s.strike, ce=out[0] if s.ce else None, pe=out[1] if s.pe else None))

    full = sorted((o for o in req.oi if o.ce_oi > 0 or o.pe_oi > 0), key=lambda o: o.strike)
    oi_k = np.array([float(o.strike) for o in full], dtype=np.float64)
    ce_oi = np.array([o.ce_oi for o in full], dtype=np.int64)
    pe_oi = np.array([o.pe_oi for o in full], dtype=np.int64)
    mp = max_pain(oi_k, ce_oi, pe_oi)
    atm = _nearest(strikes, spot if spot is not None else underlying)
    return ChainResult(
        model=model,  # type: ignore[arg-type]
        underlying_price=underlying,
        forward_source=source,
        t_years=t,
        rate=r,
        rate_as_of=rate_as_of,
        atm_strike=req.strikes[atm].strike if atm is not None else None,
        pcr=pcr(ce_oi, pe_oi),
        max_pain=full[mp].strike if mp is not None else None,
        oi_strikes=len(full),
        rows=rows,
    )
