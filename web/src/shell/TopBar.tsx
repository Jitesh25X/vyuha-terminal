import type { Ref } from "react";
import { useLogout } from "../lib/api/auth";
import type { User } from "../lib/api/types";
import { CommandBar } from "./CommandBar";
import { IstClock } from "./IstClock";

export function TopBar({ user, commandRef }: { user: User; commandRef: Ref<HTMLInputElement> }) {
  const logout = useLogout();
  return (
    <header className="h-7 flex items-center gap-3 px-2 bg-panel-header border-b border-line">
      <span className="text-amber font-label font-semibold tracking-widest">VYUHA ▸</span>
      <CommandBar ref={commandRef} />
      <kbd className="text-muted">Ctrl+K</kbd>
      <IstClock />
      <span className="text-amber uppercase" title={user.role}>
        {user.display_name}
      </span>
      <button
        type="button"
        onClick={() => logout.mutate()}
        disabled={logout.isPending}
        className="text-muted hover:text-fg border border-line px-1"
      >
        Sign out
      </button>
    </header>
  );
}
