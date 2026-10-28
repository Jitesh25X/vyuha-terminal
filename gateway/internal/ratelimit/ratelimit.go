// Package ratelimit is a keyed token-bucket limiter (e.g. per client IP) using only the standard library.
package ratelimit

import (
	"sync"
	"time"
)

// Limiter allows `burst` events per key, refilled evenly over `per` (burst / per is the steady rate).
// Keys whose bucket has refilled completely are forgotten, so memory is bounded by active keys.
type Limiter struct {
	mu        sync.Mutex
	burst     float64
	interval  time.Duration // time to refill one token
	now       func() time.Time
	buckets   map[string]*bucket
	lastSweep time.Time
}

type bucket struct {
	tokens float64
	at     time.Time
}

// New creates a limiter. now is injected for tests; pass time.Now in production.
func New(burst int, per time.Duration, now func() time.Time) *Limiter {
	return &Limiter{
		burst: float64(burst), interval: per / time.Duration(burst), now: now,
		buckets: map[string]*bucket{}, lastSweep: now(),
	}
}

// Allow takes a token for key. When denied it returns how long until the next token.
func (l *Limiter) Allow(key string) (bool, time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	l.sweep(now)

	b, ok := l.buckets[key]
	if !ok {
		b = &bucket{tokens: l.burst, at: now}
		l.buckets[key] = b
	}
	b.tokens = l.refilled(b, now)
	b.at = now
	if b.tokens >= 1 {
		b.tokens--
		return true, 0
	}
	wait := time.Duration((1 - b.tokens) * float64(l.interval))
	return false, wait.Round(time.Millisecond)
}

// Len reports how many keys are tracked.
func (l *Limiter) Len() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.buckets)
}

func (l *Limiter) refilled(b *bucket, now time.Time) float64 {
	t := b.tokens + float64(now.Sub(b.at))/float64(l.interval)
	if t > l.burst {
		return l.burst
	}
	return t
}

// sweep drops full buckets at most once per refill period of a whole bucket.
func (l *Limiter) sweep(now time.Time) {
	if now.Sub(l.lastSweep) < time.Duration(l.burst)*l.interval {
		return
	}
	l.lastSweep = now
	for k, b := range l.buckets {
		if l.refilled(b, now) >= l.burst {
			delete(l.buckets, k)
		}
	}
}
