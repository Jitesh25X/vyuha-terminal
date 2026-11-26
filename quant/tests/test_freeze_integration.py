"""D-52: a missing freeze quantity is carried forward or inherited, never written over, and admins are told.

Calendar used below: 2026-10-28 is a Wednesday, 10-31 a Saturday, 11-02 a Monday. All contracts expire 11-24.
"""

from __future__ import annotations

import json
import logging
from datetime import date
from typing import Any

import asyncpg
import pytest
from conftest import MIGRATIONS
from test_sync_integration import DAY1, active, add_admin, q, run, sync_files, without

from vyuha_quant.data.instruments import freeze, sync

THU, FRI, MON, TUE, WED = (
    date(2026, 10, 29),
    date(2026, 10, 30),
    date(2026, 11, 2),
    date(2026, 11, 3),
    date(2026, 11, 4),
)
FUT, CE, REL_FUT = "NSE_FO|61471", "NSE_FO|50001", "NSE_FO|60001"
NIFTY_PE = ("NFO", "NIFTY26NOV24100PE")  # Kite-only in day1: never has a value of its own
NIFTY_CE = ("NFO", "NIFTY26NOV24000CE")
NIFTY_FUT = ("NFO", "NIFTY26NOVFUT")
CARRIED, INHERITED, UNKNOWN = (
    "Freeze quantity carried forward",
    "Freeze quantity inherited",
    "Freeze quantity unknown",
)
GONE = object()  # remove the field from the upstox row


def with_freeze(files: sync.Files, values: dict[str, Any]) -> sync.Files:
    """Day files with `freeze_quantity` replaced (or removed with GONE) by upstox instrument key."""

    def row(r: dict[str, Any]) -> dict[str, Any]:
        if r["instrument_key"] not in values:
            return r
        out = {k: v for k, v in r.items() if k != "freeze_quantity"}
        value = values[r["instrument_key"]]
        return out if value is GONE else {**out, "freeze_quantity": value}

    return sync.Files(kite=files.kite, upstox={f: [row(r) for r in rows] for f, rows in files.upstox.items()})


def no_freeze_at_all(files: sync.Files) -> sync.Files:
    keys = {r["instrument_key"] for rows in files.upstox.values() for r in rows}
    return with_freeze(files, dict.fromkeys(keys, GONE))


def alerts(dsn: str, title: str) -> list[dict[str, Any]]:
    rows = q(dsn, "SELECT severity, body::text AS body FROM alert_event WHERE title = $1 ORDER BY id", title)
    assert all(r["severity"] == "warn" for r in rows)
    return [json.loads(r["body"]) for r in rows]


def freeze_of(dsn: str, key: tuple[str, str]) -> tuple[int | None, date | None, int | None]:
    r = active(dsn)[key]
    return r["freeze_qty"], r["freeze_qty_as_of"], r["freeze_qty_from_id"]


def test_own_value_is_stored_with_the_file_date(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    assert freeze_of(dsn, NIFTY_CE) == (1800, DAY1, None)
    assert freeze_of(dsn, ("NSE", "RELIANCE")) == (100000, DAY1, None)
    report = sync_files(dsn, day1, THU)
    assert freeze_of(dsn, NIFTY_CE) == (1800, THU, None)
    assert report["updated"] == 0  # a newer file date alone is not a changed instrument


@pytest.mark.parametrize(
    "bad", [GONE, None, 0.0, -1.0, 1800.5, float("nan"), float("inf"), float("-inf"), 2147483648.0]
)
def test_absent_or_invalid_never_overwrites(dsn: str, day1: sync.Files, bad: Any) -> None:
    sync_files(dsn, day1, DAY1)
    sync_files(dsn, with_freeze(day1, {CE: bad}), THU)
    assert freeze_of(dsn, NIFTY_CE) == (1800, DAY1, None)


def test_carried_value_alerts_at_two_trading_days(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, day1, DAY1)
    gone = with_freeze(day1, {CE: GONE, FUT: -1.0})
    sync_files(dsn, gone, THU)  # one trading day after the value's date
    assert alerts(dsn, CARRIED) == []
    sync_files(dsn, gone, FRI)
    assert freeze_of(dsn, NIFTY_CE) == (1800, DAY1, None)
    [body] = alerts(dsn, CARRIED)  # one alert for both contracts
    assert (body["file_date"], body["count"], body["underlyings"]) == (FRI.isoformat(), 2, ["NIFTY"])
    assert body["contracts"][0] == {
        "exchange": "NFO",
        "symbol": "NIFTY26NOV24000CE",
        "value": 1800,
        "as_of": DAY1.isoformat(),
        "donor": None,
    }
    assert "NSE circular" in body["action"]
    # The file has a value again: it replaces the carried one.
    sync_files(dsn, with_freeze(day1, {CE: 1200.0}), MON)
    assert freeze_of(dsn, NIFTY_CE) == (1200, MON, None)


def test_weekend_and_holiday_are_not_trading_days(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    q(dsn, "INSERT INTO exchange_holiday (exchange, day) VALUES ('NFO', $1)", TUE)
    sync_files(dsn, day1, FRI)
    gone = with_freeze(day1, {CE: GONE})
    sync_files(dsn, gone, MON)  # Saturday and Sunday do not count
    sync_files(dsn, gone, TUE)  # nor does the holiday
    assert alerts(dsn, CARRIED) == []
    sync_files(dsn, gone, WED)
    assert [b["count"] for b in alerts(dsn, CARRIED)] == [1]


def test_carried_value_of_unknown_date_alerts_at_once(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, day1, DAY1)
    q(dsn, "UPDATE instrument SET freeze_qty_as_of = NULL")  # rows from before migration 000015
    sync_files(dsn, with_freeze(day1, {CE: GONE}), THU)
    assert freeze_of(dsn, NIFTY_CE) == (1800, None, None)
    [body] = alerts(dsn, CARRIED)
    assert [(c["symbol"], c["as_of"]) for c in body["contracts"]] == [("NIFTY26NOV24000CE", None)]


def test_inherits_the_smallest_own_value_and_records_the_donor(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, with_freeze(day1, {CE: 1200.0}), DAY1)
    ce_id = active(dsn)[NIFTY_CE]["id"]
    assert freeze_of(dsn, NIFTY_PE) == (1200, DAY1, ce_id)
    [body] = alerts(dsn, INHERITED)
    assert (body["count"], body["underlyings"]) == (1, ["NIFTY"])
    assert body["contracts"] == [
        {
            "exchange": "NFO",
            "symbol": "NIFTY26NOV24100PE",
            "value": 1200,
            "as_of": DAY1.isoformat(),
            "donor": "NIFTY26NOV24000CE",
        }
    ]
    # Re-derived on the next sync, from whichever donor is now smallest.
    sync_files(dsn, with_freeze(day1, {CE: 2400.0, FUT: 900.0}), THU)
    assert freeze_of(dsn, NIFTY_PE) == (900, THU, active(dsn)[NIFTY_FUT]["id"])
    assert len(alerts(dsn, INHERITED)) == 2  # every inherited case, every sync


def test_donor_date_travels_with_a_carried_donor(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, with_freeze(day1, {CE: 1200.0}), DAY1)
    sync_files(dsn, with_freeze(day1, {CE: GONE}), THU)
    assert freeze_of(dsn, NIFTY_PE) == (1200, DAY1, active(dsn)[NIFTY_CE]["id"])


def test_own_value_replaces_an_inherited_one(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    assert freeze_of(dsn, NIFTY_PE)[0] == 1800
    pe = next(r for r in day1.upstox["NSE"] if r["instrument_key"] == CE)
    pe = {**pe, "instrument_key": "NSE_FO|50002", "exchange_token": "50002", "instrument_type": "PE",
          "trading_symbol": "NIFTY 24100 PE 24 NOV 26", "strike_price": 24100.0, "freeze_quantity": 600.0}  # fmt: skip
    files = sync.Files(kite=day1.kite, upstox={**day1.upstox, "NSE": [*day1.upstox["NSE"], pe]})
    sync_files(dsn, files, THU)
    assert freeze_of(dsn, NIFTY_PE) == (600, THU, None)


def test_never_inherits_from_an_inherited_value(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    donor_value = freeze_of(dsn, NIFTY_PE)
    # Both donors delist; a new NIFTY contract appears with no value of its own.
    files = without(day1, {"NIFTY26NOVFUT", "NIFTY26NOV24000CE"}, {FUT, CE})
    files.kite["NFO"] += '1,59999,NIFTY26NOV24200CE,"NIFTY",0,2026-11-24,24200,0.05,65,CE,NFO-OPT,NFO\n'
    sync_files(dsn, files, THU)
    assert freeze_of(dsn, NIFTY_PE) == donor_value  # no donor left: the inherited value is kept
    assert freeze_of(dsn, ("NFO", "NIFTY26NOV24200CE")) == (None, None, None)


def test_unknown_stays_null_and_is_alerted_once(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    files = no_freeze_at_all(day1)
    files.kite["NFO"] += "".join(
        f'{900 + n},{70000 + n},NIFTY26NOV{25000 + n}CE,"NIFTY",0,2026-11-24,{25000 + n},0.05,65,CE,NFO-OPT,NFO\n'
        for n in range(60)
    )
    sync_files(dsn, files, DAY1)
    assert all(r["freeze_qty"] is None for r in active(dsn).values())
    [body] = alerts(dsn, UNKNOWN)
    assert body["count"] == 66  # 6 F&O contracts of day1 plus 60; no equity or index
    assert body["underlyings"] == ["NIFTY", "RELIANCE", "SENSEX"]
    assert len(body["contracts"]) == 50
    assert {c["exchange"] for c in body["contracts"]} <= {"NFO", "BFO"}
    assert body["contracts"][0]["value"] is None
    assert alerts(dsn, CARRIED) == alerts(dsn, INHERITED) == []


def test_equities_are_not_inherited_or_alerted(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, with_freeze(day1, {"NSE_EQ|INE0GS101011": GONE}), DAY1)
    assert freeze_of(dsn, ("NSE", "GOLDSTAR-ST")) == (None, None, None)
    assert alerts(dsn, UNKNOWN) == []
    sync_files(dsn, with_freeze(day1, {"NSE_EQ|INE002A01018": GONE}), FRI)
    assert freeze_of(dsn, ("NSE", "RELIANCE")) == (100000, DAY1, None)  # carried, but not an alert case
    assert alerts(dsn, CARRIED) == []


def test_expired_contracts_are_not_alerted(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, no_freeze_at_all(day1), date(2026, 11, 25))
    assert q(dsn, "SELECT 1 FROM alert_event") == []


def test_expiry_day_is_still_alerted(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, no_freeze_at_all(day1), date(2026, 11, 24))
    assert [b["count"] for b in alerts(dsn, UNKNOWN)] == [6]


def test_lot_size_change_and_relisting_keep_the_value(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    old_id = active(dsn)[NIFTY_CE]["id"]
    # Lot size is updated in place by the store.
    changed = with_freeze(day1, {CE: GONE})
    changed.kite["NFO"] = changed.kite["NFO"].replace(",24000,0.05,65,", ",24000,0.05,75,")
    changed.upstox["NSE"][:] = [
        {**r, "lot_size": 75} if r["instrument_key"] == CE else r for r in changed.upstox["NSE"]
    ]
    sync_files(dsn, changed, THU)
    assert (active(dsn)[NIFTY_CE]["id"], active(dsn)[NIFTY_CE]["lot_size"]) == (old_id, 75)
    assert freeze_of(dsn, NIFTY_CE) == (1800, DAY1, None)
    # Delisted, then listed again: a new version row, which takes the last known value and its date.
    sync_files(dsn, without(day1, {"NIFTY26NOV24000CE"}, {CE}), FRI)
    sync_files(dsn, changed, MON)
    assert active(dsn)[NIFTY_CE]["id"] != old_id
    assert freeze_of(dsn, NIFTY_CE) == (1800, DAY1, None)


def test_a_reused_symbol_does_not_take_the_old_contracts_value(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    # Same trading symbol, another strike: a different contract to the store. With no NIFTY donor it is unknown.
    files = no_freeze_at_all(day1)
    files.kite["NFO"] = files.kite["NFO"].replace(",24000,0.05,", ",24050,0.05,")
    files.upstox["NSE"][:] = [
        {**r, "strike_price": 24050.0} if r["instrument_key"] == CE else r for r in files.upstox["NSE"]
    ]
    q(dsn, "UPDATE instrument SET freeze_qty = NULL, freeze_qty_as_of = NULL, freeze_qty_from_id = NULL WHERE tradingsymbol <> $1", NIFTY_CE[1])  # fmt: skip
    sync_files(dsn, files, THU)
    assert freeze_of(dsn, NIFTY_CE) == (None, None, None)


def test_sync_log_counts_carried_inherited_and_unknown(
    dsn: str, day1: sync.Files, caplog: pytest.LogCaptureFixture
) -> None:
    sync_files(dsn, day1, DAY1)
    files = with_freeze(day1, {CE: GONE, REL_FUT: GONE})
    files.kite["NFO"] += '1,59999,BANKNIFTY26NOVFUT,"BANKNIFTY",0,2026-11-24,0,0.05,30,FUT,NFO-FUT,NFO\n'
    with caplog.at_level(logging.INFO, logger=sync.__name__):
        sync_files(dsn, files, THU)
    [line] = [r.getMessage() for r in caplog.records if "freeze quantity source" in r.getMessage()]
    assert line.endswith("F&O contracts now: 2 carried forward, 1 inherited, 1 unknown")


def test_a_failed_alert_does_not_fail_the_sync(
    dsn: str, day1: sync.Files, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    add_admin(dsn)

    async def boom(*_: Any) -> int:
        raise RuntimeError("alert table gone")

    monkeypatch.setattr(freeze, "alert_admins", boom)
    report = sync_files(dsn, day1, DAY1)
    assert report["inserted"] == 12
    assert len(active(dsn)) == 12
    assert f"could not raise the admin alert: {INHERITED}" in caplog.text


def test_provenance_needs_a_value(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    with pytest.raises(asyncpg.CheckViolationError):
        q(dsn, "UPDATE instrument SET freeze_qty = NULL WHERE freeze_qty_as_of IS NOT NULL")
    with pytest.raises(asyncpg.CheckViolationError):
        q(dsn, "UPDATE instrument SET freeze_qty = NULL, freeze_qty_as_of = NULL, freeze_qty_from_id = id")


def test_migration_000015_up_down_up(dsn: str) -> None:
    """The fixture database is already fully migrated: take 000015 down, up, down and up again."""
    columns = (
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = 'instrument' AND column_name LIKE 'freeze_qty%' ORDER BY 1"
    )

    async def apply(direction: str) -> None:
        conn = await asyncpg.connect(dsn)
        try:
            await conn.execute(
                (MIGRATIONS / f"000015_instrument_freeze_provenance.{direction}.sql").read_text()
            )
        finally:
            await conn.close()

    full = ["freeze_qty", "freeze_qty_as_of", "freeze_qty_from_id"]
    for direction, want in [("down", full[:1]), ("up", full), ("down", full[:1]), ("up", full)]:
        run(apply(direction))
        assert [r["column_name"] for r in q(dsn, columns)] == want


def test_migration_000015_clears_non_positive_values(dsn: str) -> None:
    """Rows written before the parser fix can hold 0 or a negative value: the migration sets them to NULL."""

    async def apply(direction: str) -> None:
        conn = await asyncpg.connect(dsn)
        try:
            await conn.execute(
                (MIGRATIONS / f"000015_instrument_freeze_provenance.{direction}.sql").read_text()
            )
        finally:
            await conn.close()

    insert = (
        "INSERT INTO instrument (exchange, segment, symbol, tradingsymbol, tick_size, freeze_qty, valid_from) "
        "VALUES ('NSE', 'EQ', $1, $1, 0.05, $2, '2026-10-28')"
    )
    values = "SELECT tradingsymbol, freeze_qty FROM instrument ORDER BY tradingsymbol"
    run(apply("down"))
    for symbol, qty in [("NEG", -1), ("ZERO", 0), ("GOOD", 1800)]:
        q(dsn, insert, symbol, qty)
    for round_ in range(2):  # up, down, up
        run(apply("up"))
        assert [tuple(r) for r in q(dsn, values)][:3] == [("GOOD", 1800), ("NEG", None), ("ZERO", None)]
        for bad in (0, -1):
            with pytest.raises(asyncpg.CheckViolationError, match="instrument_freeze_qty_positive"):
                q(dsn, insert, f"BAD{bad}", bad)
        if round_ == 0:
            run(apply("down"))
            q(dsn, insert, "ZZ_AFTER_DOWN", 0)  # the constraint is gone
