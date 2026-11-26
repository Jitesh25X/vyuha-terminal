from __future__ import annotations

import json
import logging
from datetime import date
from decimal import Decimal
from typing import Any

import pytest

from vyuha_quant.data import banlist
from vyuha_quant.data.instruments import freeze, kite, merge, sync, upstox
from vyuha_quant.data.instruments.model import SourceRow


def _by_ts(rows: list[SourceRow]) -> dict[str, SourceRow]:
    return {r.tradingsymbol: r for r in rows}


def test_kite_parse(day1: sync.Files) -> None:
    rows = _by_ts([r for ex in kite.EXCHANGES for r in kite.parse(day1.kite[ex])])
    idx = rows["NIFTY 50"]
    assert (idx.segment, idx.lot_size, idx.tick_size) == ("IDX", 1, Decimal("0.05"))  # zeros in the dump
    assert rows["GOLDSTAR-ST"].symbol == "GOLDSTAR"
    ce = rows["NIFTY26NOV24000CE"]
    assert (ce.exchange, ce.segment, ce.symbol, ce.option_type) == ("NFO", "OPT", "NIFTY", "CE")
    assert (ce.expiry, ce.strike, ce.lot_size) == (date(2026, 11, 24), Decimal(24000), 65)
    assert rows["NIFTY26NOVFUT"].strike is None


def test_kite_rejects_missing_columns() -> None:
    with pytest.raises(kite.KiteFormatError, match="missing columns"):
        kite.parse("instrument_token,tradingsymbol\n1,X\n")


def test_upstox_parse_units(day1: sync.Files) -> None:
    rows = _by_ts([r for f in upstox.FILES for r in upstox.parse(day1.upstox[f])])
    assert "GOLD FUT 24 NOV 26" not in rows  # commodity segment skipped
    nse_rel = next(r for r in upstox.parse(day1.upstox["NSE"]) if r.tradingsymbol == "RELIANCE")
    assert nse_rel.tick_size == Decimal("0.1")  # 10 paise → ₹0.10
    assert rows["RELIANCE"].exchange == "BSE"
    assert rows["RELIANCE"].tick_size == Decimal("0.05")
    ce = rows["NIFTY 24000 CE 24 NOV 26"]
    assert (ce.exchange, ce.expiry, ce.strike, ce.tick_size) == (
        "NFO",
        date(2026, 11, 24),
        Decimal(24000),
        Decimal("0.05"),
    )
    assert ce.freeze_qty == 1800


def test_upstox_expiry_is_ist_date() -> None:
    # 2026-11-24 23:59:59 IST == 18:29:59 UTC; a UTC reading would still be the 24th, but
    # 2026-11-25 00:30 IST is 2026-11-24 19:00 UTC and must map to the 25th.
    assert upstox.expiry_date(1795544999000) == date(2026, 11, 24)
    assert upstox.expiry_date(1795546800000) == date(2026, 11, 25)


def test_upstox_rejects_derivative_without_expiry() -> None:
    item = {"segment": "NSE_FO", "instrument_type": "FUT", "instrument_key": "NSE_FO|1", "exchange_token": "1",
            "trading_symbol": "X FUT", "lot_size": 1, "tick_size": 5.0}  # fmt: skip
    with pytest.raises(upstox.UpstoxFormatError, match="without expiry"):
        upstox.parse([item])


def _future(**extra: Any) -> dict[str, Any]:
    return {"segment": "NSE_FO", "instrument_type": "FUT", "instrument_key": "NSE_FO|1", "exchange_token": "1",
            "trading_symbol": "X FUT", "lot_size": 1, "tick_size": 5.0, "expiry": 1795544999000, **extra}  # fmt: skip


@pytest.mark.parametrize(
    ("extra", "want", "invalid"),
    [
        ({"freeze_quantity": 1800.0}, 1800, False),
        ({"freeze_quantity": 0.0}, None, False),
        ({"freeze_quantity": None}, None, False),
        ({}, None, False),
        ({"freeze_quantity": -1800.0}, None, True),
        ({"freeze_quantity": 1800.5}, None, True),
        ({"freeze_quantity": 0.5}, None, True),
        ({"freeze_quantity": float("nan")}, None, True),
        ({"freeze_quantity": float("inf")}, None, True),
        ({"freeze_quantity": float("-inf")}, None, True),
        ({"freeze_quantity": 2147483647.0}, 2147483647, False),  # the column is a 32-bit int
        ({"freeze_quantity": 2147483648.0}, None, True),
        ({"freeze_quantity": 1e30}, None, True),
    ],
)
def test_upstox_freeze_quantity(extra: dict[str, Any], want: int | None, invalid: bool) -> None:
    [row] = upstox.parse([_future(**extra)])
    assert (row.freeze_qty, row.freeze_qty_invalid) == (want, invalid)


def _freeze_line(files: sync.Files, caplog: pytest.LogCaptureFixture) -> str:
    k, u = sync.parse(files)
    with caplog.at_level(logging.INFO, logger=sync.__name__):
        nothing = freeze.Status(date(2026, 10, 28), [], [], [], [], [])  # database counts: see integration
        sync.log_freeze_source(u, merge.merge(k, u).instruments, nothing)
    [record] = caplog.records
    return record.getMessage()


def test_freeze_source_log(day1: sync.Files, caplog: pytest.LogCaptureFixture) -> None:
    # The one F&O instrument without a value is the Kite-only NIFTY26NOV24100PE.
    assert _freeze_line(day1, caplog) == (
        "freeze quantity source upstox.freeze_quantity: 8 instruments have a value in the file, "
        "1 F&O instruments have none in the file, 0 invalid values ignored; "
        "F&O contracts now: 0 carried forward, 0 inherited, 0 unknown"
    )


def test_freeze_source_log_counts_missing_and_invalid(
    day1: sync.Files, caplog: pytest.LogCaptureFixture
) -> None:
    rows = day1.upstox["NSE"]
    for i, r in enumerate(rows):
        if r["instrument_key"] == "NSE_FO|61471":
            rows[i] = {key: v for key, v in r.items() if key != "freeze_quantity"}
        elif r["instrument_key"] == "NSE_FO|50001":
            rows[i] = {**r, "freeze_quantity": -1.0}
    assert _freeze_line(day1, caplog) == (
        "freeze quantity source upstox.freeze_quantity: 6 instruments have a value in the file, "
        "3 F&O instruments have none in the file, 1 invalid values ignored; "
        "F&O contracts now: 0 carried forward, 0 inherited, 0 unknown"
    )


@pytest.mark.parametrize("bad", ["NaN", "Infinity", "-Infinity", "2147483648"])
def test_freeze_source_log_counts_unusable_numbers(
    day1: sync.Files, caplog: pytest.LogCaptureFixture, bad: str
) -> None:
    rows = day1.upstox["NSE"]
    for i, r in enumerate(rows):
        if r["instrument_key"] == "NSE_FO|50001":
            rows[i] = {**r, "freeze_quantity": json.loads(bad)}  # as the file parser would read it
    assert "1 invalid values ignored" in _freeze_line(day1, caplog)


def test_merge(day1: sync.Files) -> None:
    k, u = sync.parse(day1)
    m = merge.merge(k, u)
    by = {(i.exchange, i.tradingsymbol): i for i in m.instruments}
    assert m.conflicts == []
    both = by[("NFO", "NIFTY26NOV24000CE")]
    assert (both.kite_token, both.upstox_key, both.partial_mapping) == (13050370, "NSE_FO|50001", False)
    kite_only = by[("NFO", "NIFTY26NOV24100PE")]
    assert (kite_only.upstox_key, kite_only.partial_mapping) == (None, True)
    up_only = by[("BFO", "SENSEX 80100 CE 24 NOV 26")]
    assert (up_only.kite_token, up_only.partial_mapping) == (None, True)
    # underlyings: index via upstox key (and borrowed for the kite-only contract), stock via ISIN key
    assert both.underlying == ("NSE", "NIFTY 50")
    assert kite_only.underlying == ("NSE", "NIFTY 50")
    assert by[("NFO", "RELIANCE26NOVFUT")].underlying == ("NSE", "RELIANCE")
    assert up_only.underlying == ("BSE", "SENSEX")
    assert by[("NSE", "NIFTY 50")].upstox_key == "NSE_INDEX|Nifty 50"
    gs = by[("NSE", "GOLDSTAR-ST")]
    assert (gs.symbol, gs.isin, gs.partial_mapping) == ("GOLDSTAR", "INE0GS101011", False)
    assert by[("NSE", "KITEONLY")].partial_mapping
    assert (m.matched, m.kite_only, m.upstox_only) == (9, 2, 1)


def test_merge_reports_term_conflicts(day1: sync.Files) -> None:
    k, u = sync.parse(day1)
    u = [r if r.broker_id != "NSE_FO|61471" else _replace_lot(r, 75) for r in u]
    m = merge.merge(k, u)
    assert len(m.conflicts) == 1
    assert "NIFTY26NOVFUT" in m.conflicts[0]


def _replace_lot(r: SourceRow, lot: int) -> SourceRow:
    from dataclasses import replace

    return replace(r, lot_size=lot)


@pytest.mark.parametrize(
    ("content", "want"),
    [
        (b"Securities in Ban For Trade Date 28-OCT-2026:\n1,RELIANCE\n2,M&M\n", ["M&M", "RELIANCE"]),
        (b"Securities in Ban For Trade Date 28-OCT-2026:\r\n1,abc\r\n", ["ABC"]),
        (b"Securities in Ban For Trade Date 28-OCT-2026: NIL\n", []),
        (b"Securities in Ban For Trade Date 28-OCT-2026:\nNIL\n", []),
        (b"\xef\xbb\xbfSecurities in Ban For Trade Date 28-Oct-2026:\n", []),
    ],
)
def test_banlist_parse(content: bytes, want: list[str]) -> None:
    assert banlist.parse(content) == (date(2026, 10, 28), want)


@pytest.mark.parametrize(
    ("content", "err"),
    [
        (b"", "empty"),
        (b"1,RELIANCE\n", "trade date"),
        (b"Securities in Ban For Trade Date 28-OCT-2026:\nRELIANCE;DROP\n", "unexpected line"),
    ],
)
def test_banlist_parse_rejects(content: bytes, err: str) -> None:
    with pytest.raises(banlist.BanListError, match=err):
        banlist.parse(content)
