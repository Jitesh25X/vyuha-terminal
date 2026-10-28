// Display time is IST (Asia/Kolkata); storage and wire formats stay UTC (CLAUDE.md invariant 9).

const istTime = new Intl.DateTimeFormat("en-IN", {
  timeZone: "Asia/Kolkata",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
  hourCycle: "h23",
});

/** "14:32:07" in IST for any instant. */
export function formatIstTime(d: Date): string {
  return istTime.format(d);
}
