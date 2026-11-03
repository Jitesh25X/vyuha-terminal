"""Shared fixtures. Integration tests get a fresh database migrated from db/migrations and
dropped afterwards (like gateway/internal/testdb); they skip unless VYUHA_TEST_POSTGRES_URL is set."""

from __future__ import annotations

import asyncio
import os
import secrets
from collections.abc import Iterator
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit

import asyncpg
import pytest

from vyuha_quant.data.instruments import sync

FIXTURES = Path(__file__).parent / "fixtures"
MIGRATIONS = Path(__file__).parents[2] / "db" / "migrations"
# Same key as gateway/internal/testdb: 000001 creates a cluster-wide role, so migrate one DB at a time.
MIGRATE_LOCK_KEY = 0x76797568617464


async def _create(admin: str, name: str, url: str) -> None:
    conn = await asyncpg.connect(admin)
    try:
        await conn.execute(f'CREATE DATABASE "{name}"')
        await conn.execute("SELECT pg_advisory_lock($1)", MIGRATE_LOCK_KEY)
        try:
            db = await asyncpg.connect(url)
            try:
                for f in sorted(MIGRATIONS.glob("*.up.sql")):
                    await db.execute(f.read_text())
            finally:
                await db.close()
        finally:
            await conn.execute("SELECT pg_advisory_unlock($1)", MIGRATE_LOCK_KEY)
    finally:
        await conn.close()


async def _drop(admin: str, name: str) -> None:
    conn = await asyncpg.connect(admin)
    try:
        await conn.execute(f'DROP DATABASE IF EXISTS "{name}" WITH (FORCE)')
    finally:
        await conn.close()


def skip_without_service(message: str) -> None:
    """Skips, for quick local runs. With VYUHA_REQUIRE_DB=1 (CI and `make verify`) fails: down can't look like a pass."""
    if os.environ.get("VYUHA_REQUIRE_DB") == "1":
        pytest.fail(f"{message} and VYUHA_REQUIRE_DB=1: it must not skip")
    pytest.skip(message)


@pytest.fixture
def dsn() -> Iterator[str]:
    admin = os.environ.get("VYUHA_TEST_POSTGRES_URL")
    if not admin:
        skip_without_service("VYUHA_TEST_POSTGRES_URL not set")
    name = "vyuha_qtest_" + secrets.token_hex(6)
    parts = urlsplit(admin)
    url = urlunsplit(parts._replace(path="/" + name))
    asyncio.run(_create(admin, name, url))
    try:
        yield url
    finally:
        asyncio.run(_drop(admin, name))


@pytest.fixture
def day1() -> sync.Files:
    return sync.load_dir(FIXTURES / "day1")
