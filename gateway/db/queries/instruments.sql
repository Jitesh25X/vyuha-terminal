-- name: SearchInstruments :many
-- Prefix match on trading symbol or underlying symbol; exact symbol first, then cash/index, then
-- nearer expiries. @prefix arrives with LIKE wildcards escaped.
SELECT id, exchange, segment, symbol, tradingsymbol, name, underlying_id, expiry, strike, option_type,
       lot_size, tick_size, freeze_qty, fo_ban, valid_to
FROM instrument
WHERE valid_to IS NULL
  AND (tradingsymbol ILIKE @prefix::text || '%' OR symbol ILIKE @prefix::text || '%')
  AND (sqlc.narg(segment)::segment_code IS NULL OR segment = sqlc.narg(segment)::segment_code)
  AND (sqlc.narg(exchange)::exchange_code IS NULL OR exchange = sqlc.narg(exchange)::exchange_code)
ORDER BY upper(tradingsymbol) = upper(@prefix::text) DESC, upper(symbol) = upper(@prefix::text) DESC,
         segment IN ('EQ', 'IDX') DESC, expiry NULLS FIRST, length(tradingsymbol), tradingsymbol
LIMIT @lim;

-- name: GetInstrument :one
SELECT id, exchange, segment, symbol, tradingsymbol, name, underlying_id, expiry, strike, option_type,
       lot_size, tick_size, freeze_qty, fo_ban, valid_to
FROM instrument WHERE id = @id;

-- name: ListOptionExpiries :many
-- Active option expiries of an underlying from @from_day (today IST), nearest first.
SELECT DISTINCT expiry::date AS expiry
FROM instrument
WHERE underlying_id = @underlying_id AND segment = 'OPT' AND valid_to IS NULL AND expiry >= @from_day::date
ORDER BY expiry;

-- name: ListChainContracts :many
-- Every active option of one expiry (instrument_chain index), strikes ascending, CE before PE.
SELECT id, tradingsymbol, strike, option_type, lot_size, tick_size
FROM instrument
WHERE underlying_id = @underlying_id AND segment = 'OPT' AND expiry = @expiry::date AND valid_to IS NULL
ORDER BY strike, option_type;

-- name: GetSameExpiryFuture :one
SELECT id
FROM instrument
WHERE underlying_id = @underlying_id AND segment = 'FUT' AND expiry = @expiry::date AND valid_to IS NULL
ORDER BY id
LIMIT 1;

-- name: ListUpcomingDividends :many
-- Cash dividends going ex in (@after, @until] (escrowed-dividend BSM for stock options).
SELECT ex_date, amount::numeric AS amount
FROM corporate_action
WHERE instrument_id = @instrument_id AND kind = 'dividend' AND amount IS NOT NULL
  AND ex_date > @after::date AND ex_date <= @until::date
ORDER BY ex_date;
