-- name: DatabaseNow :one
-- Round-trip used by the health check (and later clock-skew checks).
SELECT now()::timestamptz AS now;
