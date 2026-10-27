package audit

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5"
)

// Canonical form v2 — must match audit_chain() in db/migrations/000004_audit_hash_v2.up.sql:
//
//	hash = sha256(prev_hash || F(ts) F(actor_id) F(actor_kind) F(action) F(entity) F(entity_id) F(payload) F(ip))
//	F(NULL) = "-", F(v) = len(v) + ":" + v   (len in bytes)
//
// The hash and chain are recomputed here in Go, independently of the database function, so a
// replaced or edited trigger function is detected too. Field text is rendered by Postgres.
const verifyQuery = `
SELECT id,
       to_char(ts AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
       actor_id::text, actor_kind, action, entity, entity_id, payload::text, host(ip),
       prev_hash, hash
FROM audit_event ORDER BY id`

// genesis is prev_hash of the first row.
var genesis = []byte{0x00}

// Querier is satisfied by pgx pools, connections and transactions.
type Querier interface {
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
}

// Anchor is an externally kept (id, hash) of a known row, e.g. from `vyuhactl audit head` stored
// off-database. The chain alone cannot detect deletion of the newest rows; an anchor can.
type Anchor struct {
	ID   int64
	Hash []byte
}

// ParseAnchor parses "<id>:<hex hash>".
func ParseAnchor(s string) (Anchor, error) {
	idPart, hashPart, ok := strings.Cut(s, ":")
	id, err := strconv.ParseInt(idPart, 10, 64)
	h, herr := hex.DecodeString(hashPart)
	if !ok || err != nil || herr != nil || len(h) != sha256.Size {
		return Anchor{}, errors.New("anchor must be <id>:<64 hex chars>")
	}
	return Anchor{ID: id, Hash: h}, nil
}

// Result reports a verification run. FirstBadID is set when OK is false.
type Result struct {
	OK          bool
	RowsChecked int64
	FirstBadID  int64
	Reason      string
	HeadID      int64  // last row checked
	HeadHash    []byte // its stored hash (use as the next external anchor)
}

// Verify walks the whole chain in id order. It reports the first row whose stored hash doesn't
// match its content, whose prev_hash doesn't match the previous row (a deleted or inserted row),
// or, given anchors, a missing or altered anchored row.
func Verify(ctx context.Context, db Querier, anchors ...Anchor) (Result, error) {
	rows, err := db.Query(ctx, verifyQuery)
	if err != nil {
		return Result{}, fmt.Errorf("audit verify: %w", err)
	}
	defer rows.Close()

	want := map[int64][]byte{}
	for _, a := range anchors {
		want[a.ID] = a.Hash
	}
	res := Result{OK: true}
	prev := genesis
	for rows.Next() {
		var (
			id                             int64
			ts, actorKind, action, entity  string
			actorID, entityID, payload, ip *string
			prevHash, hash                 []byte
		)
		if err := rows.Scan(&id, &ts, &actorID, &actorKind, &action, &entity, &entityID, &payload, &ip, &prevHash, &hash); err != nil {
			return Result{}, fmt.Errorf("audit verify scan: %w", err)
		}
		res.RowsChecked++
		res.HeadID, res.HeadHash = id, hash

		fail := func(reason string) (Result, error) {
			res.OK, res.FirstBadID, res.Reason = false, id, reason
			return res, nil
		}
		if !bytes.Equal(prevHash, prev) {
			return fail("chain broken: prev_hash does not match the previous row (row deleted, inserted or reordered)")
		}
		if !bytes.Equal(hash, rowHash(prevHash, ts, actorID, actorKind, action, entity, entityID, payload, ip)) {
			return fail("hash mismatch: row content was modified")
		}
		if a, ok := want[id]; ok {
			if !bytes.Equal(a, hash) {
				return fail("anchor mismatch")
			}
			delete(want, id)
		}
		prev = hash
	}
	if err := rows.Err(); err != nil {
		return Result{}, fmt.Errorf("audit verify: %w", err)
	}
	for id := range want {
		res.OK, res.FirstBadID, res.Reason = false, id, "anchored row missing (newest rows deleted?)"
		return res, nil
	}
	return res, nil
}

func rowHash(prev []byte, ts string, actorID *string, actorKind, action, entity string, entityID, payload, ip *string) []byte {
	var b strings.Builder
	for _, f := range []*string{&ts, actorID, &actorKind, &action, &entity, entityID, payload, ip} {
		b.WriteString(field(f))
	}
	h := sha256.New()
	h.Write(prev)
	h.Write([]byte(b.String()))
	return h.Sum(nil)
}

func field(v *string) string {
	if v == nil {
		return "-"
	}
	return strconv.Itoa(len(*v)) + ":" + *v
}
