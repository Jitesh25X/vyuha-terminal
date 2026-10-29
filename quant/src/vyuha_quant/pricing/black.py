"""Generalized Black-Scholes (cost-of-carry form) for European options — RISK_ENGINE §6.1.

One kernel covers both models the desk uses:
- Black-76 on a forward/futures price F:  S = F, b = 0
- Black-Scholes-Merton on spot with continuous dividend yield q:  b = r − q

    d1 = (ln(S/K) + (b + σ²/2)·T) / (σ·√T),   d2 = d1 − σ·√T
    call = S·e^{(b−r)T}·N(d1) − K·e^{−rT}·N(d2)
    put  = K·e^{−rT}·N(−d2) − S·e^{(b−r)T}·N(−d1)

Units: S, K and prices in rupees per unit of underlying (float64, analytics only — invariant 8);
T in years (ACT/365, see `year_fraction`); r, b, q, σ continuously compounded annual decimals
(0.055 = 5.5 %). All functions broadcast over NumPy arrays; `is_call` may be a bool or bool array.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import ArrayLike, NDArray
from scipy.special import ndtr

F64 = NDArray[np.float64]

_INV_SQRT_2PI = 1.0 / np.sqrt(2.0 * np.pi)


def _pdf(x: F64) -> F64:
    return np.asarray(_INV_SQRT_2PI * np.exp(-0.5 * x * x), dtype=np.float64)


def _d1d2(s: F64, k: F64, t: F64, b: F64, sigma: F64) -> tuple[F64, F64, F64]:
    vt = sigma * np.sqrt(t)
    d1 = (np.log(s / k) + (b + 0.5 * sigma * sigma) * t) / vt
    return d1, d1 - vt, vt


def _arrays(*xs: ArrayLike) -> list[F64]:
    return [np.asarray(x, dtype=np.float64) for x in xs]


def gbs_price(
    is_call: ArrayLike, s: ArrayLike, k: ArrayLike, t: ArrayLike, r: ArrayLike, b: ArrayLike, sigma: ArrayLike
) -> F64:
    """Option premium (rupees per unit) under the generalized Black-Scholes model (formula above)."""
    cp = np.where(np.asarray(is_call, dtype=bool), 1.0, -1.0)
    s_, k_, t_, r_, b_, sig = _arrays(s, k, t, r, b, sigma)
    d1, d2, _ = _d1d2(s_, k_, t_, b_, sig)
    carry = s_ * np.exp((b_ - r_) * t_)
    disc = k_ * np.exp(-r_ * t_)
    return np.asarray(cp * (carry * ndtr(cp * d1) - disc * ndtr(cp * d2)), dtype=np.float64)


def black76_price(
    is_call: ArrayLike, f: ArrayLike, k: ArrayLike, t: ArrayLike, r: ArrayLike, sigma: ArrayLike
) -> F64:
    """Black-76 premium on forward F, discounted at r (index options, RISK_ENGINE §6.1)."""
    return gbs_price(is_call, f, k, t, r, 0.0, sigma)


def bsm_price(
    is_call: ArrayLike, s: ArrayLike, k: ArrayLike, t: ArrayLike, r: ArrayLike, q: ArrayLike, sigma: ArrayLike
) -> F64:
    """Black-Scholes-Merton premium on spot S with continuous dividend yield q (stock options)."""
    return gbs_price(
        is_call, s, k, t, r, np.asarray(r, dtype=np.float64) - np.asarray(q, dtype=np.float64), sigma
    )


@dataclass(frozen=True, slots=True)
class Greeks:
    """Per-unit analytic Greeks with respect to the model's underlying S (the forward for Black-76).

    delta: ∂V/∂S (dimensionless).            gamma: ∂²V/∂S² (per rupee).
    theta: −∂V/∂T per calendar day (₹/day).  vega: ∂V/∂σ per 1 vol point (₹ per 0.01 σ).
    """

    delta: F64
    gamma: F64
    theta: F64
    vega: F64


def gbs_greeks(
    is_call: ArrayLike, s: ArrayLike, k: ArrayLike, t: ArrayLike, r: ArrayLike, b: ArrayLike, sigma: ArrayLike
) -> Greeks:
    """Analytic Greeks (Haug §2.1) for the generalized model; conventions in `Greeks`.

    delta = cp·e^{(b−r)T}·N(cp·d1)
    gamma = e^{(b−r)T}·n(d1) / (S·σ·√T)
    vega  = S·e^{(b−r)T}·n(d1)·√T / 100
    theta = [−S·e^{(b−r)T}·n(d1)·σ/(2√T) − cp·(b−r)·S·e^{(b−r)T}·N(cp·d1) − cp·r·K·e^{−rT}·N(cp·d2)] / 365
    """
    cp = np.where(np.asarray(is_call, dtype=bool), 1.0, -1.0)
    s_, k_, t_, r_, b_, sig = _arrays(s, k, t, r, b, sigma)
    d1, d2, vt = _d1d2(s_, k_, t_, b_, sig)
    eb = np.exp((b_ - r_) * t_)
    n1 = _pdf(d1)
    nd1, nd2 = ndtr(cp * d1), ndtr(cp * d2)
    sqrt_t = np.sqrt(t_)
    delta = cp * eb * nd1
    gamma = eb * n1 / (s_ * vt)
    vega = s_ * eb * n1 * sqrt_t
    theta = (
        -s_ * eb * n1 * sig / (2.0 * sqrt_t)
        - cp * (b_ - r_) * s_ * eb * nd1
        - cp * r_ * k_ * np.exp(-r_ * t_) * nd2
    )
    return Greeks(
        delta=np.asarray(delta, dtype=np.float64),
        gamma=np.asarray(gamma, dtype=np.float64),
        theta=np.asarray(theta / 365.0, dtype=np.float64),
        vega=np.asarray(vega / 100.0, dtype=np.float64),
    )
