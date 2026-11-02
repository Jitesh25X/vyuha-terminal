"""F-RS-1 unit tests: planning, validation, the pace, the lake."""

from __future__ import annotations

import asyncio
from datetime import UTC, date, datetime, time
from decimal import Decimal
from pathlib import Path

import httpx
import polars as pl
import pytest
from aiolimiter import AsyncLimiter

from vyuha_quant import config
from vyuha_quant.data import download as dl
from vyuha_quant.data import lake, upstox_history
from vyuha_quant.data.limits import UPSTOX_INTERVALS, UPSTOX_RATE, pace_seconds


def d(s: str) -> date:
    return date.fromisoformat(s)


def test_pace_stays_under_every_window_with_headroom() -> None:
    pace = pace_seconds(UPSTOX_RATE)
    assert pace == pytest.approx(1.125)
    assert 1 / pace <= UPSTOX_RATE.per_second * 0.8
    assert 60 / pace <= UPSTOX_RATE.per_minute * 0.8
    assert 1800 / pace <= UPSTOX_RATE.per_30_minutes * 0.8


@pytest.mark.parametrize(
    ("want", "covered", "expected"),
    [
        (("2022-01-01", "2022-03-31"), [], [("2022-01-01", "2022-03-31")]),
        (("2022-01-01", "2022-03-31"), [("2022-01-01", "2022-03-31")], []),
        (("2022-01-01", "2022-03-31"), [("2021-01-01", "2023-01-01")], []),
        (
            ("2022-01-01", "2022-03-31"),
            [("2022-02-01", "2022-02-28")],
            [("2022-01-01", "2022-01-31"), ("2022-03-01", "2022-03-31")],
        ),
        (
            ("2022-01-01", "2022-03-31"),
            [("2022-01-01", "2022-01-31"), ("2022-01-15", "2022-02-10")],
            [("2022-02-11", "2022-03-31")],
        ),
        (
            ("2022-02-01", "2022-02-28"),
            [("2021-01-01", "2021-12-31"), ("2023-01-01", "2023-02-01")],
            [("2022-02-01", "2022-02-28")],
        ),
    ],
)
def test_missing_ranges(
    want: tuple[str, str], covered: list[tuple[str, str]], expected: list[tuple[str, str]]
) -> None:
    conv = lambda r: (d(r[0]), d(r[1]))  # noqa: E731
    assert dl.missing_ranges(conv(want), [conv(c) for c in covered]) == [conv(e) for e in expected]


def test_split_respects_the_broker_range_caps() -> None:
    minute = dl.split_range((d("2022-01-15"), d("2022-03-02")), UPSTOX_INTERVALS["1minute"].max_months)
    assert minute == [
        (d("2022-01-15"), d("2022-01-31")),
        (d("2022-02-01"), d("2022-02-28")),
        (d("2022-03-01"), d("2022-03-02")),
    ]
    day = dl.split_range((d("2015-01-01"), d("2026-11-01")), UPSTOX_INTERVALS["day"].max_months)
    assert day == [(d("2015-01-01"), d("2024-12-31")), (d("2025-01-01"), d("2026-11-01"))]


def test_first_job_request_count() -> None:
    """Nifty 50 + 2 indices: the numbers quoted in docs/RUN_LOCAL.md."""
    months = dl.split_range((d("2022-01-01"), d("2026-11-01")), 1)
    assert len(months) == 59
    assert len(dl.split_range((d("2015-01-01"), d("2026-11-01")), 120)) == 2


def candle(
    ts: str,
    o: object = 100,
    h: object = 101,
    lo: object = 99,
    c: object = 100.5,
    v: object = 10,
    oi: object = 0,
) -> list[object]:
    def dec(x: object) -> object:
        return Decimal(str(x)) if isinstance(x, float) else x

    return [ts, dec(o), dec(h), dec(lo), dec(c), v, oi]


def test_normalise_prices_are_exact_and_sorted_utc() -> None:
    dropped = dl.Dropped()
    bars = dl.normalise(
        [
            candle("2024-01-02T09:16:00+05:30", o=1234.05, h=1300),
            candle("2024-01-02T09:15:00+05:30", o=100.1),
        ],
        "1minute",
        {},
        dropped,
    )
    assert bars["ts"].to_list() == [
        datetime(2024, 1, 2, 3, 45, tzinfo=UTC),
        datetime(2024, 1, 2, 3, 46, tzinfo=UTC),
    ]
    assert bars["open"].to_list() == [1_001_000, 12_340_500]
    assert bars.schema["open"] == pl.Int64
    assert dropped == dl.Dropped()


def test_normalise_drops_and_counts_bad_rows() -> None:
    dropped = dl.Dropped()
    bars = dl.normalise(
        [
            candle("2024-01-02T09:15:00+05:30"),
            candle("2024-01-02T09:15:00+05:30"),  # duplicate
            candle("2024-01-02T09:14:00+05:30"),  # before the open
            candle("2024-01-02T15:30:00+05:30"),  # the close is exclusive
            candle("2024-01-02T09:16:00+05:30", h=99.5),  # high below the close
            candle("2024-01-02T09:17:00+05:30", lo=0),
            ["2024-01-02T09:18:00+05:30", 1],  # truncated
            candle("not a time"),
            candle("2024-01-02T09:19:00"),  # no zone
        ],
        "1minute",
        {},
        dropped,
    )
    assert bars.height == 1
    assert dropped == dl.Dropped(outside_session=2, bad_ohlc=2, duplicates=1, malformed=3)


def test_session_uses_holidays_and_special_sessions() -> None:
    muhurat = {d("2024-11-01"): [(time(18, 0), time(19, 0))], d("2024-11-15"): []}
    dropped = dl.Dropped()
    bars = dl.normalise(
        [
            candle("2024-11-01T18:30:00+05:30"),
            candle("2024-11-01T10:00:00+05:30"),
            candle("2024-11-15T10:00:00+05:30"),
        ],
        "1minute",
        muhurat,
        dropped,
    )
    assert bars.height == 1
    assert dropped.outside_session == 2


def test_daily_candles_at_midnight_ist_are_kept_in_the_right_year(tmp_path: Path) -> None:
    bars = dl.normalise([candle("2024-01-01T00:00:00+05:30")], "day", {}, dl.Dropped())
    writes = lake.write_bars(tmp_path, "upstox", "day", 7, bars)
    assert [w.path for w in writes] == [
        "bars/source=upstox/interval=day/instrument_id=7/year=2024/part-0.parquet"
    ]
    assert writes[0].to_ts == datetime(2023, 12, 31, 18, 30, tzinfo=UTC)


def test_rewrite_keeps_stored_rows_and_skips_a_file_with_nothing_new(tmp_path: Path) -> None:
    first = dl.normalise([candle("2024-01-02T09:15:00+05:30", c=100.5)], "1minute", {}, dl.Dropped())
    w1 = lake.write_bars(tmp_path, "upstox", "1minute", 7, first)
    assert w1[0].new_rows == 1
    path = tmp_path / w1[0].path
    stamp = path.stat().st_mtime_ns

    changed = dl.normalise([candle("2024-01-02T09:15:00+05:30", c=100.75)], "1minute", {}, dl.Dropped())
    same = lake.write_bars(tmp_path, "upstox", "1minute", 7, changed)
    assert [(w.path, w.new_rows, w.row_count, w.sha256) for w in same] == [(w1[0].path, 0, 1, w1[0].sha256)]
    assert path.stat().st_mtime_ns == stamp
    assert lake.read_bars(tmp_path, "upstox", "1minute", 7)["close"].to_list() == [1_005_000]

    more = dl.normalise([candle("2024-01-02T09:16:00+05:30")], "1minute", {}, dl.Dropped())
    w2 = lake.write_bars(tmp_path, "upstox", "1minute", 7, more)
    assert (w2[0].new_rows, w2[0].row_count) == (1, 2)
    assert w2[0].sha256 != w1[0].sha256


def test_adjusted_view_is_computed_on_read_and_raw_stays(tmp_path: Path) -> None:
    """A 1:2 split with ex_date 2024-01-03, IST dates (a 00:00 IST candle is the previous UTC day)."""
    bars = dl.normalise(
        [candle("2024-01-02T00:00:00+05:30", v=100), candle("2024-01-03T00:00:00+05:30", v=100)],
        "day",
        {},
        dl.Dropped(),
    )
    w = lake.write_bars(tmp_path, "upstox", "day", 7, bars)
    before = (tmp_path / w[0].path).read_bytes()
    split = lake.CorporateAction(d("2024-01-03"), price_factor=0.5, volume_factor=2.0)
    adj = lake.read_bars(tmp_path, "upstox", "day", 7, adjusted=True, actions=[split])
    assert adj["close"].to_list() == [502_500, 1_005_000]
    assert adj["volume"].to_list() == [200, 100]
    raw = lake.read_bars(tmp_path, "upstox", "day", 7)
    assert raw["close"].to_list() == [1_005_000, 1_005_000]
    assert (tmp_path / w[0].path).read_bytes() == before


def test_constituents_csv() -> None:
    text = "﻿Company Name,Industry,Symbol,Series,ISIN Code\nA Ltd,X, AAA ,EQ,INE1\nB Ltd,Y,BBB,EQ,INE2\n"
    assert dl.read_constituents(text) == ["NSE:AAA", "NSE:BBB"]
    with pytest.raises(dl.SpecError):
        dl.read_constituents("Name\nA\n")


def test_spec_refuses_expired_contracts_and_unknown_fields() -> None:
    assert dl.JobSpec.model_validate({"interval": "day", "from": "2015-01-01"}).to is None
    with pytest.raises(ValueError, match="include_expired"):
        dl.JobSpec.model_validate({"interval": "day", "from": "2015-01-01", "include_expired": True})
    with pytest.raises(ValueError, match="universe"):
        dl.JobSpec.model_validate({"interval": "day", "from": "2015-01-01", "universe": "all"})


def test_static_ip_not_on_host_is_refused() -> None:
    upstox_history.preflight_source_ip("127.0.0.1")
    for bad in ("203.0.113.7", "0.0.0.0", "::", "224.0.0.1"):  # TEST-NET-3, wildcards, multicast
        with pytest.raises(upstox_history.StaticIPNotOnHost):
            upstox_history.preflight_source_ip(bad)


def test_requests_are_bound_to_the_account_address_and_ignore_proxies() -> None:
    """Invariant 4. Loopback tests can't tell which address was used, so check what the transport was given."""

    async def check() -> None:
        c = upstox_history.UpstoxHistory("t", "192.0.2.10", AsyncLimiter(1, 1))
        assert c._client._transport._pool._local_address == "192.0.2.10"  # type: ignore[attr-defined]
        assert c._client._mounts == {}  # trust_env=False: no environment proxy routes around the binding
        await c.aclose()

    asyncio.run(check())


def _client(handler: object, sleeps: list[float]) -> upstox_history.UpstoxHistory:
    async def sleep(s: float) -> None:
        sleeps.append(s)

    c = upstox_history.UpstoxHistory("SECRET-TOKEN", "127.0.0.1", AsyncLimiter(1000, 1), sleep=sleep)
    c._client = httpx.AsyncClient(
        base_url="http://fake",
        headers=c._client.headers,
        transport=httpx.MockTransport(handler),  # type: ignore[arg-type]
    )
    return c


LIMIT = UPSTOX_INTERVALS["1minute"]


def test_request_path_and_exact_decimal_parsing() -> None:
    seen: list[httpx.Request] = []

    def handler(r: httpx.Request) -> httpx.Response:
        seen.append(r)
        return httpx.Response(
            200,
            text='{"status":"success","data":{"candles":[["2024-01-02T09:15:00+05:30",0.1,0.3,0.1,0.2,1,0]]}}',
        )

    out = asyncio.run(
        _client(handler, []).candles("NSE_EQ|INE002A01018", LIMIT, d("2024-01-01"), d("2024-01-31"))
    )
    assert (
        seen[0].url.raw_path.decode()
        == "/v3/historical-candle/NSE_EQ%7CINE002A01018/minutes/1/2024-01-31/2024-01-01"
    )
    assert seen[0].headers["authorization"] == "Bearer SECRET-TOKEN"
    assert out[0][1] == Decimal("0.1")


def test_backs_off_on_429_exponentially_then_gives_up() -> None:
    sleeps: list[float] = []

    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(429)

    c = _client(handler, sleeps)
    with pytest.raises(upstox_history.RetriesExhausted):
        asyncio.run(c.candles("K", LIMIT, d("2024-01-01"), d("2024-01-31")))
    assert sleeps == [1, 2, 4, 8, 16]
    assert (c.requests, c.rate_limited) == (5, 5)


def test_429_honours_retry_after_and_recovers() -> None:
    sleeps: list[float] = []
    replies = [
        httpx.Response(429, headers={"retry-after": "30"}),
        httpx.Response(200, text='{"data":{"candles":[]}}'),
    ]

    def handler(_: httpx.Request) -> httpx.Response:
        return replies.pop(0)

    assert asyncio.run(_client(handler, sleeps).candles("K", LIMIT, d("2024-01-01"), d("2024-01-31"))) == []
    assert sleeps == [30]


@pytest.mark.parametrize(
    ("status", "body", "exc"),
    [
        (401, "{}", upstox_history.TokenExpired),
        (
            400,
            '{"errors":[{"errorCode":"UDAPI1148","message":"echo SECRET-TOKEN"}]}',
            upstox_history.BrokerRejected,
        ),
    ],
)
def test_errors_are_final_and_never_carry_the_token(status: int, body: str, exc: type[Exception]) -> None:
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(status, text=body)

    with pytest.raises(exc) as e:
        asyncio.run(_client(handler, []).candles("K", LIMIT, d("2024-01-01"), d("2024-01-31")))
    assert "SECRET-TOKEN" not in str(e.value)


def test_last_finished_day_is_yesterday_ist() -> None:
    # 20:00 UTC on the 1st is already 01:30 IST on the 2nd.
    assert dl.last_finished_day(datetime(2026, 11, 1, 20, 0, tzinfo=UTC)) == d("2026-11-01")
    assert config.today_ist(datetime(2026, 11, 1, 20, 0, tzinfo=UTC)) == d("2026-11-02")


def test_api_override_must_be_loopback_because_it_receives_the_token() -> None:
    assert config.load({"UPSTOX_API_BASE": "http://127.0.0.1:9"}).upstox_api_base == "http://127.0.0.1:9"
    with pytest.raises(ValueError, match="loopback"):
        config.load({"UPSTOX_API_BASE": "https://example.com"})


def test_pace_override_never_applies_to_the_real_api() -> None:
    with pytest.raises(ValueError, match="DOWNLOAD_PACE_S"):
        config.load({"DOWNLOAD_PACE_S": "0.001"})


def test_dev_overrides_are_refused_outside_dev() -> None:
    base = {"VYUHA_ENV": "staging", "POSTGRES_URL": "postgres://x"}
    with pytest.raises(ValueError, match="dev only"):
        config.load({**base, "UPSTOX_API_BASE": "http://evil"})
    with pytest.raises(ValueError, match="dev only"):
        config.load({**base, "DOWNLOAD_PACE_S": "0"})
