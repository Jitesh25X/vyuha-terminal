#!/usr/bin/env python3
"""Generate Kite Ticker binary frame fixtures from the documented layout.

These are synthesized, not recorded: no live Kite session is used in this repo's tests (CLAUDE.md
invariant 3). Layout: kite.trade/docs/connect/v3/websocket (verified 2026-10-29), big-endian per
Zerodha's pykiteconnect. Replace or extend with frames recorded from a real session (scrubbed)
when one is available; the tests only depend on the expectations in expected.json.

Run: python3 -I gen_frames.py  (writes *.bin + expected.json next to this file)
"""

import json
import struct
from pathlib import Path

OUT = Path(__file__).parent
NFO_FUT = (61471 << 8) | 2  # token = exchange_token << 8 | segment (2 = NFO)
NSE_EQ = (2885 << 8) | 1
NIFTY_IDX = 256265  # 256265 & 0xff == 9 (indices)
CDS = (1234 << 8) | 3  # currency: unsupported

TS = 1_793_190_900  # 2026-10-28 12:35:00 UTC (18:05 IST), seconds


def ltp(token, paise):
    return struct.pack(">II", token, paise)


def quote(token, ltp, ltq, avg, vol, buy, sell, o, h, l, c):
    return struct.pack(">IIIIIIIIIII", token, ltp, ltq, avg, vol, buy, sell, o, h, l, c)


def full(token, ltp, ltq, avg, vol, o, h, l, c, oi, ts, bids, asks):
    head = quote(token, ltp, ltq, avg, vol, 1000, 2000, o, h, l, c)
    head += struct.pack(">IIIII", ts - 1, oi, oi + 10, oi - 10, ts)
    depth = b"".join(struct.pack(">IIHxx", q, p, n) for (q, p, n) in bids + asks)
    pkt = head + depth
    assert len(pkt) == 184
    return pkt


def index_full(token, ltp, h, l, o, c, change, ts):
    return struct.pack(">IIIIIIII", token, ltp, h, l, o, c, change, ts)


def frame(*packets):
    out = struct.pack(">H", len(packets))
    for p in packets:
        out += struct.pack(">H", len(p)) + p
    return out


bids = [(75 * (i + 1), 2_445_000 - 5 * i, i + 1) for i in range(5)]
asks = [(65 * (i + 1), 2_445_005 + 5 * i, i + 2) for i in range(5)]
frames = {
    "full_mixed.bin": frame(
        full(NFO_FUT, 2_445_005, 75, 2_440_000, 1_234_500, 2_430_000, 2_450_000, 2_425_000, 2_428_000, 9_876_500, TS, bids, asks),
        index_full(NIFTY_IDX, 2_440_015, 2_450_000, 2_420_000, 2_430_000, 2_428_000, 12_015, TS),
        quote(NSE_EQ, 245_005, 10, 244_900, 5_000_000, 100, 200, 244_000, 246_000, 243_500, 243_800),
        ltp(CDS, 881_234_500),
    ),
    "ltp_only.bin": frame(ltp(NSE_EQ, 245_010), ltp(NFO_FUT, 2_445_010)),
    "heartbeat.bin": b"\x00",
}
for name, data in frames.items():
    (OUT / name).write_bytes(data)

expected = {
    "tokens": {"nfo_fut": NFO_FUT, "nse_eq": NSE_EQ, "nifty": NIFTY_IDX, "cds": CDS},
    "ts_seconds": TS,
}
(OUT / "expected.json").write_text(json.dumps(expected, indent=1) + "\n")
print("wrote", ", ".join(frames))
