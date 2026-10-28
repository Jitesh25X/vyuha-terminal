import { create } from "zustand";
import type { SessionResponse, User } from "../lib/api/types";

// Tokens live in memory only (never localStorage): a reload restores the session through the
// HttpOnly refresh cookie.

export interface MfaChallenge {
  token: string;
  enrolled: boolean;
}

export type SessionState =
  | { status: "restoring" }
  | { status: "anonymous"; notice?: string }
  | { status: "mfa"; mfa: MfaChallenge }
  | { status: "authenticated"; accessToken: string; expiresAt: number; user: User };

interface SessionStore {
  state: SessionState;
  startMfa: (mfa: MfaChallenge) => void;
  signedIn: (s: SessionResponse, now?: number) => void;
  signedOut: (notice?: string) => void;
}

export const useSession = create<SessionStore>((set) => ({
  state: { status: "restoring" },
  startMfa: (mfa) => set({ state: { status: "mfa", mfa } }),
  signedIn: (s, now = Date.now()) =>
    set({
      state: {
        status: "authenticated",
        accessToken: s.access_token,
        expiresAt: now + s.expires_in * 1000,
        user: s.user,
      },
    }),
  signedOut: (notice) => set({ state: notice ? { status: "anonymous", notice } : { status: "anonymous" } }),
}));

export function accessToken(): string | undefined {
  const s = useSession.getState().state;
  return s.status === "authenticated" ? s.accessToken : undefined;
}
