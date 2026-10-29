"""Pricing kernels (RISK_ENGINE §6.1): published reference vectors, identities and IV round trips.

Reference values: Haug, *The Complete Guide to Option Pricing Formulas* (2nd ed.), generalized
Black-Scholes examples (§1.1-1.2), and Hull, *Options, Futures and Other Derivatives*, Example 15.6.
They are quoted to 4 decimals, so the checks allow half a unit in the last place.

TODO(M5.1): cross-check these kernels against QuantLib to 1e-6 (test-only dependency on M5.1's needs
line; formal pricing sign-off happens there). Until then: published vectors + finite differences.
"""

from __future__ import annotations

from datetime import UTC, date, datetime

import numpy as np
import pytest

from vyuha_quant.pricing import black76_price, bsm_price, gbs_greeks, gbs_price, implied_vol, year_fraction

Q = 5e-5  # half a unit in the 4th decimal


def f(x: object) -> float:
    return float(np.asarray(x))


# ── reference vectors ────────────────────────────────────────────────────────


def test_hull_bsm_example() -> None:
    # S=42, K=40, r=10%, σ=20%, T=0.5 → c=4.7594, p=0.8086
    assert f(bsm_price(True, 42.0, 40.0, 0.5, 0.10, 0.0, 0.20)) == pytest.approx(4.7594, abs=Q)
    assert f(bsm_price(False, 42.0, 40.0, 0.5, 0.10, 0.0, 0.20)) == pytest.approx(0.8086, abs=Q)


def test_haug_generalized_bsm_put() -> None:
    # S=75, K=70, T=0.5, r=10%, b=5% (q=5%), σ=35% → put 4.0870
    assert f(gbs_price(False, 75.0, 70.0, 0.5, 0.10, 0.05, 0.35)) == pytest.approx(4.0870, abs=Q)
    assert f(bsm_price(False, 75.0, 70.0, 0.5, 0.10, 0.05, 0.35)) == pytest.approx(4.0870, abs=Q)


def test_haug_black76() -> None:
    # F=19, K=19, T=0.75, r=10%, σ=28% → call = put = 1.7011
    assert f(black76_price(True, 19.0, 19.0, 0.75, 0.10, 0.28)) == pytest.approx(1.7011, abs=Q)
    assert f(black76_price(False, 19.0, 19.0, 0.75, 0.10, 0.28)) == pytest.approx(1.7011, abs=Q)


def test_haug_greeks() -> None:
    # Delta: futures option S=105, K=100, T=0.5, r=10%, b=0, σ=36% → call 0.5946, put −0.3566
    assert f(gbs_greeks(True, 105.0, 100.0, 0.5, 0.10, 0.0, 0.36).delta) == pytest.approx(0.5946, abs=Q)
    assert f(gbs_greeks(False, 105.0, 100.0, 0.5, 0.10, 0.0, 0.36).delta) == pytest.approx(-0.3566, abs=Q)
    # Gamma: S=55, K=60, T=0.75, r=10%, b=10%, σ=30% → 0.0278 (Haug quotes 4 dp)
    assert f(gbs_greeks(True, 55.0, 60.0, 0.75, 0.10, 0.10, 0.30).gamma) == pytest.approx(0.0278, abs=Q)
    # Vega has no published vector here; it is checked against finite differences below.
    # Theta: S=430, K=405, T=0.0833, r=7%, b=2%, σ=20% → put −31.1924 per year
    th = f(gbs_greeks(False, 430.0, 405.0, 0.0833, 0.07, 0.02, 0.20).theta)
    assert th * 365 == pytest.approx(-31.1924, abs=Q)


# ── identities ───────────────────────────────────────────────────────────────

GRID_S = np.array([80.0, 95.0, 100.0, 105.0, 130.0])
GRID_T = np.array([1 / 365, 7 / 365, 30 / 365, 0.5, 2.0])


@pytest.mark.parametrize("b", [0.0, 0.065, 0.03])
def test_put_call_parity(b: float) -> None:
    s, t = np.meshgrid(GRID_S, GRID_T)
    r, k, sig = 0.065, 100.0, 0.25
    c = gbs_price(True, s, k, t, r, b, sig)
    p = gbs_price(False, s, k, t, r, b, sig)
    np.testing.assert_allclose(c - p, s * np.exp((b - r) * t) - k * np.exp(-r * t), atol=1e-10)


@pytest.mark.parametrize("is_call", [True, False])
@pytest.mark.parametrize("b", [0.0, 0.065])
def test_greeks_match_finite_differences(is_call: bool, b: float) -> None:
    s, k, r, sig = 102.0, 100.0, 0.065, 0.22
    for t in (5 / 365, 0.25, 1.0):
        g = gbs_greeks(is_call, s, k, t, r, b, sig)
        h = 1e-3  # rupees: O(h²) truncation stays below 1e-7 even at 5 days to expiry
        up, mid, dn = (f(gbs_price(is_call, x, k, t, r, b, sig)) for x in (s + h, s, s - h))
        assert f(g.delta) == pytest.approx((up - dn) / (2 * h), rel=1e-6, abs=1e-9)
        assert f(g.gamma) == pytest.approx((up - 2 * mid + dn) / h**2, rel=1e-4)
        dv = 1e-5
        v_up = f(gbs_price(is_call, s, k, t, r, b, sig + dv))
        v_dn = f(gbs_price(is_call, s, k, t, r, b, sig - dv))
        assert f(g.vega) == pytest.approx((v_up - v_dn) / (2 * dv) / 100, rel=1e-6)
        dt = 1e-6
        t_up = f(gbs_price(is_call, s, k, t + dt, r, b, sig))
        t_dn = f(gbs_price(is_call, s, k, t - dt, r, b, sig))
        assert f(g.theta) == pytest.approx(-(t_up - t_dn) / (2 * dt) / 365, rel=1e-5)


# ── implied volatility ───────────────────────────────────────────────────────


@pytest.mark.parametrize("is_call", [True, False])
def test_iv_round_trip(is_call: bool) -> None:
    k = np.array([18000.0, 22000.0, 24000.0, 24300.0, 26000.0, 30000.0])
    t = 3 / 365
    for sig in (0.05, 0.12, 0.35, 1.2, 3.0):
        price = gbs_price(is_call, 24310.0, k, t, 0.055, 0.0, sig)
        iv = implied_vol(is_call, price, 24310.0, k, t, 0.055, 0.0, tick=0.05)
        # Every premium with at least one tick (₹0.05) of time value must round-trip. With less, σ is
        # not identifiable (deep ITM: any small σ fits; far OTM: float cancellation) and IV is NaN.
        disc_fwd = 24310.0 * np.exp(-0.055 * t) - k * np.exp(-0.055 * t)
        intrinsic = np.maximum(disc_fwd if is_call else -disc_fwd, 0.0)
        identifiable = price - intrinsic >= 0.05
        assert identifiable.any()
        np.testing.assert_allclose(iv[identifiable], sig, rtol=1e-6)
        # Elsewhere the solver either recovers σ (to the precision tiny vega allows) or says NaN.
        rest = ~identifiable & ~np.isnan(iv)
        np.testing.assert_allclose(iv[rest], sig, rtol=1e-4)


def test_iv_vectorised_shapes_and_mixed_flags() -> None:
    is_call = np.array([True, False, True, False])
    k = np.array([100.0, 100.0, 110.0, 90.0])
    sig = np.array([0.2, 0.3, 0.4, 0.25])
    price = gbs_price(is_call, 100.0, k, 0.5, 0.06, 0.06, sig)
    np.testing.assert_allclose(implied_vol(is_call, price, 100.0, k, 0.5, 0.06, 0.06), sig, rtol=1e-7)


def test_iv_nan_below_intrinsic_minus_tick() -> None:
    # Deep ITM futures call: intrinsic (discounted) ≈ 1,000 × e^{-rT}
    intrinsic = 1000 * np.exp(-0.06 * 0.1)
    assert np.isnan(implied_vol(True, intrinsic - 0.10, 25000.0, 24000.0, 0.1, 0.06, 0.0, tick=0.05))
    # Above the no-arbitrage upper bound (call > discounted forward): no solution either.
    assert np.isnan(implied_vol(True, 26000.0, 25000.0, 24000.0, 0.1, 0.06, 0.0, tick=0.05))
    # Zero/negative price, zero time: NaN, never an exception.
    assert np.isnan(implied_vol(False, 0.0, 100.0, 100.0, 0.1, 0.06, 0.0))
    assert np.isnan(implied_vol(False, 1.0, 100.0, 100.0, 0.0, 0.06, 0.0))


# ── time to expiry ───────────────────────────────────────────────────────────


def test_year_fraction_to_1530_ist_act365() -> None:
    # 09:30 IST = 04:00 UTC; expiry 15:30 IST same day = 10:00 UTC → 6 h
    now = datetime(2026, 10, 27, 4, 0, tzinfo=UTC)
    assert year_fraction(now, date(2026, 10, 27)) == pytest.approx(6 / (24 * 365))
    # A week out: 7 days + 6 h
    assert year_fraction(now, date(2026, 11, 3)) == pytest.approx((7 * 24 + 6) / (24 * 365))


def test_year_fraction_one_minute_floor() -> None:
    at_close = datetime(2026, 10, 27, 10, 0, tzinfo=UTC)  # 15:30 IST on expiry day
    assert year_fraction(at_close, date(2026, 10, 27)) == pytest.approx(60 / (365 * 86400))
    after = datetime(2026, 10, 27, 12, 0, tzinfo=UTC)
    assert year_fraction(after, date(2026, 10, 27)) == pytest.approx(60 / (365 * 86400))


def test_year_fraction_rejects_naive_datetimes() -> None:
    with pytest.raises(ValueError, match="timezone"):
        year_fraction(datetime(2026, 10, 27, 9, 30), date(2026, 10, 27))  # noqa: DTZ001
