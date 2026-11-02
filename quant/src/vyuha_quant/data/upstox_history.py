"""Upstox V3 historical candles, over the account's own source IP (invariant 4).

Source and limits: see `limits.py`. The access token stays in this object's headers; it never reaches a
log line, an exception message or the job row.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket
from collections.abc import Awaitable, Callable
from datetime import date
from decimal import Decimal
from ipaddress import ip_address
from typing import Any
from urllib.parse import quote

import httpx
from aiolimiter import AsyncLimiter

from .limits import IntervalLimit

log = logging.getLogger("vyuha.download.upstox")

API_BASE = "https://api.upstox.com"
MAX_TRIES = 5
BACKOFF_BASE_S = 1.0


class DownloadError(Exception):
    """A job failure with a stable code. The message is safe to store and print."""

    code = "DOWNLOAD_FAILED"

    def __str__(self) -> str:
        return f"{self.code}: {super().__str__()}"


class TokenExpired(DownloadError):
    code = "BROKER_TOKEN_EXPIRED"


class StaticIPNotOnHost(DownloadError):
    code = "STATIC_IP_NOT_ON_HOST"


class BrokerRejected(DownloadError):
    code = "BROKER_REJECTED"


class RetriesExhausted(DownloadError):
    code = "BROKER_RETRIES_EXHAUSTED"


def preflight_source_ip(ip: str) -> None:
    """Refuses to start when the account's address is not on this host (invariant 4). A wildcard would
    bind successfully but pin nothing: the OS would choose the source address."""
    addr = ip_address(ip)
    if addr.is_unspecified or addr.is_multicast:
        raise StaticIPNotOnHost(f"{ip} does not name one address of this host")
    family = socket.AF_INET6 if addr.version == 6 else socket.AF_INET
    with socket.socket(family, socket.SOCK_STREAM) as s:
        try:
            s.bind((ip, 0))
        except OSError:
            raise StaticIPNotOnHost(f"{ip} is not an address of this host") from None


class UpstoxHistory:
    def __init__(
        self,
        token: str,
        source_ip: str,
        limiter: AsyncLimiter,
        *,
        base_url: str = API_BASE,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    ) -> None:
        self._limiter = limiter
        self._sleep = sleep
        self.requests = 0
        self.rate_limited = 0  # HTTP 429s seen; the pace should keep this at zero
        self._client = httpx.AsyncClient(
            base_url=base_url,
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
            transport=httpx.AsyncHTTPTransport(local_address=source_ip, retries=0),
            trust_env=False,  # environment proxies would leave from another IP
            timeout=30.0,
        )

    async def aclose(self) -> None:
        await self._client.aclose()

    async def candles(self, key: str, limit: IntervalLimit, start: date, end: date) -> list[list[Any]]:
        """Raw candle arrays for [start, end], in whatever order the broker sends them."""
        path = f"/v3/historical-candle/{quote(key, safe='')}/{limit.unit}/{limit.value}/{end}/{start}"
        for attempt in range(MAX_TRIES):
            await self._limiter.acquire()
            self.requests += 1
            try:
                r = await self._client.get(path)
            except httpx.TransportError as e:
                log.warning("upstox request failed: %s (try %d)", type(e).__name__, attempt + 1)
                await self._backoff(attempt, None)
                continue
            if r.status_code == 401:
                raise TokenExpired("the Upstox session has expired: log in again, then re-run this job")
            if r.status_code == 429 or r.status_code >= 500:
                self.rate_limited += r.status_code == 429
                log.warning("upstox HTTP %d (try %d)", r.status_code, attempt + 1)
                await self._backoff(attempt, r.headers.get("retry-after"))
                continue
            if r.status_code != 200:
                raise BrokerRejected(f"HTTP {r.status_code} {_error_code(r.text)} for {key} {start}..{end}")
            # Decimal, not float: prices are money until they are int64 units (invariant 8).
            try:
                candles: list[list[Any]] = json.loads(r.text, parse_float=Decimal)["data"]["candles"]
                if not isinstance(candles, list):
                    raise TypeError
            except (ValueError, KeyError, TypeError):
                raise BrokerRejected(f"unreadable response for {key} {start}..{end}") from None
            return candles
        raise RetriesExhausted(f"{MAX_TRIES} tries failed for {key} {start}..{end}")

    async def _backoff(self, attempt: int, retry_after: str | None) -> None:
        delay = BACKOFF_BASE_S * 2**attempt
        if retry_after and retry_after.isdigit():
            delay = max(delay, float(retry_after))
        await self._sleep(delay)


def _error_code(text: str) -> str:
    """The broker's error code only: a response body is not ours to copy into logs."""
    try:
        code = json.loads(text)["errors"][0]["errorCode"]
    except (ValueError, KeyError, IndexError, TypeError):
        return "(no error code)"
    return str(code)[:32]
