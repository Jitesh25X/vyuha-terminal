package ratelimit

import (
	"testing"
	"time"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

func newTest(burst int, per time.Duration) (*Limiter, *clock) {
	c := &clock{t: time.Date(2026, 10, 28, 4, 0, 0, 0, time.UTC)}
	return New(burst, per, c.now), c
}

func TestBurstThenRefill(t *testing.T) {
	l, c := newTest(10, time.Minute) // 10 per minute = one token every 6 s
	for i := 0; i < 10; i++ {
		if ok, _ := l.Allow("ip-a"); !ok {
			t.Fatalf("request %d denied within burst", i+1)
		}
	}
	ok, retry := l.Allow("ip-a")
	if ok || retry != 6*time.Second {
		t.Fatalf("11th: ok=%t retry=%v, want denied with 6s", ok, retry)
	}
	c.advance(5 * time.Second)
	if ok, retry := l.Allow("ip-a"); ok || retry != time.Second {
		t.Fatalf("after 5s: ok=%t retry=%v, want denied with 1s", ok, retry)
	}
	c.advance(time.Second)
	if ok, _ := l.Allow("ip-a"); !ok {
		t.Fatal("token not refilled after 6s")
	}
	if ok, _ := l.Allow("ip-a"); ok {
		t.Fatal("only one token should have refilled")
	}
}

func TestKeysAreIndependent(t *testing.T) {
	l, _ := newTest(2, time.Minute)
	l.Allow("ip-a")
	l.Allow("ip-a")
	if ok, _ := l.Allow("ip-a"); ok {
		t.Fatal("ip-a should be limited")
	}
	if ok, _ := l.Allow("ip-b"); !ok {
		t.Fatal("ip-b must not share ip-a's bucket")
	}
}

func TestRefillCapsAtBurst(t *testing.T) {
	l, c := newTest(3, time.Minute)
	l.Allow("k")
	c.advance(24 * time.Hour)
	for i := 0; i < 3; i++ {
		if ok, _ := l.Allow("k"); !ok {
			t.Fatalf("request %d denied", i+1)
		}
	}
	if ok, _ := l.Allow("k"); ok {
		t.Fatal("bucket refilled beyond burst")
	}
}

func TestIdleKeysAreEvicted(t *testing.T) {
	l, c := newTest(5, time.Minute)
	for _, k := range []string{"a", "b", "c"} {
		l.Allow(k)
	}
	c.advance(2 * time.Minute) // all buckets full again: nothing to remember
	l.Allow("d")               // triggers a sweep
	if n := l.Len(); n != 1 {
		t.Fatalf("%d keys tracked, want 1", n)
	}
}
