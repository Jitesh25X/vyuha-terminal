"""F-MD-1 acceptance tests against a fresh Postgres (see conftest.dsn)."""

from __future__ import annotations

import asyncio
import json
from dataclasses import replace
from datetime import date
from decimal import Decimal
from typing import Any

import asyncpg
import pytest

from vyuha_quant.data import banlist, holidays
from vyuha_quant.data.instruments import sync

DAY1 = date(2026, 10, 28)  # Wednesday
DAY2 = date(2026, 10, 29)
BAN = b"Securities in Ban For Trade Date 28-OCT-2026:\n1,RELIANCE\n"


def run(coro: Any) -> Any:
    return asyncio.run(coro)


async def _q(dsn: str, sql: str, *args: Any) -> list[asyncpg.Record]:
    conn = await asyncpg.connect(dsn)
    try:
        return list(await conn.fetch(sql, *args))
    finally:
        await conn.close()


def q(dsn: str, sql: str, *args: Any) -> list[asyncpg.Record]:
    return run(_q(dsn, sql, *args))


def active(dsn: str) -> dict[tuple[str, str], asyncpg.Record]:
    rows = q(dsn, "SELECT * FROM instrument WHERE valid_to IS NULL")
    return {(r["exchange"], r["tradingsymbol"]): r for r in rows}


def add_admin(dsn: str) -> None:
    q(
        dsn,
        "INSERT INTO app_user (username, display_name, role, password_hash) VALUES ('ops', 'Ops', 'admin', 'x')",
    )


def sync_files(dsn: str, files: sync.Files, day: date, **kw: Any) -> dict[str, Any]:
    async def fetch() -> sync.Files:
        return files

    result: dict[str, Any] = run(sync.run(dsn, day, fetch, **kw))
    return result


def without(files: sync.Files, kite_ts: set[str], upstox_keys: set[str]) -> sync.Files:
    kite_files = {
        ex: "\n".join(
            ln for ln in text.splitlines() if ln.split(",")[2:3] != [] and ln.split(",")[2] not in kite_ts
        )
        + "\n"
        for ex, text in files.kite.items()
    }
    up = {f: [r for r in rows if r["instrument_key"] not in upstox_keys] for f, rows in files.upstox.items()}
    return sync.Files(kite=kite_files, upstox=up)


def test_first_sync_maps_contracts_from_both_brokers(dsn: str, day1: sync.Files) -> None:
    report = sync_files(dsn, day1, DAY1)
    assert report["inserted"] == 12
    assert report["conflicts"] == 0
    assert report["partial_mapping"] == 3
    m = active(dsn)
    ce = m[("NFO", "NIFTY26NOV24000CE")]
    # AC1: a contract in both files → one row with both mappings
    assert (ce["kite_token"], ce["upstox_key"], ce["partial_mapping"]) == (13050370, "NSE_FO|50001", False)
    assert (ce["segment"], ce["option_type"], ce["strike"], ce["expiry"]) == (
        "OPT",
        "CE",
        Decimal(24000),
        date(2026, 11, 24),
    )
    assert (ce["lot_size"], ce["tick_size"], ce["freeze_qty"], ce["valid_from"]) == (
        65,
        Decimal("0.05"),
        1800,
        DAY1,
    )
    assert ce["underlying_id"] == m[("NSE", "NIFTY 50")]["id"]
    assert m[("NFO", "RELIANCE26NOVFUT")]["underlying_id"] == m[("NSE", "RELIANCE")]["id"]
    # AC2: present in one broker only → other mapping NULL, partial_mapping = true
    pe = m[("NFO", "NIFTY26NOV24100PE")]
    assert (pe["upstox_key"], pe["partial_mapping"]) == (None, True)
    up_only = m[("BFO", "SENSEX 80100 CE 24 NOV 26")]
    assert (up_only["kite_token"], up_only["partial_mapping"]) == (None, True)
    assert up_only["underlying_id"] == m[("BSE", "SENSEX")]["id"]
    audit = q(dsn, "SELECT actor_kind, payload FROM audit_event WHERE action = 'instrument.sync'")
    assert len(audit) == 1
    assert audit[0]["actor_kind"] == "system"


def test_resync_is_idempotent(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    ids = {k: r["id"] for k, r in active(dsn).items()}
    again = sync_files(dsn, day1, DAY1)
    assert (again["inserted"], again["updated"], again["closed"], again["reopened"]) == (0, 0, 0, 0)
    assert {k: r["id"] for k, r in active(dsn).items()} == ids


def test_next_day_expiry_and_lot_change(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    before = active(dsn)
    day2 = without(day1, {"RELIANCE26NOVFUT"}, {"NSE_FO|60001"})
    day2.kite["NFO"] = day2.kite["NFO"].replace(",0.1,65,FUT,", ",0.1,75,FUT,")
    day2.upstox["NSE"][:] = [
        r if r["instrument_key"] != "NSE_FO|61471" else {**r, "lot_size": 75} for r in day2.upstox["NSE"]
    ]
    report = sync_files(dsn, day2, DAY2)
    assert (report["closed"], report["updated"], report["inserted"]) == (1, 1, 0)
    expired = q(dsn, "SELECT valid_to FROM instrument WHERE tradingsymbol = 'RELIANCE26NOVFUT'")
    assert [r["valid_to"] for r in expired] == [DAY2]
    fut = active(dsn)[("NFO", "NIFTY26NOVFUT")]
    assert (fut["id"], fut["lot_size"]) == (
        before[("NFO", "NIFTY26NOVFUT")]["id"],
        75,
    )  # same contract, same id


def test_failed_sync_keeps_master_and_alerts_admins(dsn: str, day1: sync.Files) -> None:
    add_admin(dsn)
    sync_files(dsn, day1, DAY1)
    before = active(dsn)
    broken = replace(
        day1, upstox={**day1.upstox, "NSE": [{"segment": "NSE_FO", "instrument_key": "NSE_FO|1"}]}
    )
    with pytest.raises(sync.upstox.UpstoxFormatError):
        sync_files(dsn, broken, DAY2)
    assert active(dsn) == before  # AC3: yesterday's master stays active
    alerts = q(dsn, "SELECT severity, title, body FROM alert_event")
    assert len(alerts) == 1
    assert alerts[0]["severity"] == "critical"
    assert "trading not blocked" in alerts[0]["body"]


def test_db_failure_rolls_back_everything(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    before = active(dsn)
    # Two contracts claiming one kite token violates instrument_kite_uniq mid-transaction.
    bad = (
        day1.kite["NFO"]
        + '13050370,59999,NIFTY26NOV24200CE,"NIFTY",0,2026-11-24,24200,0.05,65,CE,NFO-OPT,NFO\n'
    )
    with pytest.raises(asyncpg.UniqueViolationError):
        sync_files(dsn, replace(day1, kite={**day1.kite, "NFO": bad}), DAY2)
    assert active(dsn) == before
    assert len(q(dsn, "SELECT 1 FROM audit_event WHERE action = 'instrument.sync'")) == 1


def test_mass_close_guard(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    gone_kite = {"NIFTY26NOVFUT", "NIFTY26NOV24000CE", "NIFTY26NOV24100PE", "KITEONLY", "GOLDSTAR-ST"}
    gone_up = {"NSE_FO|61471", "NSE_FO|50001", "NSE_EQ|INE0GS101011"}
    shrunk = without(day1, gone_kite, gone_up)
    with pytest.raises(sync.SyncRejected, match="would close"):
        sync_files(dsn, shrunk, DAY2)
    assert len(active(dsn)) == 12
    assert sync_files(dsn, shrunk, DAY2, allow_large_change=True)["closed"] == 5


def test_ban_import_flags_derivatives_only(dsn: str, day1: sync.Files) -> None:
    sync_files(dsn, day1, DAY1)
    report = run(banlist.import_file(dsn, BAN, DAY1))
    assert report["contracts_banned"] == 1
    m = active(dsn)
    assert m[("NFO", "RELIANCE26NOVFUT")]["fo_ban"] is True
    assert m[("NSE", "RELIANCE")]["fo_ban"] is False  # cash still tradable
    assert not any(r["fo_ban"] for k, r in m.items() if k != ("NFO", "RELIANCE26NOVFUT"))
    # A re-import for the same day replaces the list; NIL clears it.
    run(banlist.import_file(dsn, b"Securities in Ban For Trade Date 28-OCT-2026: NIL\n", DAY1))
    assert not any(r["fo_ban"] for r in active(dsn).values())
    assert len(q(dsn, "SELECT 1 FROM audit_event WHERE action = 'fo_ban.import'")) == 2


def test_ban_import_refuses_wrong_date(dsn: str) -> None:
    with pytest.raises(banlist.BanListError, match="expected 2026-10-29"):
        run(banlist.import_file(dsn, BAN, DAY2))


def test_sync_applies_latest_ban_list_to_new_contracts(dsn: str, day1: sync.Files) -> None:
    run(banlist.import_file(dsn, BAN, DAY1))  # imported before the 08:00 sync created the contracts
    report = sync_files(dsn, day1, DAY2)
    assert (report["fo_ban_list"], report["fo_ban_contracts"]) == ("2026-10-28", 1)


def test_ban_check_alerts_on_trading_days_only(dsn: str) -> None:
    add_admin(dsn)
    assert run(banlist.check(dsn, date(2026, 10, 31))) is None  # Saturday
    assert q(dsn, "SELECT 1 FROM alert_event") == []
    run(banlist.check(dsn, DAY1))
    alerts = q(dsn, "SELECT severity, title FROM alert_event")
    assert [(a["severity"], a["title"]) for a in alerts] == [("warn", "F&O ban list not imported")]
    run(banlist.import_file(dsn, BAN, DAY1))
    assert run(banlist.check(dsn, DAY1)) == DAY1
    assert len(q(dsn, "SELECT 1 FROM alert_event")) == 1


def test_holiday_seed_keeps_desk_overrides(dsn: str) -> None:
    add_admin(dsn)
    q(
        dsn,
        "INSERT INTO exchange_holiday (exchange, day, description) VALUES ('NSE', '2026-01-26', 'desk: Republic Day')",
    )
    report = run(holidays.seed(dsn, DAY1))
    assert report["covered_until"] == "2026-12-31"
    rows = q(
        dsn, "SELECT exchange, description FROM exchange_holiday WHERE day = '2026-01-26' ORDER BY exchange"
    )
    assert [(r["exchange"], r["description"]) for r in rows] == [
        ("NSE", "desk: Republic Day"),
        ("BSE", "exchange_calendars XBOM"),
        ("NFO", "exchange_calendars XBOM"),
        ("BFO", "exchange_calendars XBOM"),
    ]
    assert run(holidays.seed(dsn, DAY1))["inserted"] == 0
    # The library's calendar ends 2026-12-31, within 90 days of DAY1: admins are told to extend it.
    titles = [r["title"] for r in q(dsn, "SELECT title FROM alert_event")]
    assert titles == ["Holiday calendar coverage ending"] * 2


def test_seed_covers_the_first_job_and_every_muhurat_session(dsn: str) -> None:
    add_admin(dsn)
    report = run(holidays.seed(dsn, DAY1))
    assert report["muhurat_sessions"] == 5
    rows = q(dsn, "SELECT day, description, special_session FROM exchange_holiday WHERE exchange = 'NSE'")
    days = {r["day"]: r for r in rows}
    assert min(days).year == 2015
    want = {
        date(2022, 10, 24): ("18:15", "19:15"),
        date(2023, 11, 12): ("18:15", "19:15"),
        date(2024, 11, 1): ("18:00", "19:00"),
        date(2025, 10, 21): ("13:45", "14:45"),
        date(2026, 11, 8): ("14:20", "15:30"),
    }
    for day, (start, end) in want.items():
        assert json.loads(days[day]["special_session"]) == [{"open": start, "close": end}]
        assert "archives.nseindia.com/content/circulars/" in days[day]["description"]
    # Saturdays, from the circulars: the library lists the first and last, and neither DR switchover.
    saturdays = {
        date(2024, 1, 20): [{"open": "09:15", "close": "15:30"}],
        date(2024, 3, 2): [{"open": "09:15", "close": "10:00"}, {"open": "11:30", "close": "12:30"}],
        date(2024, 5, 18): [{"open": "09:15", "close": "10:00"}, {"open": "11:30", "close": "12:30"}],
        date(2025, 2, 1): [{"open": "09:15", "close": "15:30"}],
    }
    for day, windows in saturdays.items():
        assert day.weekday() == 5
        assert json.loads(days[day]["special_session"]) == windows
        assert "archives.nseindia.com/content/circulars/" in days[day]["description"]
    assert len([r for r in rows if r["special_session"] is not None]) == len(want) + len(saturdays)


def test_seed_keeps_a_desks_special_session(dsn: str) -> None:
    add_admin(dsn)
    q(
        dsn,
        """INSERT INTO exchange_holiday (exchange, day, description, special_session) VALUES
           ('NSE', '2024-11-01', 'desk', '[{"open": "18:01", "close": "19:01"}]')""",
    )
    run(holidays.seed(dsn, DAY1))
    row = q(
        dsn,
        "SELECT description, special_session FROM exchange_holiday WHERE day = '2024-11-01' AND exchange = 'NSE'",
    )[0]
    assert row["description"] == "desk"
    assert json.loads(row["special_session"]) == [{"open": "18:01", "close": "19:01"}]
