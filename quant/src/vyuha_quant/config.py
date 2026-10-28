"""Settings from the environment (see .env.example). Same variable names as the gateway."""

from __future__ import annotations

import os
from dataclasses import dataclass
from datetime import date, datetime
from zoneinfo import ZoneInfo

IST = ZoneInfo("Asia/Kolkata")

# Matches deploy/docker-compose.dev.yml; used only when VYUHA_ENV=dev.
_DEV_POSTGRES_URL = "postgres://vyuha:vyuha@localhost:5432/vyuha?sslmode=disable"


@dataclass(frozen=True, slots=True)
class Settings:
    env: str
    postgres_url: str

    def __repr__(self) -> str:  # never print credentials
        return f"Settings(env={self.env!r}, postgres=<redacted>)"


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
    return Settings(env=env, postgres_url=url)


def today_ist(now: datetime | None = None) -> date:
    """The current trading date: wall clock in IST (storage stays UTC, invariant 9)."""
    return (now or datetime.now(IST)).astimezone(IST).date()
