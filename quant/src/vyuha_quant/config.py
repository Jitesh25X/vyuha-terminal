"""Settings from the environment (see env.example). Same variable names as the gateway."""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import date, datetime
from pathlib import Path
from urllib.parse import urlsplit
from zoneinfo import ZoneInfo

IST = ZoneInfo("Asia/Kolkata")

# Matches deploy/docker-compose.dev.yml; used only when VYUHA_ENV=dev.
_DEV_POSTGRES_URL = "postgres://vyuha:vyuha@localhost:5432/vyuha?sslmode=disable"


# RBI policy repo rate, 5.50 % since the MPC decision of 2026-10-07 (verified 2026-10-29). RISK_ENGINE
# §6.1: the admin sets RISK_FREE_RATE and RISK_FREE_RATE_AS_OF together (the OC panel shows the date, so
# a stale rate is visible); update these defaults when the repo rate changes.
DEFAULT_RISK_FREE_RATE = 0.055
DEFAULT_RISK_FREE_RATE_AS_OF = date(2026, 10, 7)


@dataclass(frozen=True, slots=True)
class Settings:
    env: str
    postgres_url: str
    risk_free_rate: float = DEFAULT_RISK_FREE_RATE
    risk_free_rate_as_of: date = DEFAULT_RISK_FREE_RATE_AS_OF
    http_addr: str = "127.0.0.1:8090"  # internal API (gateway → quant); loopback unless told otherwise
    iv_mid_max_spread: float = 0.10  # IV from the mid only when spread ≤ this fraction of the mid
    iv_ltp_max_age_s: float = 60.0  # else from the LTP only when it traded this recently
    data_lake_dir: Path = Path("./data/lake")
    upstox_api_base: str = "https://api.upstox.com"  # overridable in dev only, for a local fake server
    download_pace_s: float | None = None  # dev only; default comes from data/limits.py

    def __repr__(self) -> str:  # never print credentials
        return (
            f"Settings(env={self.env!r}, postgres=<redacted>, risk_free_rate={self.risk_free_rate!r}, "
            f"risk_free_rate_as_of={self.risk_free_rate_as_of!r}, http_addr={self.http_addr!r}, "
            f"iv_mid_max_spread={self.iv_mid_max_spread!r}, iv_ltp_max_age_s={self.iv_ltp_max_age_s!r}, "
            f"data_lake_dir={str(self.data_lake_dir)!r})"
        )


def load(environ: dict[str, str] | None = None) -> Settings:
    e = os.environ if environ is None else environ
    env = e.get("VYUHA_ENV", "dev").strip() or "dev"
    if env not in ("dev", "staging", "prod"):
        raise ValueError(f"VYUHA_ENV must be dev, staging or prod, got {env!r}")
    url = e.get("POSTGRES_URL", "").strip()
    if not url:
        if env != "dev":
            raise ValueError("POSTGRES_URL is required outside dev")
        url = _DEV_POSTGRES_URL
    raw_rate = e.get("RISK_FREE_RATE", "").strip()
    rate = _float(e, "RISK_FREE_RATE", DEFAULT_RISK_FREE_RATE)
    if not -0.05 <= rate <= 0.5:
        raise ValueError(f"RISK_FREE_RATE is a decimal (0.055 = 5.5 %), got {raw_rate!r}")
    raw_as_of = e.get("RISK_FREE_RATE_AS_OF", "").strip()
    if raw_rate and not raw_as_of:
        raise ValueError("RISK_FREE_RATE_AS_OF (YYYY-MM-DD) is required with RISK_FREE_RATE")
    as_of = date.fromisoformat(raw_as_of) if raw_as_of else DEFAULT_RISK_FREE_RATE_AS_OF
    spread = _float(e, "IV_MID_MAX_SPREAD", 0.10)
    if not 0 < spread <= 1:
        raise ValueError(f"IV_MID_MAX_SPREAD is a fraction of the mid in (0, 1], got {spread!r}")
    age = _float(e, "IV_LTP_MAX_AGE_S", 60.0)
    if not 0 < age <= 3600:
        raise ValueError(f"IV_LTP_MAX_AGE_S must be in (0, 3600] seconds, got {age!r}")
    addr = e.get("QUANT_HTTP_ADDR", "").strip() or "127.0.0.1:8090"
    base = e.get("UPSTOX_API_BASE", "").strip()
    raw_pace = e.get("DOWNLOAD_PACE_S", "").strip()
    if env != "dev" and (base or raw_pace):
        raise ValueError("UPSTOX_API_BASE and DOWNLOAD_PACE_S are for dev only")
    if raw_pace and not base:
        raise ValueError(
            "DOWNLOAD_PACE_S needs UPSTOX_API_BASE: the real API always gets the documented pace"
        )
    if base and urlsplit(base).hostname not in ("127.0.0.1", "localhost"):
        raise ValueError("UPSTOX_API_BASE must be a loopback URL: it receives the access token")
    return Settings(
        env=env,
        postgres_url=url,
        risk_free_rate=rate,
        risk_free_rate_as_of=as_of,
        http_addr=addr,
        iv_mid_max_spread=spread,
        iv_ltp_max_age_s=age,
        data_lake_dir=Path(e.get("DATA_LAKE_DIR", "").strip() or "./data/lake"),
        upstox_api_base=base or "https://api.upstox.com",
        download_pace_s=float(raw_pace) if raw_pace else None,
    )


def _float(e: Mapping[str, str], key: str, default: float) -> float:
    raw = e.get(key, "").strip()
    try:
        return float(raw) if raw else default
    except ValueError:
        raise ValueError(f"{key} must be a number, got {raw!r}") from None


def today_ist(now: datetime | None = None) -> date:
    """The current trading date: wall clock in IST (storage stays UTC, invariant 9)."""
    return (now or datetime.now(IST)).astimezone(IST).date()
