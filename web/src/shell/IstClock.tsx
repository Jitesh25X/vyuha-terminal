import { useEffect, useState } from "react";
import { formatIstTime } from "../lib/format/time";

export function IstClock() {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    const t = setInterval(() => setNow(new Date()), 1000);
    return () => clearInterval(t);
  }, []);
  return (
    <time className="text-fg" dateTime={now.toISOString()}>
      {formatIstTime(now)} IST
    </time>
  );
}
