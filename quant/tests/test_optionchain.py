"""Option-chain analytics (F-MD-3): forward, ATM, IV/Greeks per contract, PCR, max pain."""

from __future__ import annotations

import math
from datetime import UTC, date, datetime, timedelta
from decimal import Decimal

import numpy as np
import pytest

from vyuha_quant.optionchain import ChainPolicy, ChainRequest, compute_chain, max_pain, pcr
from vyuha_quant.pricing import gbs_greeks, gbs_price, year_fraction

NOW = datetime(2026, 10, 27, 4, 0, tzinfo=UTC)  # 09:30 IST
EXPIRY = date(2026, 11, 3)
R = 0.055
T = year_fraction(NOW, EXPIRY)
STRIKES = [Decimal(k) for k in range(24000, 24650, 50)]


def smile(k: float, f: float) -> float:
    return 0.12 + 0.6 * math.log(k / f) ** 2


def q(x: float) -> str:
    return f"{round(x / 0.05) * 0.05:.2f}"  # premium rounded to the ₹0.05 tick, as the exchange prints


def index_request(
    fwd: float, *, future: float | None, two_sided: bool = True, spot: float = 24290.0
) -> ChainRequest:
    rows = []
    for k in STRIKES:
        sides = {}
        for side, is_call in (("ce", True), ("pe", False)):
            p = float(gbs_price(is_call, fwd, float(k), T, R, 0.0, smile(float(k), fwd)))
            # Traded 5 s ago: LTP is usable wherever the quote is too wide for the mid.
            quote: dict[str, object] = {
                "ltp": q(p),
                "oi": 1000,
                "traded_at": (NOW - timedelta(seconds=5)).isoformat(),
            }
            if two_sided:
                quote |= {"bid": q(p - 0.5), "ask": q(p + 0.5)}
            sides[side] = quote
        rows.append({"strike": str(k), **sides})
    oi = [{"strike": str(k), "ce_oi": 1000, "pe_oi": 1000} for k in STRIKES]
    return ChainRequest.model_validate({
        "kind": "index", "now": NOW.isoformat(), "expiry": EXPIRY.isoformat(), "tick_size": "0.05",
        "spot": str(spot), "future": None if future is None else str(future), "rate": R, "strikes": rows, "oi": oi,
    })  # fmt: skip


def test_index_chain_uses_same_expiry_future_and_recovers_smile() -> None:
    res = compute_chain(index_request(24325.0, future=24325.0))
    assert res.model == "black76"
    assert res.forward_source == "future"
    assert res.underlying_price == pytest.approx(24325.0)
    assert res.t_years == pytest.approx(T)
    for row in res.rows:
        k = float(row.strike)
        for side, is_call in ((row.ce, True), (row.pe, False)):
            assert side is not None
            assert side.iv is not None
            # F-MD-3 AC: within 1 % of the reference. Premiums are tick-rounded, so allow that.
            assert side.iv == pytest.approx(smile(k, 24325.0), rel=0.01)
            g = gbs_greeks(is_call, 24325.0, k, T, R, 0.0, side.iv)
            assert side.delta == pytest.approx(float(g.delta))
            assert side.gamma == pytest.approx(float(g.gamma))
            assert side.theta == pytest.approx(float(g.theta))
            assert side.vega == pytest.approx(float(g.vega))
    assert {r.ce.delta > 0 and r.pe.delta < 0 for r in res.rows if r.ce and r.pe} == {True}  # type: ignore[operator]


def test_index_chain_without_future_uses_synthetic_forward_from_atm_mids() -> None:
    res = compute_chain(index_request(24325.0, future=None))
    assert res.forward_source == "synthetic_mid"
    assert res.underlying_price == pytest.approx(24325.0, abs=0.1)  # within two ticks of carry
    ce = res.rows[6].ce  # 24300
    assert ce is not None
    assert ce.iv == pytest.approx(smile(24300.0, 24325.0), rel=0.01)


def test_synthetic_forward_falls_back_to_ltp_when_no_two_sided_quotes() -> None:
    res = compute_chain(index_request(24325.0, future=None, two_sided=False))
    assert res.forward_source == "synthetic_ltp"
    assert res.underlying_price == pytest.approx(24325.0, abs=0.1)


def test_no_forward_means_no_iv_but_market_fields_survive() -> None:
    req = index_request(24325.0, future=None)
    for row in req.strikes:
        row.pe = None  # no puts: no parity, no future
    res = compute_chain(req)
    assert res.forward_source is None
    assert res.underlying_price is None
    assert all(r.ce is not None and r.ce.iv is None and r.ce.delta is None for r in res.rows)
    assert res.atm_strike == Decimal(24300)  # still from spot


def test_atm_strike_nearest_spot_ties_go_lower() -> None:
    assert compute_chain(index_request(24325.0, future=24325.0, spot=24290.0)).atm_strike == Decimal(24300)
    assert compute_chain(index_request(24325.0, future=24325.0, spot=24275.0)).atm_strike == Decimal(24250)


def test_stock_chain_bsm_on_spot_with_escrowed_dividend() -> None:
    spot, k, sig, div = 1450.0, 1450.0, 0.28, 10.0
    ex = date(2026, 10, 30)
    s_adj = spot - div * math.exp(-R * year_fraction(NOW, ex))
    ce = float(gbs_price(True, s_adj, k, T, R, R, sig))
    pe = float(gbs_price(False, s_adj, k, T, R, R, sig))
    req = ChainRequest.model_validate({
        "kind": "stock", "now": NOW.isoformat(), "expiry": EXPIRY.isoformat(), "tick_size": "0.05",
        "spot": str(spot), "rate": R, "dividends": [{"ex_date": ex.isoformat(), "amount": "10"},
                                                    {"ex_date": "2026-12-01", "amount": "99"}],  # after expiry: ignored
        "strikes": [{"strike": "1450", "ce": {"ltp": f"{ce:.4f}", "oi": 5, "traded_at": NOW.isoformat()},
                     "pe": {"ltp": f"{pe:.4f}", "oi": 7, "traded_at": NOW.isoformat()}}],
    })  # fmt: skip
    res = compute_chain(req)
    assert res.model == "bsm"
    assert res.forward_source == "spot"
    assert res.underlying_price == pytest.approx(s_adj)
    assert res.rows[0].ce is not None
    assert res.rows[0].ce.iv == pytest.approx(sig, rel=1e-6)
    assert res.rows[0].pe is not None
    assert res.rows[0].pe.iv == pytest.approx(sig, rel=1e-6)


def test_pcr_is_put_oi_over_call_oi() -> None:
    assert pcr(np.array([100, 200, 0]), np.array([150, 50, 100])) == pytest.approx(1.0)
    assert pcr(np.array([100, 300]), np.array([30, 30])) == pytest.approx(0.15)
    assert pcr(np.array([0, 0]), np.array([10, 0])) is None


def test_max_pain_minimises_writer_payout() -> None:
    # Payout at settlement K*: Σ CE_oi·max(K*−K, 0) + Σ PE_oi·max(K−K*, 0)
    # K*=100 → PE 5·10 + 40·20 = 850;  110 → CE 50·10 + PE 40·10 = 900;  120 → CE 50·20 + 10·10 = 1100
    strikes = np.array([100.0, 110.0, 120.0])
    assert max_pain(strikes, np.array([50, 10, 0]), np.array([0, 5, 40])) == 0
    # Mirror image → the top strike.
    assert max_pain(strikes, np.array([40, 5, 0]), np.array([0, 10, 50])) == 2
    assert max_pain(strikes, np.zeros(3), np.zeros(3)) is None


def test_iv_nan_below_intrinsic() -> None:
    req = index_request(24325.0, future=24325.0)
    req.strikes[0].ce.ltp = Decimal("10")  # type: ignore[union-attr]  # 24000 CE far below intrinsic (~325)
    req.strikes[0].ce.bid = req.strikes[0].ce.ask = None  # type: ignore[union-attr]
    res = compute_chain(req)
    assert res.rows[0].ce is not None
    assert res.rows[0].ce.iv is None


def test_pcr_and_max_pain_cover_the_full_expiry_not_the_window() -> None:
    req = index_request(24325.0, future=24325.0)
    req.strikes = req.strikes[5:8]  # display window: 24250-24350 only
    req.oi[0].ce_oi = 90_000  # 24000 CE, outside the window
    res = compute_chain(req)
    assert len(res.rows) == 3
    assert res.oi_strikes == 13
    assert res.pcr == pytest.approx(13_000 / 102_000)
    # Settling at 24000 costs put writers 1000·50·(0+1+…+12) = ₹39 L; one strike up adds 90,000·50 = ₹45 L
    # on the calls and saves only ₹6 L on puts ⇒ max pain is the bottom strike, outside the window.
    assert res.max_pain == Decimal(24000)
    # Strikes without any OI don't count; no OI list at all ⇒ no PCR / max pain.
    req.oi[1].ce_oi = req.oi[1].pe_oi = 0
    assert compute_chain(req).oi_strikes == 12
    req.oi = []
    res = compute_chain(req)
    assert (res.pcr, res.max_pain, res.oi_strikes) == (None, None, 0)


def side_quote(**kw: object) -> ChainRequest:
    """One 24300 strike, F = 24325: CE worth ~₹150 at σ 12 %."""
    ce = {"oi": 10} | kw
    return ChainRequest.model_validate({
        "kind": "index", "now": NOW.isoformat(), "expiry": EXPIRY.isoformat(), "tick_size": "0.05",
        "spot": "24290", "future": "24325", "rate": R, "strikes": [{"strike": "24300", "ce": ce}],
    })  # fmt: skip


FAIR = float(gbs_price(True, 24325.0, 24300.0, T, R, 0.0, 0.12))
FRESH = (NOW - timedelta(seconds=59)).isoformat()
STALE = (NOW - timedelta(seconds=61)).isoformat()


@pytest.mark.parametrize(
    ("quote", "source"),
    [
        (
            {"bid": f"{FAIR - 2:.2f}", "ask": f"{FAIR + 2:.2f}", "ltp": "1", "traded_at": FRESH},
            "mid",
        ),  # tight
        (
            {"bid": f"{FAIR - 20:.2f}", "ask": f"{FAIR + 20:.2f}", "ltp": f"{FAIR:.2f}", "traded_at": FRESH},
            "ltp",
        ),
        (
            {"bid": f"{FAIR - 20:.2f}", "ask": f"{FAIR + 20:.2f}", "ltp": f"{FAIR:.2f}", "traded_at": STALE},
            None,
        ),
        ({"bid": f"{FAIR - 20:.2f}", "ask": f"{FAIR + 20:.2f}", "ltp": f"{FAIR:.2f}"}, None),  # no trade seen
        ({"bid": f"{FAIR:.2f}", "ltp": f"{FAIR:.2f}", "traded_at": FRESH}, "ltp"),  # one-sided quote
        ({"ltp": f"{FAIR:.2f}", "traded_at": STALE}, None),
    ],
)
def test_iv_source_mid_if_tight_else_recent_ltp_else_none(
    quote: dict[str, object], source: str | None
) -> None:
    ce = compute_chain(side_quote(**quote)).rows[0].ce
    assert ce is not None
    assert ce.iv_source == source
    if source is None:
        assert (ce.iv, ce.delta, ce.vega) == (None, None, None)
    else:
        assert ce.iv == pytest.approx(0.12, rel=0.02)


def test_iv_policy_is_configurable() -> None:
    wide = side_quote(bid=f"{FAIR - 20:.2f}", ask=f"{FAIR + 20:.2f}")  # spread ≈ 27 % of mid
    assert compute_chain(wide).rows[0].ce.iv_source is None  # type: ignore[union-attr]
    loose = ChainPolicy(mid_max_spread=0.30)
    assert compute_chain(wide, loose).rows[0].ce.iv_source == "mid"  # type: ignore[union-attr]
    stale = side_quote(ltp=f"{FAIR:.2f}", traded_at=STALE)
    assert compute_chain(stale, ChainPolicy(ltp_max_age_s=120)).rows[0].ce.iv_source == "ltp"  # type: ignore[union-attr]


def test_rate_and_its_date_come_from_policy_unless_overridden() -> None:
    req = side_quote(ltp="1")
    req.rate = None
    res = compute_chain(req, ChainPolicy(rate=0.055, rate_as_of=date(2026, 10, 7)))
    assert (res.rate, res.rate_as_of) == (0.055, date(2026, 10, 7))
    req.rate = 0.06
    assert compute_chain(req, ChainPolicy(rate=0.055, rate_as_of=date(2026, 10, 7))).rate_as_of is None
