"""Merge Kite and Upstox rows into one canonical instrument per contract (F-MD-1).

Matching:
- tradable instruments (EQ/FUT/OPT): same exchange + exchange-issued token. Both brokers carry the
  exchange token, and within one day's files it is unique per exchange.
- indices (IDX): tokens differ between brokers, so match on the upper-cased name/trading symbol.

When both sides have a row they must agree on the contract terms (segment, expiry, strike,
option type, lot size, tick size); disagreements are reported, and the sync refuses to publish
if there are too many (see sync.MAX_CONFLICT_RATIO).
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field

from .model import CASH_EXCHANGE, Exchange, Instrument, SourceRow


@dataclass(slots=True)
class MergeResult:
    instruments: list[Instrument]
    matched: int = 0
    kite_only: int = 0
    upstox_only: int = 0
    duplicate_indices: int = 0  # indices listed twice under one name (not tradable): first wins
    conflicts: list[str] = field(default_factory=list)


def _terms(r: SourceRow) -> tuple[object, ...]:
    return (r.segment, r.expiry, r.strike, r.option_type, r.lot_size, r.tick_size)


def _index_names(r: SourceRow) -> set[str]:
    names = {r.tradingsymbol.upper()}
    if r.name:
        names.add(r.name.upper())
    return names


def _pairs(
    kite: list[SourceRow], upstox: list[SourceRow]
) -> Iterable[tuple[SourceRow | None, SourceRow | None]]:
    by_token: dict[tuple[str, str], SourceRow] = {}
    by_index_name: dict[tuple[str, str], SourceRow] = {}
    for row in upstox:
        if row.segment == "IDX":
            for n in _index_names(row):
                by_index_name.setdefault((row.exchange, n), row)
        else:
            by_token[(row.exchange, row.exchange_token)] = row
    used: set[str] = set()
    for k in kite:
        u: SourceRow | None
        if k.segment == "IDX":
            u = by_index_name.get((k.exchange, k.tradingsymbol.upper()))
        else:
            u = by_token.get((k.exchange, k.exchange_token))
        if u is not None and u.broker_id in used:
            u = None  # each upstox row pairs at most once
        if u is not None:
            used.add(u.broker_id)
        yield k, u
    for row in upstox:
        if row.broker_id not in used:
            yield None, row


def merge(kite: list[SourceRow], upstox: list[SourceRow]) -> MergeResult:
    res = MergeResult(instruments=[])
    merged: list[tuple[SourceRow | None, SourceRow | None]] = []
    seen: set[tuple[str, str]] = set()
    for k, u in _pairs(kite, upstox):
        primary = k or u
        assert primary is not None
        if k and u:
            if _terms(k) != _terms(u):
                res.conflicts.append(f"{k.exchange}:{k.tradingsymbol} kite {_terms(k)} != upstox {_terms(u)}")
            res.matched += 1
        elif k:
            res.kite_only += 1
        else:
            res.upstox_only += 1
        key = (primary.exchange, primary.tradingsymbol)
        if key in seen:
            if primary.segment == "IDX":
                res.duplicate_indices += 1
            else:
                res.conflicts.append(f"{key[0]}:{key[1]} appears twice")
            continue
        seen.add(key)
        merged.append((k, u))

    # Underlyings: Upstox names them by instrument_key; Kite-only derivatives borrow the key that
    # Upstox uses for the same underlying symbol, else fall back to the cash row with that symbol.
    by_upstox_key: dict[str, tuple[Exchange, str]] = {}
    cash_by_symbol: dict[tuple[str, str], tuple[Exchange, str]] = {}
    key_by_symbol: dict[tuple[str, str], str] = {}
    for k, u in merged:
        p = k or u
        assert p is not None
        ident = (p.exchange, p.tradingsymbol)
        if u is not None:
            by_upstox_key[u.broker_id] = ident
            if u.underlying_key:
                key_by_symbol.setdefault((u.exchange, u.symbol), u.underlying_key)
        if p.segment == "EQ" and p.tradingsymbol == p.symbol:
            cash_by_symbol.setdefault((p.exchange, p.symbol), ident)

    for k, u in merged:
        p = k or u
        assert p is not None
        underlying: tuple[Exchange, str] | None = None
        if p.segment in ("FUT", "OPT"):
            ukey = (u.underlying_key if u else None) or key_by_symbol.get((p.exchange, p.symbol))
            underlying = by_upstox_key.get(ukey) if ukey else None
            if underlying is None:
                underlying = cash_by_symbol.get((CASH_EXCHANGE[p.exchange], p.symbol))
        res.instruments.append(
            Instrument(
                exchange=p.exchange,
                segment=p.segment,
                symbol=p.symbol,
                tradingsymbol=p.tradingsymbol,
                name=(u.name if u and u.name else None) or (k.name if k else None),
                isin=u.isin if u else None,
                expiry=p.expiry,
                strike=p.strike,
                option_type=p.option_type,
                lot_size=p.lot_size,
                tick_size=p.tick_size,
                freeze_qty=u.freeze_qty if u else None,
                kite_token=int(k.broker_id) if k else None,
                upstox_key=u.broker_id if u else None,
                underlying=underlying,
            )
        )
    return res
