# Upstox adapter — Market Data Feed V3

Order placement, modify and cancel arrive in M3 and accept only `risk::ApprovedOrder` (invariant 1).
Nothing here talks to live Upstox in tests (invariant 3): tests use local HTTP/WebSocket servers.

## Sources (verified 2026-10-29)
| What | URL |
|---|---|
| Feed protocol, modes, limits, request format (binary frames) | https://upstox.com/developer/api-documentation/v3/get-market-data-feed/ |
| Authorize endpoint (single-use `wss://` URL) | https://upstox.com/developer/api-documentation/get-market-data-feed-authorize-v3/ |
| Protobuf schema (vendored as `engine/third_party/upstox/MarketDataFeed.proto`) | https://assets.upstox.com/feed/market-data-feed/v3/MarketDataFeed.proto |
| Instrument keys | https://upstox.com/developer/api-documentation/instruments/ |

## Limits (per user)
| Plan | Connections | LTPC | Option Greeks | Full | Full D30 |
|---|---|---|---|---|---|
| Standard | 2 | 5 000 / 2 000 | 3 000 / 2 000 | 2 000 / 1 500 | — |
| Plus | 5 | (as Standard) | (as Standard) | (as Standard) | 50 / 1 500 |

`individual / combined`: individual applies when only that mode is subscribed, combined (per mode)
when several modes are used together. The docs don't say whether subscription limits are per
connection; `plan_subscriptions` enforces them per user (stricter) and the feed uses one connection.

## Protocol notes
- `GET https://api.upstox.com/v3/feed/market-data-feed/authorize` with `Authorization: Bearer <token>`
  → `{"status":"success","data":{"authorized_redirect_uri":"wss://…?requestId=…&code=…"}}`. The
  `code` is single-use, so every (re)connection authorizes again. Neither the token nor the URL is
  logged.
- Requests are JSON sent as **binary** WebSocket frames:
  `{"guid":"…","method":"sub","data":{"mode":"full","instrumentKeys":["NSE_FO|50001"]}}` (chunks of 500).
- Frames are protobuf `FeedResponse`: `market_info` first (segment status), then `live_feed`.
  The engine decodes the wire format directly (`feed_parser.cc`) with string_views into the frame:
  no allocation per frame/tick. `protoc`-generated classes are used only in tests, as the reference.
- Prices arrive as IEEE-754 rupee values; the decoder converts each straight to `Price` through the
  single core helper `Price::from_wire_rupees` (nearest 1/10 000 rupee); NaN/inf/negative values drop
  the feed (`bad_price`). No floating-point price is kept in broker code (invariant 8). Timestamps (`ltt`, `currentTs`) are epoch ms;
  `exchange_ts = ltt`, else `currentTs`, else receive time. `ltt` (the last trade time) also fills
  `Tick::last_trade_ts`; without it that stays 0 and the core's last-value book infers it (M1.11b).
- `full_d30` carries 30 depth levels; `Tick` keeps the best 5. Day OHLC comes from the `1d` entry.
- The server pings when idle; Beast answers pings and the idle timeout (30 s) catches dead sockets.

## Static IP (invariant 4)
`start()` fails with `kStaticIpNotOnHost` unless `source_ip` is on a local interface; both the
authorize call and the WebSocket bind to it before connecting.

## Tests / fixtures / fuzz
- `fixtures/vcr/upstox/*.bin`: built with the reference classes (`tests/upstox_test_frames.h`;
  regenerate with `VYUHA_WRITE_FIXTURES=1`). Synthesized, not recorded.
- `tests/upstox_parser_test.cc` (decoder vs reference encoder, limits table, URL parsing),
  `tests/upstox_feed_test.cc` (authorize per connection, single-use codes, binary requests,
  reconnect + gap log, static-IP preflight).
- `fuzz/upstox_feed_fuzz.cc`: `make fuzz-upstox`.
