-- ClickHouse schema. Prices stored as Int64 in 1/100 paise (price * 10000) to match the engine; views expose Decimal.
CREATE DATABASE IF NOT EXISTS vyuha;

CREATE TABLE IF NOT EXISTS vyuha.ticks
(
    instrument_id   UInt64,
    exchange_ts     DateTime64(6, 'UTC'),
    recv_ts         DateTime64(6, 'UTC'),
    source          LowCardinality(String),          -- 'zerodha' | 'upstox'
    ltp             Int64,
    ltq             UInt32,
    volume          UInt64,
    oi              UInt64,
    bid             Int64,
    ask             Int64,
    bid_qty         UInt32,
    ask_qty         UInt32,
    avg_price       Int64
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(exchange_ts)
ORDER BY (instrument_id, exchange_ts)
TTL toDateTime(exchange_ts) + INTERVAL 365 DAY
SETTINGS index_granularity = 8192;

CREATE TABLE IF NOT EXISTS vyuha.depth
(
    instrument_id UInt64,
    exchange_ts   DateTime64(6, 'UTC'),
    side          Enum8('bid' = 1, 'ask' = 2),
    level         UInt8,
    price         Int64,
    qty           UInt32,
    orders        UInt32
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(exchange_ts)
ORDER BY (instrument_id, exchange_ts, side, level)
TTL toDateTime(exchange_ts) + INTERVAL 30 DAY;

-- Bars: both live-built (engine) and downloaded (historical). ReplacingMergeTree dedupes by key; prefer latest `version`.
CREATE TABLE IF NOT EXISTS vyuha.bars
(
    instrument_id UInt64,
    interval      LowCardinality(String),           -- '1s','1m','5m','15m','1h','1d'
    ts            DateTime('UTC'),                  -- bar open time
    open          Int64,
    high          Int64,
    low           Int64,
    close         Int64,
    volume        UInt64,
    oi            UInt64,
    source        LowCardinality(String),           -- 'engine' | 'zerodha_hist' | 'upstox_hist'
    version       UInt64                            -- unix ms of write
)
ENGINE = ReplacingMergeTree(version)
PARTITION BY (interval, toYYYYMM(ts))
ORDER BY (instrument_id, interval, ts);

CREATE TABLE IF NOT EXISTS vyuha.risk_metrics
(
    ts         DateTime64(3, 'UTC'),
    scope      LowCardinality(String),               -- desk|account|trader|strategy
    scope_id   String,
    metric     LowCardinality(String),               -- delta, gamma, vega, theta, var99_1d, gross, net, beta_delta, ...
    value      Float64
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(ts)
ORDER BY (scope, scope_id, metric, ts)
TTL toDateTime(ts) + INTERVAL 730 DAY;

CREATE TABLE IF NOT EXISTS vyuha.pnl_ts
(
    ts            DateTime64(3, 'UTC'),
    account_id    UUID,
    user_id       UUID,
    strategy_id   Nullable(UUID),
    realized      Float64,
    unrealized    Float64,
    costs         Float64
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(ts)
ORDER BY (account_id, ts);

CREATE TABLE IF NOT EXISTS vyuha.engine_latency
(
    ts      DateTime64(6, 'UTC'),
    stage   LowCardinality(String),
    ns      UInt64
)
ENGINE = MergeTree
ORDER BY (stage, ts)
TTL toDateTime(ts) + INTERVAL 14 DAY;

CREATE TABLE IF NOT EXISTS vyuha.eval_results
(
    ts        DateTime('UTC'),
    suite     LowCardinality(String),
    case_id   String,
    model     LowCardinality(String),
    passed    UInt8,
    score     Float64,
    details   String
)
ENGINE = MergeTree
ORDER BY (suite, ts);

-- Convenience view in rupees
CREATE VIEW IF NOT EXISTS vyuha.bars_inr AS
SELECT instrument_id, interval, ts,
       open / 10000.0 AS open, high / 10000.0 AS high, low / 10000.0 AS low, close / 10000.0 AS close,
       volume, oi, source
FROM vyuha.bars FINAL;
