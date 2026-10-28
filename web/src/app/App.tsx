import { useEffect } from "react";
import { restoreSession } from "../lib/api/auth";
import { useSession } from "../stores/session";
import { LoginScreen } from "./LoginScreen";
import { MfaScreen } from "./MfaScreen";
import { Terminal } from "./Terminal";

export function App() {
  const state = useSession((s) => s.state);

  // A reload has no access token in memory: try the refresh cookie once.
  useEffect(() => {
    if (useSession.getState().state.status === "restoring") void restoreSession();
  }, []);

  switch (state.status) {
    case "restoring":
      return <p className="p-4 text-muted">Restoring session…</p>;
    case "anonymous":
      return <LoginScreen notice={state.notice} />;
    case "mfa":
      return <MfaScreen mfa={state.mfa} />;
    case "authenticated":
      return <Terminal user={state.user} expiresAt={state.expiresAt} />;
  }
}
