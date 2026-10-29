package rbac

import "testing"

func TestCanBrokerLogin(t *testing.T) {
	trade := &Mapping{CanView: true, CanTrade: true}
	view := &Mapping{CanView: true}
	tests := []struct {
		name string
		role string
		m    *Mapping
		want bool
	}{
		{"admin unmapped", RoleAdmin, nil, true},
		{"trader with can_trade", RoleTrader, trade, true},
		{"trader view only", RoleTrader, view, false},
		{"trader unmapped", RoleTrader, nil, false},
		{"risk manager mapped", RoleRiskManager, trade, false},
		{"analyst mapped", RoleAnalyst, trade, false},
		{"unknown role", "root", trade, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := CanBrokerLogin(tt.role, tt.m); got != tt.want {
				t.Errorf("CanBrokerLogin(%q, %+v) = %v, want %v", tt.role, tt.m, got, tt.want)
			}
		})
	}
}

func TestCanReceiveMarketData(t *testing.T) {
	tests := []struct {
		name      string
		m         *Mapping
		entitled  bool
		wantAllow bool
	}{
		{"mapped viewer, entitled account", &Mapping{CanView: true}, true, true},
		{"mapped viewer, account without entitlement", &Mapping{CanView: true}, false, false},
		{"mapping without can_view", &Mapping{CanTrade: true}, true, false},
		{"not mapped", nil, true, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := CanReceiveMarketData(tt.m, tt.entitled); got != tt.wantAllow {
				t.Errorf("got %v, want %v", got, tt.wantAllow)
			}
		})
	}
}

func TestSeesAllAccounts(t *testing.T) {
	for role, want := range map[string]bool{RoleAdmin: true, RoleRiskManager: true, RoleTrader: false, RoleAnalyst: false} {
		if got := SeesAllAccounts(role); got != want {
			t.Errorf("SeesAllAccounts(%q) = %v, want %v", role, got, want)
		}
	}
}
