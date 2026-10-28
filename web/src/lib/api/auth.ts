import { useMutation, useQuery } from "@tanstack/react-query";
import { useSession } from "../../stores/session";
import { formatIstTime } from "../format/time";
import { ApiError, refreshSession, request } from "./client";
import type { Health, LoginResponse, MfaEnrollResponse, SessionResponse } from "./types";

export function useLogin() {
  return useMutation({
    mutationFn: (v: { username: string; password: string }) =>
      request<LoginResponse>("/auth/login", { body: v }),
    onSuccess: (r) => useSession.getState().startMfa({ token: r.mfa_token, enrolled: r.mfa_enrolled }),
  });
}

/** TOTP step: verify for an enrolled user, or confirm a pending enrollment. */
export function useSubmitTotp(mode: "verify" | "confirm") {
  return useMutation({
    mutationFn: (v: { mfa_token: string; totp: string }) =>
      request<SessionResponse>(mode === "verify" ? "/auth/mfa" : "/auth/mfa/enroll/confirm", { body: v }),
    onSuccess: (s) => useSession.getState().signedIn(s),
  });
}

/** Starts enrollment; the response (QR + otpauth URL) stays in this query's memory only. */
export function useBeginEnroll(mfaToken: string) {
  return useQuery({
    queryKey: ["auth", "enroll", mfaToken],
    queryFn: () => request<MfaEnrollResponse>("/auth/mfa/enroll", { body: { mfa_token: mfaToken } }),
    staleTime: Infinity,
    gcTime: 0,
    retry: false,
  });
}

export function useLogout() {
  return useMutation({
    mutationFn: () => request<void>("/auth/logout", { method: "POST" }),
    // Signed out locally even if the call fails: the access token is dropped from memory either way.
    onSettled: () => useSession.getState().signedOut(),
  });
}

export function restoreSession(): Promise<SessionResponse | null> {
  return refreshSession();
}

export function useHealth() {
  return useQuery({
    queryKey: ["health"],
    queryFn: () => request<Health>("/health"),
    refetchInterval: 10_000,
    retry: false,
  });
}

/** User-facing text for an auth failure. Never echoes server detail beyond the stable code. */
export function authErrorMessage(err: unknown, step: "password" | "totp"): string {
  if (!(err instanceof ApiError)) return "Network error. Check the gateway and try again.";
  switch (err.code) {
    case "AUTH_INVALID":
      return step === "password" ? "Invalid username or password." : "Invalid or already used code.";
    case "AUTH_LOCKED":
      return err.lockedUntil ? `Account locked until ${formatLockedUntil(err.lockedUntil)}.` : "Account locked.";
    case "RATE_LIMITED":
    case "AUTH_BUSY":
      return `Too many attempts. Retry in ${err.retryAfter ?? 60}s.`;
    case "AUTH_UNAUTHENTICATED":
      return "Sign-in step expired. Start again.";
    default:
      return err.message;
  }
}

function formatLockedUntil(iso: string): string {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : `${formatIstTime(d)} IST`;
}
