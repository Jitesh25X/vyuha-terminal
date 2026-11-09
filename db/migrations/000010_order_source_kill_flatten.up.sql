-- D-23: an order the kill switch generated is the only kind R-KILL lets through, so it has its own source.
ALTER TYPE order_source ADD VALUE IF NOT EXISTS 'kill_flatten';
