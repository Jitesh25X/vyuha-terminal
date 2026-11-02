// Tick → screen latency (PRD NFR "tick → browser paint p95 < 400 ms", M1.12 done check).
//
// A tick's `recvTs` is stamped by the engine on the server's clock. The browser's clock differs, so
// the worker estimates the offset with Ping/Pong (NTP-style, keeping the lowest-RTT sample: its
// error is at most RTT/2). Latency = (paint time + offset) − recvTs, sampled in the animation frame
// after a quote is applied (the frame that paints it). Exchange → screen is kept as well; it adds
// the broker's own delay, outside our control.

export interface ClockSample {
  offsetMs: number; // server clock − browser clock
  rttMs: number;
}

/** One Ping/Pong round trip → an offset estimate (server time is taken as the midpoint). */
export function clockSample(clientSendMs: number, serverMs: number, clientRecvMs: number): ClockSample {
  return { offsetMs: serverMs - (clientSendMs + clientRecvMs) / 2, rttMs: clientRecvMs - clientSendMs };
}

/** The best of the recent samples: the lowest RTT bounds the error tightest. */
export function bestSample(samples: readonly ClockSample[]): ClockSample | undefined {
  let best: ClockSample | undefined;
  for (const s of samples) if (s.rttMs >= 0 && (!best || s.rttMs < best.rttMs)) best = s;
  return best;
}

/** Nearest-rank percentile of an unsorted sample (p in [0, 100]). */
export function percentile(values: readonly number[], p: number): number | undefined {
  if (values.length === 0) return undefined;
  const sorted = [...values].sort((a, b) => a - b);
  const rank = Math.min(sorted.length, Math.max(1, Math.ceil((p / 100) * sorted.length)));
  return sorted[rank - 1];
}

export const HISTOGRAM_EDGES_MS = [50, 100, 200, 300, 400, 600, 1000] as const;

/** Counts per bucket: ≤50, ≤100, …, ≤1000, >1000 ms. */
export function histogram(values: readonly number[]): number[] {
  const counts = new Array<number>(HISTOGRAM_EDGES_MS.length + 1).fill(0);
  for (const v of values) {
    let i = HISTOGRAM_EDGES_MS.findIndex((edge) => v <= edge);
    if (i < 0) i = HISTOGRAM_EDGES_MS.length;
    counts[i]!++;
  }
  return counts;
}

/** Fixed-size ring of the most recent samples. */
export class Ring {
  private buf: number[] = [];
  private next = 0;
  constructor(private readonly size: number) {}
  push(v: number): void {
    if (this.buf.length < this.size) this.buf.push(v);
    else this.buf[this.next] = v;
    this.next = (this.next + 1) % this.size;
  }
  values(): readonly number[] {
    return this.buf;
  }
  clear(): void {
    this.buf = [];
    this.next = 0;
  }
}
