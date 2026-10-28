"""Daily instrument master sync (F-MD-1), scheduled 08:00 IST.

Both brokers' files are required. Any failure (download, format, too many conflicts, a suspicious
drop in listed instruments, a DB error) rolls the whole sync back, so yesterday's master stays
active, and raises a critical admin alert. Trading is never blocked by a failed sync.
"""

from __future__ import annotations

import gzip
import json
import logging
from collections.abc import Awaitable, Callable
from dataclasses import asdict, dataclass
from datetime import date
from pathlib import Path
from typing import Any

import httpx

from ...db import alert_admins, audit, connect
from . import kite, store, upstox
from .merge import merge
from .model import SourceRow

log = logging.getLogger(__name__)

# Refuse to publish when more than this share of matched rows disagree between brokers.
MAX_CONFLICT_RATIO = 0.001
# Refuse to publish when the sync would close more than this share of active instruments
# (a monthly expiry closes ~15%; a truncated file would close far more).
MAX_CLOSE_RATIO = 0.40
USER_AGENT = "vyuha-quant/0.1 (instrument master sync)"


class SyncRejected(RuntimeError):
    """The new master failed a sanity check; nothing was written."""


@dataclass(frozen=True, slots=True)
class Files:
    """Raw source files: kite CSV text per exchange, upstox JSON arrays per file."""

    kite: dict[str, str]
    upstox: dict[str, list[Any]]


Fetcher = Callable[[], Awaitable[Files]]


async def download(client: httpx.AsyncClient | None = None) -> Files:
    """Fetch today's files. Both are public dumps regenerated before market open (no credentials)."""
    own = client is None
    c = client or httpx.AsyncClient(timeout=60, headers={"User-Agent": USER_AGENT}, follow_redirects=True)
    try:
        kite_files: dict[str, str] = {}
        for ex in kite.EXCHANGES:
            r = await c.get(kite.URL.format(exchange=ex))
            r.raise_for_status()
            kite_files[ex] = r.text
        upstox_files: dict[str, list[Any]] = {}
        for f in upstox.FILES:
            r = await c.get(upstox.URL.format(file=f))
            r.raise_for_status()
            upstox_files[f] = _json_array(r.content, f)
        return Files(kite=kite_files, upstox=upstox_files)
    finally:
        if own:
            await c.aclose()


def load_dir(path: Path) -> Files:
    """Read files saved by hand: kite_<EXCHANGE>.csv and upstox_<FILE>.json[.gz] (manual re-run, tests)."""
    kite_files: dict[str, str] = {ex: (path / f"kite_{ex}.csv").read_text() for ex in kite.EXCHANGES}
    upstox_files: dict[str, list[Any]] = {}
    for f in upstox.FILES:
        p = path / f"upstox_{f}.json.gz"
        if not p.exists():
            p = path / f"upstox_{f}.json"
        upstox_files[f] = _json_array(p.read_bytes(), f)
    return Files(kite=kite_files, upstox=upstox_files)


def _json_array(content: bytes, name: str) -> list[Any]:
    if content[:2] == b"\x1f\x8b":
        content = gzip.decompress(content)
    data = json.loads(content)
    if not isinstance(data, list):
        raise upstox.UpstoxFormatError(f"upstox {name}: expected a JSON array")
    return data


def parse(files: Files) -> tuple[list[SourceRow], list[SourceRow]]:
    k: list[SourceRow] = []
    for ex in kite.EXCHANGES:
        rows = kite.parse(files.kite.get(ex, ""))
        if not rows:
            raise SyncRejected(f"kite {ex}: no instruments in file")
        k.extend(rows)
    u: list[SourceRow] = []
    for f in upstox.FILES:
        rows = upstox.parse(files.upstox.get(f, []))
        if not rows:
            raise SyncRejected(f"upstox {f}: no instruments in file")
        u.extend(rows)
    return k, u


async def run(dsn: str, day: date, fetch: Fetcher, *, allow_large_change: bool = False) -> dict[str, Any]:
    """Sync the master for `day`. Returns a report; raises after alerting admins on failure."""
    try:
        files = await fetch()
        k, u = parse(files)
        m = merge(k, u)
        if m.conflicts and len(m.conflicts) > max(1, int(m.matched * MAX_CONFLICT_RATIO)):
            raise SyncRejected(f"{len(m.conflicts)} kite/upstox conflicts, e.g. {m.conflicts[:3]}")
        conn = await connect(dsn)
        try:
            async with conn.transaction():
                stats = await store.apply(conn, m.instruments, day)
                if (
                    not allow_large_change
                    and stats.active_before > 0
                    and stats.closed > stats.active_before * MAX_CLOSE_RATIO
                ):
                    raise SyncRejected(
                        f"would close {stats.closed} of {stats.active_before} active instruments "
                        f"(> {MAX_CLOSE_RATIO:.0%}); re-run with --allow-large-change after checking the files"
                    )
                ban_date, banned = await store.apply_fo_ban(conn, day)
                report: dict[str, Any] = {
                    "date": day.isoformat(),
                    "kite_rows": len(k),
                    "upstox_rows": len(u),
                    "matched": m.matched,
                    "kite_only": m.kite_only,
                    "upstox_only": m.upstox_only,
                    "duplicate_indices": m.duplicate_indices,
                    "conflicts": len(m.conflicts),
                    "partial_mapping": sum(i.partial_mapping for i in m.instruments),
                    **asdict(stats),
                    "fo_ban_list": ban_date.isoformat() if ban_date else None,
                    "fo_ban_contracts": banned,
                }
                await audit(conn, "instrument.sync", "instrument", day.isoformat(), report)
        finally:
            await conn.close()
    except Exception as e:
        log.exception("instrument sync failed; yesterday's master stays active")
        await _alert_failure(dsn, day, e)
        raise
    for c in m.conflicts[:20]:
        log.warning("conflict: %s", c)
    log.info("instrument sync done", extra={"report": report})
    return report


async def _alert_failure(dsn: str, day: date, err: Exception) -> None:
    try:
        conn = await connect(dsn)
        try:
            n = await alert_admins(
                conn,
                "critical",
                "Instrument master sync failed",
                {
                    "date": day.isoformat(),
                    "error": type(err).__name__,
                    "detail": str(err)[:500],
                    "effect": "previous master stays active; trading not blocked",
                },
            )
            if n == 0:
                log.error("no active admin to alert")
        finally:
            await conn.close()
    except Exception:
        log.exception("could not raise the admin alert")
