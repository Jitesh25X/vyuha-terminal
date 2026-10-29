// Watchlists are kept per user and per name in this browser (localStorage) until the server-side
// `watchlist` table gets an API. They hold instrument ids and symbols only: nothing sensitive.

export interface WatchItem {
  id: number;
  sym: string;
}

export const MAX_WATCHLIST = 2000; // the gateway's quote-key limit per connection

const key = (user: string, name: string) => `vyuha.wl.${user}.${name}`;

export function loadWatchlist(user: string, name: string): WatchItem[] {
  try {
    const raw = localStorage.getItem(key(user, name));
    const parsed: unknown = raw ? JSON.parse(raw) : [];
    if (!Array.isArray(parsed)) return [];
    return parsed
      .filter((x): x is WatchItem => typeof x === "object" && x !== null && Number.isSafeInteger((x as WatchItem).id) && typeof (x as WatchItem).sym === "string")
      .slice(0, MAX_WATCHLIST);
  } catch {
    return [];
  }
}

export function saveWatchlist(user: string, name: string, items: WatchItem[]): void {
  try {
    localStorage.setItem(key(user, name), JSON.stringify(items.slice(0, MAX_WATCHLIST)));
  } catch {
    // storage full or disabled: the list stays in memory for this session
  }
}
