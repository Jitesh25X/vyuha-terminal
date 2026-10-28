"""Postgres access via asyncpg. Every write job runs in one READ COMMITTED transaction
(audit inserts require it, see migration 000005)."""

from __future__ import annotations

import json
from typing import Any

import asyncpg

Conn = Any  # asyncpg.Connection (asyncpg ships no type hints)


async def connect(dsn: str) -> Conn:
    conn = await asyncpg.connect(dsn, server_settings={"timezone": "UTC", "application_name": "vyuha-quant"})
    await conn.set_type_codec("jsonb", encoder=json.dumps, decoder=json.loads, schema="pg_catalog")
    return conn


async def audit(conn: Conn, action: str, entity: str, entity_id: str | None, payload: dict[str, Any]) -> None:
    """Append a system audit event in the caller's transaction (hash chain is computed by the DB trigger)."""
    await conn.execute(
        "INSERT INTO audit_event (actor_kind, action, entity, entity_id, payload) VALUES ('system', $1, $2, $3, $4)",
        action,
        entity,
        entity_id,
        payload,
    )


async def alert_admins(conn: Conn, severity: str, title: str, body: dict[str, Any]) -> int:
    """Raise an alert to every active admin. Returns how many were notified."""
    status: str = await conn.execute(
        """INSERT INTO alert_event (user_id, severity, title, body)
           SELECT id, $1, $2, $3 FROM app_user WHERE role = 'admin' AND is_active""",
        severity,
        title,
        body,
    )
    return int(status.split()[-1])
