"""Internal API: request validation and the optionchain round trip over HTTP."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from vyuha_quant import config
from vyuha_quant.api.app import create_app


@pytest.fixture
def client() -> TestClient:
    env = {
        "VYUHA_ENV": "dev",
        "RISK_FREE_RATE": "0.06",
        "RISK_FREE_RATE_AS_OF": "2026-10-07",
        "IV_MID_MAX_SPREAD": "0.5",
    }
    return TestClient(create_app(config.load(env)))


BODY = {
    "kind": "index",
    "now": "2026-10-27T04:00:00Z",
    "expiry": "2026-11-03",
    "tick_size": "0.05",
    "spot": "24290",
    "future": "24325",
    "strikes": [
        {"strike": "24300", "ce": {"ltp": "180.5", "bid": "180", "ask": "181", "oi": 1200},
         "pe": {"ltp": "150", "oi": 900}},
        {"strike": "24350", "ce": {"ltp": "150", "oi": 300}},
    ],
    "oi": [{"strike": "24300", "ce_oi": 1200, "pe_oi": 900}, {"strike": "24350", "ce_oi": 300},
           {"strike": "26000", "ce_oi": 3000, "pe_oi": 100}],
}  # fmt: skip


def test_health(client: TestClient) -> None:
    assert client.get("/health").json()["status"] == "ok"


def test_optionchain_round_trip_uses_configured_rate(client: TestClient) -> None:
    r = client.post("/internal/optionchain", json=BODY)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["rate"] == 0.06
    assert body["rate_as_of"] == "2026-10-07"
    assert body["model"] == "black76"
    assert body["forward_source"] == "future"
    assert body["atm_strike"] == "24300"  # decimals travel as strings
    assert body["pcr"] == pytest.approx(1000 / 4500)  # full expiry, incl. 26000 outside the window
    assert body["oi_strikes"] == 3
    assert body["rows"][0]["ce"]["iv"] > 0
    assert body["rows"][0]["ce"]["iv_source"] == "mid"  # 180/181 is well inside the configured 50 %
    assert body["rows"][0]["pe"]["iv"] is None  # LTP never seen trading: no IV
    assert body["rows"][1]["pe"] is None


def test_optionchain_rejects_naive_time_and_bad_strikes(client: TestClient) -> None:
    assert client.post("/internal/optionchain", json=BODY | {"now": "2026-10-27T09:30:00"}).status_code == 422
    bad = BODY | {"strikes": [{"strike": "-1"}]}
    assert client.post("/internal/optionchain", json=bad).status_code == 422


def test_chain_settings_are_validated() -> None:
    s = config.load({"VYUHA_ENV": "dev"})
    assert (s.risk_free_rate, s.risk_free_rate_as_of) == (
        config.DEFAULT_RISK_FREE_RATE,
        config.DEFAULT_RISK_FREE_RATE_AS_OF,
    )
    assert (s.iv_mid_max_spread, s.iv_ltp_max_age_s) == (0.10, 60.0)
    for env, match in [
        (
            {"RISK_FREE_RATE": "5.5", "RISK_FREE_RATE_AS_OF": "2026-10-07"},
            "RISK_FREE_RATE",
        ),  # percent, not decimal
        ({"RISK_FREE_RATE": "0.06"}, "RISK_FREE_RATE_AS_OF"),  # a new rate needs its date
        ({"IV_MID_MAX_SPREAD": "10"}, "IV_MID_MAX_SPREAD"),
        ({"IV_LTP_MAX_AGE_S": "soon"}, "IV_LTP_MAX_AGE_S"),
    ]:
        with pytest.raises(ValueError, match=match):
            config.load({"VYUHA_ENV": "dev", **env})
