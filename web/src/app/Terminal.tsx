import { useEffect, useRef } from "react";
import { tinykeys } from "tinykeys";
import { parseCommand } from "../commands/parse";
import { refreshSession } from "../lib/api/client";
import { BROKER_LOGIN_ERRORS } from "../lib/api/brokers";
import { startMarketData, stopMarketData, subscribe } from "../lib/ws/client";
import { pushToast } from "../stores/toasts";
import type { User } from "../lib/api/types";
import { StatusBar } from "../shell/StatusBar";
import { Toasts } from "../shell/Toasts";
import { TopBar } from "../shell/TopBar";
import { Workspace } from "../shell/Workspace";
import { focusGroup, openFunction } from "../shell/layout";

const REFRESH_LEAD_MS = 60_000;

export function Terminal({ user, expiresAt }: { user: User; expiresAt: number }) {
  const commandRef = useRef<HTMLInputElement>(null);

  // Refresh the access token a minute before it expires (serialized in lib/api/client).
  useEffect(() => {
    const t = setTimeout(() => void refreshSession(), Math.max(5_000, expiresAt - Date.now() - REFRESH_LEAD_MS));
    return () => clearTimeout(t);
  }, [expiresAt]);

  // Back from a broker login (gateway callback → /terminal?broker_login=ok|error&…): tell the user,
  // then drop the query so a reload doesn't repeat it.
  useEffect(() => {
    const q = new URLSearchParams(window.location.search);
    const outcome = q.get("broker_login");
    if (!outcome) return;
    if (outcome === "ok") pushToast("info", "Broker login complete — the session is starting");
    else {
      const code = q.get("code") ?? "INTERNAL";
      pushToast("error", `Broker login failed: ${BROKER_LOGIN_ERRORS[code] ?? code}`);
    }
    window.history.replaceState(null, "", "/");
  }, []);

  // Market-data stream for the session; the status bar's system channel is always on.
  useEffect(() => {
    startMarketData();
    const release = subscribe("system", []);
    return () => {
      release();
      stopMarketData();
    };
  }, []);

  // Global keyboard map (UI_SPEC §4).
  useEffect(() => {
    const focusBar = (e: KeyboardEvent) => {
      e.preventDefault();
      commandRef.current?.focus();
    };
    const help = (e: KeyboardEvent) => {
      e.preventDefault();
      const r = parseCommand("HELP");
      if (r.ok) openFunction(r.cmd, "tab");
    };
    const anywhere = { ignore: (e: KeyboardEvent) => e.repeat || e.isComposing };
    const digits = Object.fromEntries(
      [1, 2, 3, 4, 5, 6, 7, 8, 9].map((n) => [
        `Control+Digit${n}`,
        (e: KeyboardEvent) => {
          e.preventDefault();
          focusGroup(n);
        },
      ]),
    );
    const offPanels = tinykeys(window, { Backquote: focusBar });
    const offGlobal = tinykeys(window, { "Control+k": focusBar, "$mod+k": focusBar, F1: help, "Control+/": help, ...digits }, anywhere);
    return () => {
      offPanels();
      offGlobal();
    };
  }, []);

  return (
    <div className="h-full flex flex-col">
      <TopBar user={user} commandRef={commandRef} />
      <main className="flex-1 min-h-0 flex">
        <Workspace role={user.role} />
      </main>
      <StatusBar />
      <Toasts />
    </div>
  );
}
