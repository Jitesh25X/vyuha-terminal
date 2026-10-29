package brokers

import "time"

// ist is Asia/Kolkata (UTC+05:30, no DST); a fixed zone avoids depending on the host's tzdata.
var ist = time.FixedZone("IST", 5*3600+30*60)

// tokenExpiry is when a broker access token issued at `issued` stops working:
//   - Kite: 06:00 IST the next morning (Kite Connect v3 docs, "regulatory requirement");
//   - Upstox: 03:30 IST the next morning ("lasts until 3:30 AM the following day, regardless of the
//     time it was generated").
//
// "Next" means the first such instant after issue, so a token from 02:00 IST lasts until that
// morning's cut-off rather than a day later (conservative: we never assume a token outlives it).
func tokenExpiry(broker string, issued time.Time) time.Time {
	hour, minute := 6, 0
	if broker == "upstox" {
		hour, minute = 3, 30
	}
	t := issued.In(ist)
	cut := time.Date(t.Year(), t.Month(), t.Day(), hour, minute, 0, 0, ist)
	if !cut.After(t) {
		cut = cut.AddDate(0, 0, 1)
	}
	return cut.UTC()
}
