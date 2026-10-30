# Kite Connect adapter — market data (Kite Ticker)

Order placement, modify and cancel arrive in M3 and accept only `risk::ApprovedOrder` (invariant 1).
Nothing here talks to live Kite in tests (invariant 3): tests use fixtures and a local WebSocket server.

## Sources (verified 2026-10-29)
| What | URL |
|---|---|
| WebSocket protocol, packet layouts, limits | https://kite.trade/docs/connect/v3/websocket/ |
| Byte order (big-endian), token → segment (`token & 0xff`), price divisors, reconnect constants | https://github.com/zerodha/pykiteconnect/blob/master/kiteconnect/ticker.py |
| Instrument dump (tokens) | https://kite.trade/docs/connect/v3/market-quotes/#instruments |

## Limits
| Limit | Value | Enforced in |
|---|---|---|
| Instruments per WebSocket connection | 3 000 | `plan_shards` (`TickerLimits::max_per_connection`) |
| WebSocket connections per API key | 3 | `plan_shards` (`TickerLimits::max_connections`) |
| Heartbeat when idle | ~1 byte every couple of seconds | idle timeout 10 s → reconnect |
| Reconnect | ours: 0.5 s doubling to 30 s, equal jitter (F-MD-2) | `Backoff` |

## Protocol notes
- URL: `wss://ws.kite.trade?api_key=…&access_token=…`. The access token is a `SecretRef`; the URL is
  never logged and the copy used for the handshake is zeroed afterwards.
- Frame: `u16 count`, then `count × (u16 len, packet)`; < 2 bytes = heartbeat. All integers big-endian.
- Packet lengths: 8 LTP; 28/32 index quote/full (H, L, O, C order differs from tradables); 44 quote;
  184 full (OI, exchange time, 5×bid + 5×ask depth of `qty u32, price u32, orders u16, 2 pad`).
- Prices: paise for NSE, NFO, BSE, BFO and indices → `Price::from_paise`. Currency (÷10⁷) and
  commodity segments are rejected (`unsupported_segment`); we don't trade them.
- LTP/quote packets carry no exchange time: `exchange_ts = recv_ts` for those.
- Full packets carry the last-traded timestamp at bytes 44–48 (seconds; pykiteconnect's
  `last_trade_time`) → `Tick::last_trade_ts`. Quote packets have none: the core's `TradeClock` infers it
  from this feed's cumulative volume rising (M1.11b).
- Text frames are JSON `{type: order|error|message, data}`. `order` postbacks go to the OMS from M3;
  errors/messages are logged (kind only).
- Requests: `{"a":"subscribe","v":[tokens]}` then `{"a":"mode","v":["ltp|quote|full",[tokens]]}`; the
  same set is replayed after every reconnect.

## Static IP (invariant 4)
`start()` fails with `kStaticIpNotOnHost` unless `source_ip` is on a local interface, and every TCP
socket binds to it before `connect`.

## Tests / fixtures / fuzz
- `fixtures/vcr/kite/*.bin` are **synthesized** from the documented layout by `gen_frames.py` (no live
  session yet). Replace with recorded, scrubbed frames when a session is available.
- `tests/kite_parser_test.cc` (layouts, truncation), `tests/kite_ticker_test.cc` (reconnect,
  resubscribe, gap log, preflight) against a local server.
- `fuzz/kite_ticker_fuzz.cc`: `make fuzz-kite` (10 min by default). macOS runs libFuzzer + UBSan +
  libc++ hardening (LLVM 18 ASan hangs with libFuzzer there); CI on Ubuntu adds ASan.
