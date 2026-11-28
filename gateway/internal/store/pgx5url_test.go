package store

import "testing"

func TestPgx5URL(t *testing.T) {
	tests := map[string]string{
		"postgres://u:p@h:5432/d?sslmode=disable": "pgx5://u:p@h:5432/d?sslmode=disable",
		"postgresql://u@h/d":                      "pgx5://u@h/d",
		"pgx5://u@h/d":                            "pgx5://u@h/d",
	}
	for in, want := range tests {
		if got := pgx5URL(in); got != want {
			t.Errorf("pgx5URL(%q) = %q, want %q", in, got, want)
		}
	}
}
