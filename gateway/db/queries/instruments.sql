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
