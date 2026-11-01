package ws

import (
	"context"
	"testing"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testdb"
)

func TestPGDirectory(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := pool.Exec(ctx, sql, args...); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	var user string
	if err := pool.QueryRow(ctx, `INSERT INTO app_user (username, display_name, role, password_hash) VALUES ('meera', 'Meera', 'trader', 'x') RETURNING id::text`).Scan(&user); err != nil {
		t.Fatal(err)
	}
	account := func(broker, client string, entitled bool, status string) string {
		var id string
		err := pool.QueryRow(ctx, `INSERT INTO broker_account (broker, client_code, holder_name, api_key_enc, api_secret_enc, static_ip, data_entitled, status)
			VALUES ($1, $2, 'h', '\x00', '\x00', '127.0.0.1', $3, $4) RETURNING id::text`, broker, client, entitled, status).Scan(&id)
		if err != nil {
			t.Fatal(err)
		}
		return id
	}
	data, noData, hidden, disabled := account("zerodha", "Z1", true, "connected"), account("upstox", "U1", false, "connected"),
		account("upstox", "U2", true, "connected"), account("zerodha", "Z2", true, "disabled")
	exec(`INSERT INTO account_user_map (account_id, user_id, can_view, can_trade) VALUES ($1, $4, true, true), ($2, $4, true, false), ($3, $4, false, false)`, data, noData, hidden, user)
	exec(`INSERT INTO account_user_map (account_id, user_id) VALUES ($1, $2)`, disabled, user)

	dir := NewPGDirectory(store.New(pool))
	got, err := dir.UserAccounts(ctx, user)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]Account{data: {ID: data, Broker: "zerodha", DataEntitled: true, Label: "Z1"}, noData: {ID: noData, Broker: "upstox", Label: "U1"}}
	if len(got) != len(want) {
		t.Fatalf("accounts = %+v (can_view=false and disabled accounts must be excluded)", got)
	}
	for _, a := range got {
		if want[a.ID] != a {
			t.Fatalf("account %+v, want %+v", a, want[a.ID])
		}
	}

	exec(`UPDATE app_user SET is_active = false WHERE id = $1`, user)
	if got, err := dir.UserAccounts(ctx, user); err != nil || len(got) != 0 {
		t.Fatalf("inactive user still entitled: %+v, %v", got, err)
	}

	exec(`INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, tick_size, valid_from, kite_token, upstox_key) VALUES
		(1, 'NSE', 'EQ', 'INFY', 'INFY', 0.05, '2026-10-01', 408065, 'NSE_EQ|INE009A01021'),
		(2, 'NSE', 'EQ', 'ONLYK', 'ONLYK', 0.05, '2026-10-01', 12345, NULL)`)
	exec(`INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, tick_size, valid_from, valid_to, kite_token) VALUES
		(3, 'NSE', 'EQ', 'GONE', 'GONE', 0.05, '2026-10-01', '2026-10-20', 999)`)
	inst, err := dir.Instruments(ctx, []uint64{1, 2, 3, 4})
	if err != nil {
		t.Fatal(err)
	}
	if len(inst) != 2 || inst[1] != (InstrumentMap{Kite: true, Upstox: true}) || inst[2] != (InstrumentMap{Kite: true}) {
		t.Fatalf("instruments = %+v (expired 3 and unknown 4 excluded)", inst)
	}

	exec(`INSERT INTO exchange_holiday (exchange, day, description) VALUES ('NSE', '2026-11-09', 'test')`)
	for day, want := range map[string]bool{"2026-11-09": true, "2026-11-10": false} {
		d, _ := time.ParseInLocation(time.DateOnly, day, ist)
		if got, err := dir.IsHoliday(ctx, d); err != nil || got != want {
			t.Errorf("%s: holiday = %v, %v", day, got, err)
		}
	}
}
