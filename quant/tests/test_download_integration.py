"""F-RS-1 acceptance tests: a real `vyuha-quant download run` process against a local fake Upstox server
that enforces the documented limits (scaled to the test's pace), on a real database. No broker is touched."""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from collections.abc import Iterator
from datetime import date, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote

import asyncpg
import polars as pl
import pytest
from aiolimiter import AsyncLimiter

from vyuha_quant.data import download, lake, upstox_history
from vyuha_quant.data.limits import UPSTOX_INTERVALS, UPSTOX_RATE, pace_seconds

TOKEN = "fake-token-never-logged"
PACE = float(os.environ.get("VYUHA_TEST_DOWNLOAD_PACE", "0.01"))
# The binding limit is 2,000 per 30 minutes (test_pace_stays_under_every_window_with_headroom covers the
# others). The job runs ~112× faster than real time and has 1,200 requests, so the fake enforces the same
# rate on a smaller window: at the real pace it sees 80 % of this limit, and any faster pace goes over it.
WINDOW_LIMIT = 100
WINDOW_S = WINDOW_LIMIT * pace_seconds(UPSTOX_RATE, 1.0) / (pace_seconds(UPSTOX_RATE) / 0.01)


class FakeUpstox(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _Handler)
        self.lock = threading.Lock()
        self.requests: list[str] = []
        self.times: deque[float] = deque()
        self.http_429 = 0
        self.expire_after: int | None = None
        self.corrupt_first: int = 0  # this many replies carry an unreadable row
        self.peers: set[str] = set()

    def over_limit(self, now: float) -> bool:
        self.times.append(now)
        while now - self.times[0] > WINDOW_S:
            self.times.popleft()
        return len(self.times) > WINDOW_LIMIT


class _Handler(BaseHTTPRequestHandler):
    server: FakeUpstox

    def log_message(self, format: str, *args: Any) -> None:
        pass

    def _reply(self, status: int, body: dict[str, Any]) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        assert self.headers["Authorization"] == f"Bearer {TOKEN}"
        with self.server.lock:
            self.server.requests.append(self.path)
            self.server.peers.add(self.client_address[0])
            corrupt = self.server.corrupt_first > 0
            self.server.corrupt_first -= corrupt
            n = len(self.server.requests)
            limited = self.server.over_limit(time.monotonic())
            if limited:
                self.server.http_429 += 1
            expired = self.server.expire_after is not None and n > self.server.expire_after
        if expired:
            return self._reply(401, {"status": "error"})
        if limited:
            return self._reply(429, {"status": "error"})
        _, _, _, _key, _unit, _value, to, frm = self.path.split("/")
        if corrupt:
            return self._reply(200, {"status": "success", "data": {"candles": [["garbage"]]}})
        self._reply(
            200,
            {
                "status": "success",
                "data": {
                    "candles": _candles(unquote(_unit), date.fromisoformat(frm), date.fromisoformat(to))
                },
            },
        )


def _candles(unit: str, frm: date, to: date) -> list[list[Any]]:
    """Newest first (as the broker sends them): three bars per weekday, two of them float-unfriendly."""
    out: list[list[Any]] = []
    d = to
    while d >= frm:
        if d.weekday() < 5:
            if unit == "days":
                out.append([f"{d}T00:00:00+05:30", 100.05, 101.1, 99.95, 100.1, 1000, 0])
            else:
                for hhmm in ("15:29", "12:00", "09:15"):
                    out.append([f"{d}T{hhmm}:00+05:30", 100.05, 101.1, 99.95, 100.1, 10, 0])
        d -= timedelta(days=1)
    return out


@pytest.fixture
def fake() -> Iterator[FakeUpstox]:
    server = FakeUpstox()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()


SYMBOLS = [f"SYM{i}" for i in range(50)]


async def _seed(
    dsn: str,
    *,
    entitled: bool = True,
    static_ip: str = "127.0.0.1",
    role: str = "admin",
    status: str = "disconnected",
) -> str:
    conn = await asyncpg.connect(dsn)
    try:
        for i, s in enumerate(SYMBOLS):
            await conn.execute(
                """INSERT INTO instrument (exchange, segment, symbol, tradingsymbol, tick_size, upstox_key, valid_from)
                   VALUES ('NSE', 'EQ', $1, $1, 0.05, $2, '2020-01-01')""",
                s,
                f"NSE_EQ|INE{i:09d}",
            )
        await conn.execute(
            """INSERT INTO app_user (username, display_name, role, password_hash)
               VALUES ('owner', 'Owner', $1, 'x')""",
            role,
        )
        account: str = str(
            await conn.fetchval(
                """INSERT INTO broker_account (broker, client_code, holder_name, api_key_enc, api_secret_enc,
                     static_ip, data_entitled, status)
                   VALUES ('upstox', 'UP1', 'Owner', 'k', 's', $1::inet, $2, $3) RETURNING id""",
                static_ip,
                entitled,
                status,
            )
        )
        return account
    finally:
        await conn.close()


def _spec(tmp: Path, **over: Any) -> Path:
    (tmp / "list.csv").write_text("Symbol\n" + "\n".join(SYMBOLS) + "\n")
    spec = tmp / "job.json"
    # 2 years × 1 minute, the F-RS-1 AC. An ended range keeps the test independent of today.
    spec.write_text(
        json.dumps(
            {
                "interval": "1minute",
                "from": "2024-01-01",
                "to": "2025-12-31",
                "constituents_csv": "list.csv",
                **over,
            }
        )
    )
    return spec


def _start(
    dsn: str, fake: FakeUpstox, tmp: Path, spec: Path, account: str, *, token: str = TOKEN
) -> subprocess.Popen[str]:
    env = {
        **os.environ,
        "VYUHA_ENV": "dev",
        "POSTGRES_URL": dsn,
        "DATA_LAKE_DIR": str(tmp / "lake"),
        "UPSTOX_API_BASE": f"http://127.0.0.1:{fake.server_address[1]}",
        "DOWNLOAD_PACE_S": str(PACE),
    }
    cmd = [
        sys.executable,
        "-m",
        "vyuha_quant.cli",
        "download",
        "run",
        "--spec",
        str(spec),
        "--account",
        account,
        "--user",
        "owner",
    ]
    p = subprocess.Popen(
        cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    assert p.stdin is not None
    p.stdin.write(token + "\n")
    p.stdin.close()
    return p


def _finish(p: subprocess.Popen[str]) -> tuple[dict[str, Any], str]:
    out, err = p.communicate(timeout=600)
    return json.loads(out), err


async def _one(dsn: str, sql: str) -> Any:
    conn = await asyncpg.connect(dsn)
    try:
        return await conn.fetchrow(sql)
    finally:
        await conn.close()


def _stored_rows(lake_dir: Path) -> pl.DataFrame:
    return pl.read_parquet(sorted(lake_dir.glob("bars/**/part-0.parquet")), hive_partitioning=False)


def test_50_symbols_2_years_kill_9_resume_and_idempotent_rerun(
    dsn: str, fake: FakeUpstox, tmp_path: Path
) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path)

    # kill -9 part-way through.
    p = _start(dsn, fake, tmp_path, spec, account)
    deadline = time.monotonic() + 60
    while len(fake.requests) < 400 and time.monotonic() < deadline:
        time.sleep(0.01)
    assert len(fake.requests) >= 400
    p.send_signal(signal.SIGKILL)
    p.wait(timeout=10)
    row = asyncio.run(_one(dsn, "SELECT status::text AS status, progress FROM download_job"))
    assert row["status"] == "running"
    done = json.loads(row["progress"])["done"]
    assert 0 < done < 1200
    before_resume = len(fake.requests)

    # Resume: same job, and at most the interrupted chunk is fetched twice.
    report, log = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert report["ok"], report
    assert before_resume + report["requests"] == len(fake.requests)
    assert report["chunks"] == 1200 - done
    assert fake.http_429 == 0
    assert len(fake.requests) - len(set(fake.requests)) <= 1
    assert TOKEN not in log
    assert TOKEN not in json.dumps(report)
    row = asyncio.run(
        _one(
            dsn,
            "SELECT count(*) AS jobs, min(status::text) AS status, min(progress::text) AS progress, min(error) AS error FROM download_job",
        )
    )
    assert (row["jobs"], row["status"], row["error"]) == (1, "done", None)
    assert json.loads(row["progress"])["done"] == 1200

    # Every bar exactly once, exact prices, UTC.
    bars = _stored_rows(tmp_path / "lake")
    weekdays = sum(1 for i in range(731) if (date(2024, 1, 1) + timedelta(days=i)).weekday() < 5)
    assert bars.height == 50 * weekdays * 3
    assert bars.schema["ts"] == pl.Datetime("us", "UTC")
    assert set(bars["open"].to_list()) == {1_000_500}
    one = lake.read_bars(tmp_path / "lake", "upstox", "1minute", 1)
    assert one["ts"].is_sorted()
    assert one["ts"].n_unique() == one.height

    # The catalogue matches the files.
    cat = asyncio.run(_one(dsn, "SELECT count(*) AS files, sum(row_count) AS rows FROM dataset"))
    assert (cat["files"], cat["rows"]) == (100, bars.height)

    # Re-run: nothing requested, nothing new.
    seen = len(fake.requests)
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert (report["requests"], report["new_bars"], report["chunks"]) == (0, 0, 0)
    assert len(fake.requests) == seen

    # Even without the coverage rows, a refetch adds nothing and leaves every file as it was.
    shas = {f: f.read_bytes() for f in (tmp_path / "lake").glob("bars/**/part-0.parquet")}

    async def forget() -> None:
        conn = await asyncpg.connect(dsn)
        await conn.execute("DELETE FROM download_coverage WHERE instrument_id = 1")
        await conn.close()

    asyncio.run(forget())
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert (report["requests"], report["new_bars"]) == (24, 0)
    assert {f: f.read_bytes() for f in shas} == shas


def test_token_expiry_stops_the_job_and_a_rerun_finishes_it(
    dsn: str, fake: FakeUpstox, tmp_path: Path
) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path, to="2024-03-31", constituents_csv=None, symbols=["NSE:SYM0", "NSE:SYM1"])
    fake.expire_after = 2
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert report["ok"] is False
    assert "BROKER_TOKEN_EXPIRED" in report["error"]
    row = asyncio.run(_one(dsn, "SELECT status::text AS status, error FROM download_job"))
    assert row["status"] == "failed"
    assert "BROKER_TOKEN_EXPIRED" in row["error"]

    fake.expire_after = None
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert report["ok"], report
    assert report["chunks"] == 4
    row = asyncio.run(_one(dsn, "SELECT count(*) AS jobs, min(status::text) AS status FROM download_job"))
    assert (row["jobs"], row["status"]) == (1, "done")


@pytest.mark.parametrize(
    ("seed", "code"),
    [
        ({"static_ip": "203.0.113.7"}, "STATIC_IP_NOT_ON_HOST"),
        ({"entitled": False}, "DOWNLOAD_ACCOUNT_NOT_USABLE"),
        ({"status": "disabled"}, "DOWNLOAD_ACCOUNT_NOT_USABLE"),
        ({"role": "trader"}, "DOWNLOAD_ACCOUNT_NOT_USABLE"),  # not mapped to the account
    ],
)
def test_refused_before_any_request(
    dsn: str, fake: FakeUpstox, tmp_path: Path, seed: dict[str, Any], code: str
) -> None:
    account = asyncio.run(_seed(dsn, **seed))
    report, _ = _finish(_start(dsn, fake, tmp_path, _spec(tmp_path), account))
    assert report["ok"] is False
    assert code in report["error"]
    assert fake.requests == []


def test_unknown_symbol_is_refused_before_any_request(dsn: str, fake: FakeUpstox, tmp_path: Path) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path, symbols=["NSE:NOPE"], constituents_csv=None)
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert "NSE:NOPE" in report["error"]
    assert fake.requests == []


def test_second_job_on_one_account_is_refused(dsn: str, fake: FakeUpstox, tmp_path: Path) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path)
    first = _start(dsn, fake, tmp_path, spec, account)
    deadline = time.monotonic() + 30
    while not fake.requests and time.monotonic() < deadline:
        time.sleep(0.01)
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert "DOWNLOAD_ALREADY_RUNNING" in report["error"]
    first.kill()
    first.wait(timeout=10)


def test_plan_reports_requests_without_a_token_or_a_request(
    dsn: str, fake: FakeUpstox, tmp_path: Path
) -> None:
    asyncio.run(_seed(dsn))
    env = {**os.environ, "VYUHA_ENV": "dev", "POSTGRES_URL": dsn}
    cmd = [sys.executable, "-m", "vyuha_quant.cli", "download", "plan", "--spec", str(_spec(tmp_path))]
    out = json.loads(subprocess.run(cmd, env=env, capture_output=True, text=True, check=True).stdout)
    assert (out["instruments"], out["requests"]) == (50, 1200)
    assert out["estimated_minutes"] == pytest.approx(22.5)
    assert fake.requests == []
    assert isinstance(datetime.fromisoformat(out["to"]), datetime)


class _Crash(BaseException):
    """Stands in for kill -9: nothing in the job catches it, so no cleanup is recorded."""


async def _catalogue(dsn: str) -> dict[str, tuple[str, int]]:
    conn = await asyncpg.connect(dsn)
    try:
        return {
            r["path"]: (r["sha256"], r["row_count"])
            for r in await conn.fetch("SELECT path, sha256, row_count FROM dataset")
        }
    finally:
        await conn.close()


async def _audit_actions(dsn: str) -> list[dict[str, Any]]:
    conn = await asyncpg.connect(dsn)
    try:
        rows = await conn.fetch(
            "SELECT action, payload FROM audit_event WHERE entity = 'download_job' ORDER BY id"
        )
    finally:
        await conn.close()
    return [{"action": r["action"], **json.loads(r["payload"])} for r in rows]


@pytest.mark.parametrize(
    ("spec_over", "crash_on_replace", "files"),
    [
        # A year's last 1-minute chunk: no later chunk would repair the catalogue.
        ({"from": "2024-12-01", "to": "2024-12-31"}, 1, 1),
        # One daily chunk spanning many files, stopped between two of them.
        ({"interval": "day", "from": "2015-01-01", "to": "2020-12-31"}, 3, 6),
    ],
)
def test_crash_between_lake_write_and_commit_leaves_a_correct_catalogue(
    dsn: str,
    fake: FakeUpstox,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    spec_over: dict[str, str],
    crash_on_replace: int,
    files: int,
) -> None:
    account = asyncio.run(_seed(dsn))
    path = _spec(tmp_path, constituents_csv=None, symbols=["NSE:SYM0"], **spec_over)
    spec = download.JobSpec.model_validate_json(path.read_text())
    lake_dir = tmp_path / "lake"
    base = f"http://127.0.0.1:{fake.server_address[1]}"
    real, calls = lake._replace, [0]

    def crashing(tmp: Path, dst: Path) -> None:
        real(tmp, dst)  # the file lands; the transaction after it never runs
        calls[0] += 1
        if calls[0] == crash_on_replace:
            raise _Crash

    def run() -> dict[str, Any]:
        return asyncio.run(
            download.run(
                dsn,
                spec,
                tmp_path,
                account=account,
                username="owner",
                token=TOKEN,
                lake_dir=lake_dir,
                base_url=base,
                pace=0.001,
            )
        )

    monkeypatch.setattr(lake, "_replace", crashing)
    with pytest.raises(_Crash):
        run()
    assert asyncio.run(_catalogue(dsn)) == {}
    monkeypatch.setattr(lake, "_replace", real)
    assert run()["chunks"] > 0

    catalogue = asyncio.run(_catalogue(dsn))
    on_disk = {f.relative_to(lake_dir).as_posix(): f for f in lake_dir.glob("bars/**/part-0.parquet")}
    assert len(on_disk) == files
    assert set(catalogue) == set(on_disk)
    for rel, f in on_disk.items():
        assert catalogue[rel] == (hashlib.sha256(f.read_bytes()).hexdigest(), pl.read_parquet(f).height)


def test_unreadable_rows_keep_the_chunk_out_of_coverage_so_a_rerun_retries(
    dsn: str, fake: FakeUpstox, tmp_path: Path
) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path, to="2024-02-29", constituents_csv=None, symbols=["NSE:SYM0"])
    fake.corrupt_first = 1
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert (report["chunks_incomplete"], report["dropped"]["malformed"]) == (1, 1)
    report, _ = _finish(_start(dsn, fake, tmp_path, spec, account))
    assert (report["chunks"], report["chunks_incomplete"]) == (1, 0)
    assert report["new_bars"] > 0


def test_audit_names_account_and_user_on_every_run_and_records_failure(
    dsn: str, fake: FakeUpstox, tmp_path: Path
) -> None:
    account = asyncio.run(_seed(dsn))
    spec = _spec(tmp_path, to="2024-02-29", constituents_csv=None, symbols=["NSE:SYM0"])
    fake.expire_after = 1
    _finish(_start(dsn, fake, tmp_path, spec, account))
    fake.expire_after = None
    _finish(_start(dsn, fake, tmp_path, spec, account))
    events = asyncio.run(_audit_actions(dsn))
    assert [e["action"] for e in events] == [
        "data.download.start",
        "data.download.failed",
        "data.download.start",
        "data.download.done",
    ]
    assert all(e["account_id"] == account and e["requested_by"] == "owner" for e in events)
    assert "BROKER_TOKEN_EXPIRED" in events[1]["error"]
    assert events[2]["resumed_at_chunk"] == 1
    assert TOKEN not in json.dumps(events)


def test_requests_really_leave_from_the_bound_address(fake: FakeUpstox) -> None:
    """Needs a second loopback address, which Linux has (CI) and macOS doesn't by default."""
    try:
        upstox_history.preflight_source_ip("127.0.0.2")
    except upstox_history.StaticIPNotOnHost:
        pytest.skip("no 127.0.0.2 on this host")

    async def fetch() -> None:
        c = upstox_history.UpstoxHistory(
            TOKEN, "127.0.0.2", AsyncLimiter(10, 1), base_url=f"http://127.0.0.1:{fake.server_address[1]}"
        )
        await c.candles("K", UPSTOX_INTERVALS["day"], date(2024, 1, 1), date(2024, 1, 31))
        await c.aclose()

    asyncio.run(fetch())
    assert fake.peers == {"127.0.0.2"}


def test_database_refuses_addresses_that_pin_nothing(dsn: str) -> None:
    for bad in ("0.0.0.0", "::", "224.0.0.1", "ff02::1"):
        with pytest.raises(asyncpg.CheckViolationError):
            asyncio.run(_insert_account(dsn, bad))


async def _insert_account(dsn: str, ip: str) -> None:
    conn = await asyncpg.connect(dsn)
    try:
        await conn.execute(
            """INSERT INTO broker_account (broker, client_code, holder_name, api_key_enc, api_secret_enc, static_ip)
               VALUES ('upstox', 'UP9', 'X', 'k', 's', $1::inet)""",
            ip,
        )
    finally:
        await conn.close()
