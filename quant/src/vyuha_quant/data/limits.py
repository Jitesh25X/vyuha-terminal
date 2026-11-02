"""Broker historical-data limits. Every entry names its source and the date it was checked."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date

UPSTOX_HISTORY_DOC = "https://upstox.com/developer/api-documentation/v3/get-historical-candle-data/"
UPSTOX_RATE_DOC = "https://upstox.com/developer/api-documentation/rate-limiting/"
VERIFIED = date(2026, 11, 2)

HEADROOM = 0.8  # documented limits minus 20 % (STRATEGY_SDK_AND_BACKTEST §1)


@dataclass(frozen=True, slots=True)
class IntervalLimit:
    unit: str  # path segment of the V3 URL
    value: int
    max_months: int  # longest range one request may cover
    earliest: date  # the broker has nothing before this
    source: str = UPSTOX_HISTORY_DOC
    verified: date = VERIFIED


@dataclass(frozen=True, slots=True)
class RateLimit:
    per_second: int
    per_minute: int
    per_30_minutes: int
    source: str = UPSTOX_RATE_DOC
    verified: date = VERIFIED


UPSTOX_INTERVALS: dict[str, IntervalLimit] = {
    "1minute": IntervalLimit("minutes", 1, max_months=1, earliest=date(2022, 1, 1)),
    "day": IntervalLimit("days", 1, max_months=120, earliest=date(2000, 1, 1)),
}

# "Standard APIs (… historical candles …)": limits apply per API, per user.
UPSTOX_RATE = RateLimit(per_second=50, per_minute=500, per_30_minutes=2000)


def pace_seconds(rate: RateLimit, headroom: float = HEADROOM) -> float:
    """Seconds between requests so that every window stays under `headroom` of its limit.

    An even pace rather than one token bucket per window: a bucket allows its full burst and then its
    full refill inside one window, so a 1,600-per-30-minutes bucket can send 3,200 in 30 minutes, over
    the 2,000 limit. A job is sequential anyway, so the burst would buy nothing.
    """
    return max(
        1 / (rate.per_second * headroom),
        60 / (rate.per_minute * headroom),
        1800 / (rate.per_30_minutes * headroom),
    )
