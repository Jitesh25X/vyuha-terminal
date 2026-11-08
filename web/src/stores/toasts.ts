import { create } from "zustand";

export interface Toast {
  id: number;
  level: "info" | "warn" | "error";
  text: string;
}

const TTL_MS = 8_000;
const MAX = 5;
let seq = 0;

export const useToasts = create<{ toasts: Toast[] }>(() => ({ toasts: [] }));

export function pushToast(level: Toast["level"], text: string): void {
  const id = ++seq;
  useToasts.setState((s) => ({ toasts: [...s.toasts, { id, level, text }].slice(-MAX) }));
  setTimeout(() => dismissToast(id), TTL_MS);
}

/** An alert's severity as a toast level; anything unknown shows as an error rather than being played down. */
export function toastLevel(severity: string): Toast["level"] {
  if (severity === "info") return "info";
  if (severity === "warn") return "warn";
  return "error";
}

export function dismissToast(id: number): void {
  useToasts.setState((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) }));
}
