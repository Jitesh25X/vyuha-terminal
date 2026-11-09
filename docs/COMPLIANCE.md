# Compliance Notes & Controls

> **Not legal advice.** This file maps publicly reported regulatory requirements to engineering controls so the software doesn't get in the way of compliance. The desk owner must confirm the legal position with a lawyer or compliance professional familiar with SEBI/exchange rules **before** going live, especially for any multi-person or profit-sharing arrangement. Rules and broker implementations change, so re-verify at each milestone that touches live trading.

## 1. Regulatory background (as researched 2026-10-27)
- SEBI circular "Safer participation of retail investors in Algorithmic trading" (Feb 2025; ref SEBI/HO/MIRSD/MIRSD-PoD/P/2025/0000013). Exchanges published implementation standards (NSE circular NSE/INVG/67858 is referenced by Upstox). The framework has been fully applicable to all brokers since **1 April 2026**.
- Brokers are principals; algo providers are agents and must be empanelled with the exchange.
- API access needs a client-specific API key, a whitelisted static IP and 2FA/OAuth. Open/unrestricted APIs are prohibited.
- Static IP: one primary (plus an optional backup) per client. Changes are allowed about once a week except in emergencies. **IPs may be shared only among family members** (as defined by SEBI), with consent.
- Orders-per-second: algos at or below the exchange threshold (currently reported as **10 OPS per exchange**) don't need exchange registration. Above it, the strategy must be registered through the broker.
- Orders must be traceable (an algo ID is tagged by the broker). Brokers keep logs for at least 5 years.
- Upstox (from 1 Apr 2026): only **one active API app per user**; static IP mandatory for order endpoints (place/modify/cancel, multi-order, GTT); market protection applies to MARKET/SL-M orders.
- Zerodha Kite Connect: the Personal plan (free) covers orders and portfolio but has no live or historical data. The Connect plan (₹500/app/month) adds WebSocket and historical data. Limits are per API key, e.g. 3 WebSocket connections × 3,000 instruments; about 10 orders/s.
- Black-box algos (logic not disclosed) offered by providers need the provider to be a SEBI Research Analyst. White-box algos don't.

Sources: [SEBI framework summary — QuantInsti](https://www.quantinsti.com/articles/algorithmic-trading-india/), [NSE rules summary — Marketcalls](https://www.marketcalls.in/market-regulations/new-nse-rules-for-retail-algo-trading-what-retail-traders-algo-platforms-and-brokers-must-now-do.html), [Upstox mandates from 1 Apr 2026](https://community.upstox.com/t/important-new-sebi-exchange-mandates-for-api-trading-effective-1st-april-2026/14822), [Zerodha Kite API charges](https://support.zerodha.com/category/trading-and-markets/general-kite/kite-api/articles/what-are-the-charges-for-kite-apis), [Kite WebSocket limits](https://kite.trade/forum/discussion/comment/51590/), [Angel One SmartAPI changes](https://www.angelone.in/news/market-updates/what-s-changing-in-angel-one-s-smartapi-access-from-april-1-2026), [Antsheel 2026 summary](https://antsheel.com/blog/sebi-retail-algo-trading-rules-2026-india/).

## 2. Requirement → control matrix
| Requirement | Vyuha control | Where | Test |
|---|---|---|---|
| Client-specific API key | One `broker_account` per client with its own encrypted key/secret; Upstox "one app per user" respected | DB, admin | Admin UI rejects a duplicate key across accounts |
| Static IP per client, no sharing across unrelated clients | Per-account `static_ip` binding; unique `public_static_ip`; preflight check | engine §5 | `STATIC_IP_NOT_ON_HOST` test; DB unique index |
| 2FA | TOTP for Vyuha users; broker's own 2FA in daily login; no broker-TOTP automation | gateway | Auth tests |
| ≤ 10 OPS unless registered | Per account per exchange, default 8, hard max 10 (DB CHECK). No rolling second holds more than the cap. The budgets run on a monotonic clock, so a wall-clock step can't release tokens (D-25), and staging and production cap NTP slew at 500 ppm so a clock-second stays within 0.05% of a real one (D-36). Checked at the gate and again at send, where cancels count too (D-20) | engine R-OPS, `OrderRouter` send budget | `SendBudget.*`, `OpsWindow.*`, `ROps.*`: a burst leaves at ≤ cap in any second |
| Traceability | Strategy tag on every order (`manual`, `strat:<id>`, `ai-draft`); `order_event` log with gate stamp | engine, DB | Tag coverage = 100% in `COMP` report |
| Kill switch / control | Multi-scope kill switch; broker-side kill switch documented | engine | Kill tests |
| Logs ≥ 5 years | Hash-chained `audit_event` + orders/events/fills retention | DB | `vyuhactl audit verify [-anchor id:hash]`; keep `vyuhactl audit head` output off-database |
| Restricted order types/instruments | Gate rules `R-ORDER-SHAPE`, `R-INSTR-*`; MARKET disable switch; market protection param | engine | Unit tests |
| Daily session expiry | Accounts → `awaiting_login`; trading blocked until the human logs in | engine/gateway | Token-expiry test |
| White-box only | Strategies are code the desk owns and can read; AI never trades autonomously | design | Tool registry test |

## 3. Open legal questions for the owner (block live pilot until answered)
1. **Desk structure:** are traders trading *their own* accounts, or the desk's capital? If the desk's capital sits in someone else's account, who is legally allowed to place orders on it? Does any profit-sharing arrangement need registration?
2. **Operator role:** if the desk operator hosts Vyuha and others trade through it, is the operator an "algo provider" that must be empanelled? Does it change anything that the software is open source and self-hosted per account holder?
3. **Strategy registration:** the defaults keep every account below the OPS threshold. Confirm with each broker how they tag client-generated algo orders (algo ID).
4. **Market data:** may a market-data feed obtained under one person's broker API be displayed to other desk members? Default: no. Data is entitled per mapped account (PRD §13 #4). A licensed vendor is the clean path for a shared desk feed.
5. **Personal data:** desk users' personal data (names, client codes, PAN if ever stored — **don't store PAN**) falls under India's Digital Personal Data Protection Act, 2023. Store the minimum and document a privacy notice.
6. **LLM processing:** sending position data to an external LLM provider. Default: aggregated tickers, quantities and Greeks only, with no client codes or names, and a local-model option.

## 4. Data licensing & ToS
- Broker market data: personal/non-redistribution terms. Entitlement enforced per user (`R-ENTITLE`, `MD_NOT_ENTITLED`).
- Exchange websites: automated scraping may breach site terms. `feed_source.kind='exchange'` cannot be enabled until `tos_reviewed=true` is set by an admin, with a note.
- News RSS: store the title, summary and link. Store full text only where the source permits it. Always link back to the original.
- TradingView Lightweight Charts: Apache-2.0, and the attribution notice must be displayed (it's included in `SYS` → About and the chart corner per their requirement).
- Neo4j Community: GPLv3, used as a separate service (not linked).

## 5. Open-source release hygiene
- License: AGPL-3.0 recommended (PRD §13 #3). Add `LICENSE`, `NOTICE` (third-party), `SECURITY.md` (vuln reporting), and `DISCLAIMER.md` ("not investment advice; software provided as is; users responsible for regulatory compliance").
- No broker credentials, IPs or personal data in the repo. Secrets scanning (gitleaks) runs in CI and as a pre-commit hook.
- Don't use the names or logos "Bloomberg" or "Aladdin" in the product UI or branding. Docs may reference them only as public comparisons.
