"""Implied volatility and time to expiry — RISK_ENGINE §6.1."""

from __future__ import annotations

from datetime import date, datetime, time, timedelta

import numpy as np
from numpy.typing import ArrayLike
from scipy.optimize import brentq

from ..config import IST
from .black import F64, gbs_price

SIGMA_LO, SIGMA_HI = 0.001, 5.0
XTOL = 1e-8
MAX_ITER = 100

_EXPIRY_CLOSE = time(15, 30)
_MIN_T_SECONDS = 60.0
_YEAR_SECONDS = 365.0 * 86400.0


def year_fraction(now: datetime, expiry: date) -> float:
    """Years (ACT/365) from `now` to 15:30 IST on `expiry`, floored at one minute.

    `now` must be timezone-aware (invariant 9). Past expiries also return the floor, so callers never
    divide by zero; filtering expired contracts is the caller's job.
    """
    if now.tzinfo is None:
        raise ValueError("now must carry a timezone")
    close = datetime.combine(expiry, _EXPIRY_CLOSE, tzinfo=IST)
    secs = (close - now) / timedelta(seconds=1)
    return max(secs, _MIN_T_SECONDS) / _YEAR_SECONDS


def _one(is_call: bool, price: float, s: float, k: float, t: float, r: float, b: float, tick: float) -> float:
    if not (price > 0 and s > 0 and k > 0 and t > 0) or not np.isfinite([price, s, k, t, r, b]).all():
        return float("nan")
    carry, disc = s * np.exp((b - r) * t), k * np.exp(-r * t)
    intrinsic = max(carry - disc, 0.0) if is_call else max(disc - carry, 0.0)
    upper = carry if is_call else disc
    if price < intrinsic - tick or price >= upper:
        return float("nan")

    def err(sig: float) -> float:
        return float(gbs_price(is_call, s, k, t, r, b, sig)) - price

    lo, hi = err(SIGMA_LO), err(SIGMA_HI)
    if lo >= 0 or hi < 0:
        # At or below the σ = 0.1 % value (no time value left, e.g. deep ITM near expiry: every small σ
        # fits) or beyond σ = 500 %: not identifiable.
        return float("nan")
    try:
        return float(brentq(err, SIGMA_LO, SIGMA_HI, xtol=XTOL, maxiter=MAX_ITER))
    except (RuntimeError, ValueError):
        pass
    a, z = SIGMA_LO, SIGMA_HI  # bisection fallback: the bracket above guarantees a sign change
    for _ in range(MAX_ITER):
        m = 0.5 * (a + z)
        if err(m) > 0:
            z = m
        else:
            a = m
        if z - a < XTOL:
            break
    return 0.5 * (a + z)


def implied_vol(
    is_call: ArrayLike,
    price: ArrayLike,
    s: ArrayLike,
    k: ArrayLike,
    t: ArrayLike,
    r: ArrayLike,
    b: ArrayLike,
    tick: ArrayLike = 0.0,
) -> F64:
    """σ such that `gbs_price(...) == price`, element-wise (arguments broadcast; see `black.py`).

    Brent's method on [0.001, 5.0], xtol 1e-8, ≤ 100 iterations, bisection fallback. Returns NaN when
    the price is below intrinsic − 1 tick (`tick`, rupees), at or above the no-arbitrage upper bound,
    or not bracketed by the σ range; also for non-positive price, spot, strike or time.
    """
    args = np.broadcast_arrays(
        np.asarray(is_call, dtype=bool),
        *(np.asarray(x, dtype=np.float64) for x in (price, s, k, t, r, b, tick)),
    )
    out = np.empty(args[0].shape, dtype=np.float64)
    flat = [a.ravel() for a in args]
    for i in range(out.size):
        c, p, s_, k_, t_, r_, b_, tk = (a[i] for a in flat)
        out.flat[i] = _one(
            bool(c), float(p), float(s_), float(k_), float(t_), float(r_), float(b_), float(tk)
        )
    return out
