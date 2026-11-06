"""`vyuha-quant` command line. Jobs log JSON lines to stderr and print a JSON report to stdout."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
from datetime import date
from pathlib import Path
from typing import Any

from . import config
from .data import banlist, download, holidays
from .data.instruments import sync

EXIT_INCOMPLETE = 3  # a download finished with chunks it couldn't record: partial, never a success


class _JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        out: dict[str, Any] = {
            "level": record.levelname.lower(),
            "logger": record.name,
            "msg": record.getMessage(),
        }
        if hasattr(record, "report"):
            out["report"] = record.report
        if record.exc_info:
            out["error"] = self.formatException(record.exc_info).splitlines()[-1]
        return json.dumps(out, default=str)


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="vyuha-quant")
    sub = p.add_subparsers(dest="cmd", required=True)

    ins = sub.add_parser("instruments", help="instrument master").add_subparsers(dest="action", required=True)
    s = ins.add_parser("sync", help="download Kite + Upstox files and update the master (F-MD-1)")
    s.add_argument("--date", type=date.fromisoformat, help="sync date (default: today IST)")
    s.add_argument("--from-dir", type=Path, help="use saved files (kite_<EX>.csv, upstox_<F>.json[.gz])")
    s.add_argument(
        "--allow-large-change", action="store_true", help="skip the mass-close guard (after review)"
    )

    ban = sub.add_parser("ban", help="NSE F&O ban list (manual import, ADR-019)").add_subparsers(
        dest="action", required=True
    )
    bi = ban.add_parser("import", help="apply a downloaded fo_secban.csv")
    bi.add_argument("file", type=Path)
    bi.add_argument(
        "--date", type=date.fromisoformat, help="trade date the file must carry (default: today IST)"
    )
    bc = ban.add_parser("check", help="alert admins if today's list is missing")
    bc.add_argument("--date", type=date.fromisoformat)

    hol = sub.add_parser("holidays", help="exchange holidays").add_subparsers(dest="action", required=True)
    hol.add_parser("seed", help="seed exchange_holiday from exchange_calendars (XBOM)")

    dl = sub.add_parser("download", help="historical data jobs (F-RS-1)").add_subparsers(
        dest="action", required=True
    )
    pl = dl.add_parser("plan", help="what a job would fetch; no token, no broker request")
    pl.add_argument("--spec", type=Path, required=True)
    dr = dl.add_parser("run", help="run or resume a job; started by `vyuhactl download run`, token on stdin")
    dr.add_argument("--spec", type=Path, required=True)
    dr.add_argument("--account", required=True, help="broker account id")
    dr.add_argument("--user", required=True, help="username requesting the job")
    kg = dl.add_parser(
        "known-gap", help="record that one instrument really has no bars on one day (admin, audited)"
    )
    kg.add_argument("--symbol", required=True, help="e.g. NSE:RELIANCE")
    kg.add_argument("--day", type=date.fromisoformat, required=True)
    kg.add_argument("--reason", required=True)
    kg.add_argument("--user", required=True, help="the admin recording it")

    sub.add_parser(
        "serve", help="internal HTTP API for the gateway (QUANT_HTTP_ADDR, default 127.0.0.1:8090)"
    )
    return p


async def _run(a: argparse.Namespace, settings: config.Settings) -> dict[str, Any]:
    day: date = getattr(a, "date", None) or config.today_ist()
    dsn = settings.postgres_url
    match (a.cmd, a.action):
        case ("instruments", "sync"):
            src: Path | None = a.from_dir

            async def fetch() -> sync.Files:
                return sync.load_dir(src) if src else await sync.download()

            return await sync.run(dsn, day, fetch, allow_large_change=a.allow_large_change)
        case ("ban", "import"):
            return await banlist.import_file(dsn, a.file.read_bytes(), day)
        case ("ban", "check"):
            latest = await banlist.check(dsn, day)
            return {"trade_date": day.isoformat(), "applied_list": latest.isoformat() if latest else None,
                    "up_to_date": latest == day}  # fmt: skip
        case ("holidays", "seed"):
            return await holidays.seed(dsn, day)
        case ("download", "plan"):
            spec = download.JobSpec.model_validate_json(a.spec.read_text())
            return await download.plan_report(dsn, spec, a.spec.resolve().parent)
        case ("download", "known-gap"):
            return await download.add_known_gap(dsn, a.symbol, a.day, a.reason, a.user)
        case ("download", "run"):
            spec = download.JobSpec.model_validate_json(a.spec.read_text())
            token = sys.stdin.readline().strip()  # never argv or the environment
            if not token:
                raise ValueError("no access token on stdin: start jobs with `vyuhactl download run`")
            logging.getLogger("httpx").setLevel(logging.WARNING)  # one INFO line per request
            return await download.run(
                dsn,
                spec,
                a.spec.resolve().parent,
                account=a.account,
                username=a.user,
                token=token,
                lake_dir=settings.data_lake_dir,
                base_url=settings.upstox_api_base,
                pace=settings.download_pace_s,
            )
    raise AssertionError(a)


def _serve(settings: config.Settings) -> int:
    """Runs the internal API until interrupted (a long-running service, not a job: no JSON report)."""
    import uvicorn

    from .api.app import create_app

    host, _, port = settings.http_addr.rpartition(":")
    uvicorn.run(
        create_app(settings), host=host.strip("[]"), port=int(port), log_config=None, access_log=False
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(_JsonFormatter())
    logging.basicConfig(level=logging.INFO, handlers=[handler])
    a = _parser().parse_args(argv)
    if a.cmd == "serve":
        return _serve(config.load())
    try:
        report = asyncio.run(_run(a, config.load()))
    except Exception as e:
        print(json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"}))
        return 1
    if report.get("chunks_incomplete"):
        print(json.dumps({"ok": False, **report}, default=str))
        return EXIT_INCOMPLETE
    print(json.dumps({"ok": True, **report}, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
