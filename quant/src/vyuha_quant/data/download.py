"""Historical download jobs (F-RS-1, STRATEGY_SDK_AND_BACKTEST §1): plan, fetch, validate, store.

`download_coverage` is the resume point. A chunk is written to the lake first and then recorded in one
transaction with its catalogue rows and the job's progress, so after a kill the chunk is either recorded
or fetched again: its bars merge away as duplicates and the catalogue catches up with the files. A re-run
plans only the dates nobody has finished. A chunk with rejected rows (unreadable, invalid, or a whole day outside any
known session) isn't recorded, so a re-run retries it.
"""

from __future__ import annotations

import csv
import io
import logging
from collections.abc import Sequence
from dataclasses import asdict, dataclass, field
from datetime import UTC, date, datetime, time, timedelta
from pathlib import Path
from typing import Any, Literal
from urllib.parse import urlsplit

import polars as pl
from aiolimiter import AsyncLimiter
from pydantic import BaseModel, ConfigDict, Field

from .. import config, db
from .lake import SCHEMA, PartitionWrite, empty_bars, to_price_units, write_bars
from .limits import UPSTOX_INTERVALS, UPSTOX_RATE, IntervalLimit, pace_seconds
from .upstox_history import API_BASE, DownloadError, UpstoxHistory, preflight_source_ip

log = logging.getLogger("vyuha.download")

SOURCE = "upstox"
SESSION_OPEN = time(9, 15)
SESSION_CLOSE = time(15, 30)
_LOCK_NAMESPACE = (
    0x76796468  # pg advisory lock: one running job per source, which shares one set of lake files
)
_LOCK_UPSTOX = 1

Range = tuple[date, date]  # both ends inclusive


class SpecError(DownloadError):
    code = "DOWNLOAD_SPEC_INVALID"


class JobBusy(DownloadError):
    code = "DOWNLOAD_ALREADY_RUNNING"


class AccountNotUsable(DownloadError):
    code = "DOWNLOAD_ACCOUNT_NOT_USABLE"


class JobSpec(BaseModel):
    """A job file: what to fetch. `to` defaults to the last finished day; later dates are clamped to it."""

    model_config = ConfigDict(extra="forbid", frozen=True, populate_by_name=True)

    interval: Literal["1minute", "day"]
    from_: date = Field(alias="from")
    to: date | None = None
    symbols: tuple[str, ...] = ()  # "NSE:NIFTY 50"
    constituents_csv: str | None = (
        None  # NSE's index list, downloaded by hand (ADR-019); relative to the job file
    )
    include_expired: Literal[False] = False  # expired F&O is M4.1b


@dataclass(frozen=True, slots=True)
class Instrument:
    id: int
    upstox_key: str
    label: str


@dataclass(frozen=True, slots=True)
class Chunk:
    instrument: Instrument
    start: date
    end: date


@dataclass(slots=True)
class Dropped:
    outside_session: int = 0
    bad_ohlc: int = 0
    duplicates: int = 0
    malformed: int = 0
    session_days_unplaced: int = 0  # days with bars but none inside a known session: a special session?

    @property
    def rejected(self) -> int:
        """Drops that leave a hole in the chunk. Routine ones (a 15:30 candle, a duplicate) don't."""
        return self.malformed + self.bad_ohlc + self.session_days_unplaced


@dataclass(slots=True)
class Totals:
    requests: int = 0
    http_429: int = 0
    new_bars: int = 0
    chunks_incomplete: int = (
        0  # had rejected rows (Dropped.rejected), so not recorded; a re-run fetches them again
    )
    days_without_bars: int = 0
    dropped: Dropped = field(default_factory=Dropped)


def last_finished_day(now: datetime | None = None) -> date:
    """Yesterday IST. Today's candles are incomplete, and a chunk once recorded is never fetched again."""
    return config.today_ist(now) - timedelta(days=1)


def read_constituents(text: str) -> list[str]:
    """Symbols from NSE's index constituents CSV (columns include Symbol and Series)."""
    rows = list(csv.DictReader(io.StringIO(text.lstrip("﻿"))))
    if not rows or "Symbol" not in rows[0]:
        raise SpecError("constituents CSV has no Symbol column")
    return [f"NSE:{r['Symbol'].strip()}" for r in rows if r["Symbol"].strip()]


def load_symbols(spec: JobSpec, spec_dir: Path) -> list[str]:
    symbols = list(spec.symbols)
    if spec.constituents_csv:
        path = spec_dir / spec.constituents_csv
        try:
            symbols += read_constituents(path.read_text())
        except OSError:
            raise SpecError(f"cannot read {path}: download it from NSE by hand (docs/RUN_LOCAL.md)") from None
    if not symbols:
        raise SpecError("the job names no instruments")
    return list(dict.fromkeys(symbols))


def missing_ranges(want: Range, covered: Sequence[Range]) -> list[Range]:
    """The parts of `want` that no covered range includes."""
    out: list[Range] = []
    cursor = want[0]
    for start, end in sorted(covered):
        if end < cursor:
            continue
        if start > want[1]:
            break
        if start > cursor:
            out.append((cursor, start - timedelta(days=1)))
        cursor = max(cursor, end + timedelta(days=1))
    if cursor <= want[1]:
        out.append((cursor, want[1]))
    return out


def split_range(r: Range, max_months: int) -> list[Range]:
    """Splits a range on calendar-month boundaries, at most `max_months` calendar months per piece."""
    out: list[Range] = []
    start = r[0]
    while start <= r[1]:
        month = start.year * 12 + (start.month - 1) + max_months
        first_after = date(month // 12, month % 12 + 1, 1)
        end = min(r[1], first_after - timedelta(days=1))
        out.append((start, end))
        start = end + timedelta(days=1)
    return out


def _sessions(day_rows: Any) -> dict[date, list[tuple[time, time]]]:
    """Holidays → their special-session windows (`[{"open": "18:15", "close": "19:15"}]`), or none."""
    out: dict[date, list[tuple[time, time]]] = {}
    for r in day_rows:
        windows = r["special_session"] or []
        out[r["day"]] = [(time.fromisoformat(w["open"]), time.fromisoformat(w["close"])) for w in windows]
    return out


def normalise(
    candles: Sequence[Sequence[Any]],
    interval: str,
    holidays: dict[date, list[tuple[time, time]]],
    dropped: Dropped,
) -> pl.DataFrame:
    """Validates one chunk: sorted, deduplicated, inside the session, sane OHLC. Bad rows are counted."""
    rows: dict[datetime, tuple[Any, ...]] = {}
    outside_days: set[date] = set()
    for c in candles:
        try:
            parsed = datetime.fromisoformat(c[0])
            if parsed.tzinfo is None:  # never guess a zone (invariant 9)
                raise ValueError("naive timestamp")
            ts = parsed.astimezone(config.IST)
            o, hi, lo, cl = (to_price_units(c[i]) for i in (1, 2, 3, 4))
            volume, oi = int(c[5]), int(c[6])
        except (ValueError, TypeError, IndexError, ArithmeticError):
            dropped.malformed += 1
            continue
        if not 0 < lo <= min(o, cl) <= max(o, cl) <= hi or volume < 0 or oi < 0:
            dropped.bad_ohlc += 1
            continue
        if interval != "day" and not _in_session(ts, holidays):
            dropped.outside_session += 1
            outside_days.add(ts.date())
            continue
        if ts in rows:
            dropped.duplicates += 1
            continue
        rows[ts] = (ts.astimezone(UTC), o, hi, lo, cl, volume, oi)
    dropped.session_days_unplaced += len(outside_days - {k.date() for k in rows})
    if not rows:
        return empty_bars()
    ordered = [rows[k] for k in sorted(rows)]
    return pl.DataFrame(ordered, schema=SCHEMA, orient="row")


def _in_session(ts: datetime, holidays: dict[date, list[tuple[time, time]]]) -> bool:
    t = ts.time()
    windows = holidays.get(ts.date())
    if windows is None:
        return SESSION_OPEN <= t < SESSION_CLOSE
    return any(a <= t < b for a, b in windows)


def _weekdays_without_bars(bars: pl.DataFrame, chunk: Chunk, holidays: dict[date, Any]) -> list[date]:
    have = (
        set(bars["ts"].dt.convert_time_zone("Asia/Kolkata").dt.date().unique().to_list())
        if not bars.is_empty()
        else set()
    )
    days = (chunk.start + timedelta(days=i) for i in range((chunk.end - chunk.start).days + 1))
    return [d for d in days if d.weekday() < 5 and d not in holidays and d not in have]


async def _instruments(conn: db.Conn, symbols: list[str]) -> list[Instrument]:
    out: list[Instrument] = []
    bad: list[str] = []
    for s in symbols:
        exchange, _, tradingsymbol = s.partition(":")
        row = await conn.fetchrow(
            """SELECT id, upstox_key FROM instrument
               WHERE exchange::text = $1 AND tradingsymbol = $2 AND valid_to IS NULL AND upstox_key IS NOT NULL""",
            exchange,
            tradingsymbol,
        )
        if row is None:
            bad.append(s)
        else:
            out.append(Instrument(row["id"], row["upstox_key"], s))
    if bad:
        raise SpecError(f"not in the instrument master with an Upstox key: {', '.join(bad)}")
    return out


async def _plan(
    conn: db.Conn, spec: JobSpec, limit: IntervalLimit, items: list[Instrument], to: date
) -> list[Chunk]:
    if spec.from_ < limit.earliest:
        raise SpecError(f"Upstox has {spec.interval} data only from {limit.earliest}")
    if spec.from_ > to:
        raise SpecError(f"nothing to fetch: from {spec.from_} is after {to}")
    chunks: list[Chunk] = []
    for inst in items:
        rows = await conn.fetch(
            """SELECT from_date, to_date FROM download_coverage
               WHERE source = $1 AND interval = $2 AND instrument_id = $3""",
            SOURCE,
            spec.interval,
            inst.id,
        )
        covered = [(r["from_date"], r["to_date"]) for r in rows]
        for gap in missing_ranges((spec.from_, to), covered):
            chunks += [Chunk(inst, a, b) for a, b in split_range(gap, limit.max_months)]
    return chunks


async def plan_report(dsn: str, spec: JobSpec, spec_dir: Path, now: datetime | None = None) -> dict[str, Any]:
    """What a run would fetch, without a token or a single broker request."""
    conn = await db.connect(dsn)
    try:
        items = await _instruments(conn, load_symbols(spec, spec_dir))
        to = min(spec.to or date.max, last_finished_day(now))
        chunks = await _plan(conn, spec, UPSTOX_INTERVALS[spec.interval], items, to)
    finally:
        await conn.close()
    pace = pace_seconds(UPSTOX_RATE)
    return {
        "interval": spec.interval,
        "from": spec.from_,
        "to": to,
        "instruments": len(items),
        "requests": len(chunks),
        "pace_seconds": pace,
        "estimated_minutes": round(len(chunks) * pace / 60, 1),
    }


async def _upsert_dataset(tx: db.Conn, interval: str, instrument_id: int, w: PartitionWrite) -> None:
    await tx.execute(
        """INSERT INTO dataset (kind, interval, source, instrument_id, from_ts, to_ts, path, row_count, sha256)
           VALUES ('bars', $1, $2, $3, $4, $5, $6, $7, $8)
           ON CONFLICT (path) DO UPDATE SET from_ts = EXCLUDED.from_ts, to_ts = EXCLUDED.to_ts,
             row_count = EXCLUDED.row_count, sha256 = EXCLUDED.sha256""",
        interval,
        SOURCE,
        instrument_id,
        w.from_ts,
        w.to_ts,
        w.path,
        w.row_count,
        w.sha256,
    )


async def _fail(conn: db.Conn, job_id: Any, who: dict[str, Any], error: str) -> None:
    """Best effort: the failure may be the database itself, and then a re-run resumes the job anyway."""
    try:
        async with conn.transaction():
            await conn.execute(
                "UPDATE download_job SET status = 'failed', error = $2, updated_at = now() WHERE id = $1",
                job_id,
                error,
            )
            await db.audit(conn, "data.download.failed", "download_job", str(job_id), {**who, "error": error})
    except Exception:
        log.exception("could not record the failure of job %s", job_id)


async def _open_job(
    conn: db.Conn, account_id: Any, user_id: Any, stored: dict[str, Any], total: int
) -> tuple[Any, int]:
    """Continues this account's unfinished job for the same spec, or starts one. Returns (id, chunks done)."""
    row = await conn.fetchrow(
        """SELECT id, progress FROM download_job
           WHERE account_id = $1 AND spec = $2 AND status IN ('queued', 'running', 'failed')
           ORDER BY created_at DESC LIMIT 1""",
        account_id,
        stored,
    )
    if row is None:
        job_id = await conn.fetchval(
            """INSERT INTO download_job (requested_by, account_id, spec, status, progress)
               VALUES ($1, $2, $3, 'running', $4) RETURNING id""",
            user_id,
            account_id,
            stored,
            {"done": 0, "total": total, "last_cursor": None, "new_bars": 0},
        )
        return job_id, 0
    done = int(row["progress"].get("done", 0))
    await conn.execute(
        """UPDATE download_job SET status = 'running', error = NULL, updated_at = now(),
           progress = progress || $2 WHERE id = $1""",
        row["id"],
        {"total": done + total},
    )
    return row["id"], done


async def run(
    dsn: str,
    spec: JobSpec,
    spec_dir: Path,
    *,
    account: str,
    username: str,
    token: str,
    lake_dir: Path,
    base_url: str = API_BASE,
    pace: float | None = None,
    now: datetime | None = None,
) -> dict[str, Any]:
    if pace is not None and urlsplit(base_url).hostname not in ("127.0.0.1", "localhost"):
        raise ValueError("a pace override is for a loopback API only: the real one gets the documented pace")
    conn = await db.connect(dsn)
    try:
        acct = await conn.fetchrow(
            """SELECT id, broker::text AS broker, host(static_ip) AS static_ip, data_entitled, status::text AS status
               FROM broker_account WHERE id = $1::uuid""",
            account,
        )
        if acct is None:
            raise AccountNotUsable("no such broker account")
        if acct["broker"] != SOURCE:
            raise AccountNotUsable(f"M4.1 downloads through Upstox accounts only, not {acct['broker']}")
        if acct["status"] == "disabled":
            raise AccountNotUsable("the account is disabled")
        if not acct["data_entitled"]:
            raise AccountNotUsable("the account is not data-entitled")
        if not await conn.fetchval("SELECT pg_try_advisory_lock($1, $2)", _LOCK_NAMESPACE, _LOCK_UPSTOX):
            raise JobBusy("another Upstox download is running")  # released when its connection dies
        source_ip = acct["static_ip"]
        preflight_source_ip(source_ip)
        user_id = await conn.fetchval(
            """SELECT u.id FROM app_user u
               WHERE u.username = $1 AND u.is_active AND (u.role IN ('admin', 'risk_manager') OR EXISTS
                 (SELECT 1 FROM account_user_map m WHERE m.user_id = u.id AND m.account_id = $2))""",
            username,
            acct["id"],
        )
        if user_id is None:
            raise AccountNotUsable(f"{username} is not an active user mapped to this account")

        limit = UPSTOX_INTERVALS[spec.interval]
        items = await _instruments(conn, load_symbols(spec, spec_dir))
        to = min(spec.to or date.max, last_finished_day(now))
        chunks = await _plan(conn, spec, limit, items, to)
        holidays = _sessions(
            await conn.fetch("SELECT day, special_session FROM exchange_holiday WHERE exchange = 'NSE'")
        )
        stored = {
            "interval": spec.interval,
            "from": spec.from_.isoformat(),
            "to": to.isoformat(),
            "instruments": sorted(i.id for i in items),
            "include_expired": False,
        }
        who = {"account_id": str(acct["id"]), "requested_by": username}
        async with conn.transaction():
            job_id, done = await _open_job(conn, acct["id"], user_id, stored, len(chunks))
            await db.audit(
                conn,
                "data.download.start",
                "download_job",
                str(job_id),
                {**stored, **who, "chunks": len(chunks), "resumed_at_chunk": done},
            )

        step = pace if pace is not None else pace_seconds(UPSTOX_RATE)
        client: UpstoxHistory | None = None
        totals = Totals()
        try:
            client = UpstoxHistory(token, source_ip, AsyncLimiter(1, step), base_url=base_url)
            for c in chunks:
                raw = await client.candles(c.instrument.upstox_key, limit, c.start, c.end)
                rejected_before = totals.dropped.rejected
                bars = normalise(raw, spec.interval, holidays, totals.dropped)
                complete = totals.dropped.rejected == rejected_before
                writes = write_bars(lake_dir, SOURCE, spec.interval, c.instrument.id, bars)
                totals.new_bars += sum(w.new_rows for w in writes)
                gaps = _weekdays_without_bars(bars, c, holidays)
                totals.days_without_bars += len(gaps)
                if gaps:  # logged, never filled
                    log.warning(
                        "no bars: %s %s %d weekday(s) from %s",
                        c.instrument.label,
                        spec.interval,
                        len(gaps),
                        gaps[0],
                    )
                if not complete:
                    totals.chunks_incomplete += 1
                    log.error("rejected rows: %s %s..%s not recorded", c.instrument.label, c.start, c.end)
                else:
                    done += 1
                async with conn.transaction():
                    for w in writes:
                        await _upsert_dataset(conn, spec.interval, c.instrument.id, w)
                    if complete:
                        await conn.execute(
                            """INSERT INTO download_coverage (source, interval, instrument_id, from_date, to_date, job_id)
                               VALUES ($1, $2, $3, $4, $5, $6) ON CONFLICT DO NOTHING""",
                            SOURCE,
                            spec.interval,
                            c.instrument.id,
                            c.start,
                            c.end,
                            job_id,
                        )
                    await conn.execute(
                        """UPDATE download_job SET updated_at = now(), progress = progress || $2 WHERE id = $1""",
                        job_id,
                        {
                            "done": done,
                            "last_cursor": f"{c.instrument.id}:{c.end}",
                            "new_bars": totals.new_bars,
                        },
                    )
        except Exception as e:
            # DownloadError text is written to be safe to store; anything else gets only its type.
            error = str(e) if isinstance(e, DownloadError) else f"INTERNAL: {type(e).__name__}"
            await _fail(conn, job_id, who, error)
            raise
        finally:
            if client is not None:
                totals.requests, totals.http_429 = client.requests, client.rate_limited
                await client.aclose()
            log.info("download totals: %s", asdict(totals))
        if totals.chunks_incomplete:  # not done: stays resumable, and the CLI exits non-zero
            error = f"DOWNLOAD_INCOMPLETE: {totals.chunks_incomplete} chunk(s) had rejected rows, run again"
            await _fail(conn, job_id, {**who, **asdict(totals)}, error)
        else:
            async with conn.transaction():
                await conn.execute(
                    "UPDATE download_job SET status = 'done', updated_at = now() WHERE id = $1", job_id
                )
                await db.audit(
                    conn, "data.download.done", "download_job", str(job_id), {**who, **asdict(totals)}
                )
        return {"job_id": str(job_id), "to": to, "chunks": len(chunks), **asdict(totals)}
    finally:
        await conn.close()
