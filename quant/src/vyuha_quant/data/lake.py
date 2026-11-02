"""Parquet lake for downloaded bars (DATA_MODEL §5). Raw prices are never changed once written; an
adjusted series is computed on read from separately stored corporate-action factors."""

from __future__ import annotations

import errno
import fcntl
import hashlib
import logging
import os
import sys
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import date, datetime
from decimal import ROUND_HALF_EVEN, Decimal
from pathlib import Path

import polars as pl

log = logging.getLogger("vyuha.lake")

IST_NAME = "Asia/Kolkata"
PRICE_UNITS = Decimal(10_000)  # engine `Price`: ₹1 = 10,000 units

SCHEMA: dict[str, pl.DataType | type[pl.DataType]] = {
    "ts": pl.Datetime("us", "UTC"),
    "open": pl.Int64,
    "high": pl.Int64,
    "low": pl.Int64,
    "close": pl.Int64,
    "volume": pl.Int64,
    "oi": pl.Int64,
}


def to_price_units(rupees: Decimal | int) -> int:
    """Rupees → int64 price units, exact for anything on a 1/100 paise grid (invariant 8)."""
    return int((Decimal(rupees) * PRICE_UNITS).to_integral_value(rounding=ROUND_HALF_EVEN))


def empty_bars() -> pl.DataFrame:
    return pl.DataFrame(schema=SCHEMA)


@dataclass(frozen=True, slots=True)
class CorporateAction:
    ex_date: date
    price_factor: float  # multiply prices before ex_date
    volume_factor: float  # multiply volumes before ex_date


@dataclass(frozen=True, slots=True)
class PartitionWrite:
    path: str  # relative to the lake root
    new_rows: int
    row_count: int
    sha256: str
    from_ts: datetime
    to_ts: datetime


def _dir(source: str, interval: str, instrument_id: int) -> Path:
    return Path("bars") / f"source={source}" / f"interval={interval}" / f"instrument_id={instrument_id}"


def write_bars(
    root: Path, source: str, interval: str, instrument_id: int, bars: pl.DataFrame
) -> list[PartitionWrite]:
    """Merges bars into one file per IST calendar year. A timestamp already stored keeps its stored row.

    Each file is replaced atomically, so a kill at any moment leaves the old file or the new one. A file
    with nothing new is not rewritten, but is still reported: it may be the work of a run killed before
    it could update the catalogue.
    """
    if bars.is_empty():
        return []
    year = pl.col("ts").dt.convert_time_zone(IST_NAME).dt.year().alias("_year")
    out: list[PartitionWrite] = []
    for (y,), part in bars.with_columns(year).partition_by("_year", as_dict=True, include_key=False).items():
        rel = _dir(source, interval, instrument_id) / f"year={y}" / "part-0.parquet"
        path = root / rel
        existing = pl.read_parquet(path) if path.exists() else empty_bars()
        fresh = part.unique("ts", keep="first").join(existing.select("ts"), on="ts", how="anti")
        merged = existing
        if not fresh.is_empty():
            merged = pl.concat([existing, fresh.select(list(SCHEMA))]).sort("ts")
            _make_dirs(path.parent)
            for stale in path.parent.glob(".part-0.*.tmp"):  # left by a killed run
                stale.unlink()
            tmp = path.parent / f".part-0.{os.getpid()}.tmp"
            merged.write_parquet(tmp, compression="zstd", statistics=True)
            _replace(tmp, path)
        else:  # a killed or failed run may have left this file's rename unflushed
            _flush(path)
            _flush(path.parent)
        out.append(
            PartitionWrite(
                path=rel.as_posix(),
                new_rows=fresh.height,
                row_count=merged.height,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                from_ts=merged["ts"][0],
                to_ts=merged["ts"][-1],
            )
        )
    return out


_FULLFSYNC_UNSUPPORTED = (errno.ENOTSUP, errno.EINVAL, errno.ENOTTY)


def _make_dirs(path: Path) -> None:
    """Creates path and flushes each new entry: a recorded chunk's file must survive power loss."""
    new: list[Path] = []
    p = path
    while not p.exists():
        new.append(p)
        p = p.parent
    path.mkdir(parents=True, exist_ok=True)
    for d in new:
        _flush(d.parent)


def _flush(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        if sys.platform == "darwin":  # plain fsync leaves data in the drive's cache
            try:
                fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
                return
            except OSError as e:
                if e.errno not in _FULLFSYNC_UNSUPPORTED:  # a real write failure: never record this chunk
                    raise
                log.warning("F_FULLFSYNC unsupported on this disk, using fsync")
        os.fsync(fd)
    finally:
        os.close(fd)


def _replace(tmp: Path, path: Path) -> None:
    _flush(tmp)
    os.replace(tmp, path)
    _flush(path.parent)


def read_bars(
    root: Path,
    source: str,
    interval: str,
    instrument_id: int,
    *,
    adjusted: bool = False,
    actions: Iterable[CorporateAction] = (),
) -> pl.DataFrame:
    """All stored bars for an instrument, oldest first. With `adjusted`, each bar is multiplied by the
    factors of every action whose ex_date is after the bar's IST date (analytics; rounded to units)."""
    files = sorted((root / _dir(source, interval, instrument_id)).glob("year=*/part-0.parquet"))
    bars = (
        pl.concat([pl.read_parquet(f, hive_partitioning=False) for f in files]).sort("ts")
        if files
        else empty_bars()
    )
    if not adjusted:
        return bars
    day = pl.col("ts").dt.convert_time_zone(IST_NAME).dt.date()
    price = pl.lit(1.0)
    volume = pl.lit(1.0)
    for a in actions:
        price = price * pl.when(day < a.ex_date).then(a.price_factor).otherwise(1.0)
        volume = volume * pl.when(day < a.ex_date).then(a.volume_factor).otherwise(1.0)
    return bars.with_columns(
        *[(pl.col(c) * price).round().cast(pl.Int64) for c in ("open", "high", "low", "close")],
        (pl.col("volume") * volume).round().cast(pl.Int64),
    )
