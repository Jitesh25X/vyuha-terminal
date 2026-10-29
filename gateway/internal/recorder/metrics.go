package recorder

import (
	"fmt"
	"io"
)

// WriteMetrics renders the counters in Prometheus text format (scraped from /metrics; alerting
// on recorder_dropped_total > 0 is wired with the obs profile, M3.8).
func WriteMetrics(w io.Writer, s Stats) error {
	for _, m := range []struct {
		name, help string
		v          uint64
	}{
		{"recorder_received_total", "Market-data messages received from NATS.", s.Received},
		{"recorder_dropped_total", "Messages dropped because the queue was full or NATS dropped them (slow consumer).", s.Dropped},
		{"recorder_decode_errors_total", "Messages that were not valid protobuf.", s.DecodeErrors},
		{"recorder_inserted_ticks_total", "Tick rows written to ClickHouse.", s.InsertedTicks},
		{"recorder_inserted_bars_total", "Bar rows written to ClickHouse.", s.InsertedBars},
		{"recorder_insert_errors_total", "Failed insert attempts (retried).", s.InsertErrors},
		{"recorder_lost_rows_total", "Rows given up after all retries.", s.LostRows},
	} {
		if _, err := fmt.Fprintf(w, "# HELP %s %s\n# TYPE %s counter\n%s %d\n", m.name, m.help, m.name, m.name, m.v); err != nil {
			return fmt.Errorf("metrics: %w", err)
		}
	}
	return nil
}
