import type { DockviewApi } from "dockview";
import type { Command } from "../commands/parse";
import type { PanelProps } from "../commands/types";
import type { Role } from "../lib/api/types";

// The live Dockview instance, set by <Workspace>. Kept outside React state: opening a panel is
// an imperative action on the layout, not a render input.
let api: DockviewApi | null = null;
let seq = 0;

export function setWorkspaceApi(a: DockviewApi | null): void {
  api = a;
}

export function panelTitle(p: PanelProps): string {
  return [p.func, p.security, p.segment].filter(Boolean).join(" ");
}

function toProps(cmd: Command): PanelProps {
  const p: PanelProps = { func: cmd.fn.mnemonic, args: cmd.args };
  if (cmd.security !== undefined) p.security = cmd.security;
  if (cmd.segment !== undefined) p.segment = cmd.segment;
  return p;
}

/** Opens a function: "replace" swaps the focused panel, "tab" adds a tab next to it. */
export function openFunction(cmd: Command, mode: "replace" | "tab"): void {
  if (!api) return;
  const params = toProps(cmd);
  const active = api.activePanel;
  const panel = api.addPanel<PanelProps>({
    id: `${params.func}-${++seq}`,
    component: "fn",
    title: panelTitle(params),
    params,
    ...(active ? { position: { referencePanel: active, direction: "within" as const } } : {}),
  });
  if (mode === "replace" && active) active.api.close();
  panel.api.setActive();
}

/** Returns keyboard focus to the active panel (Esc from the command bar). */
export function focusActivePanel(): void {
  const id = api?.activePanel?.id;
  if (id) document.querySelector<HTMLElement>(`[data-panel-id="${CSS.escape(id)}"]`)?.focus();
}

/** Ctrl+N: activate the visible panel of the Nth group (layout order). */
export function focusGroup(n: number): void {
  const group = api?.groups[n - 1];
  group?.activePanel?.api.setActive();
}

type Spec = { func: string; dir?: "right" | "below" | "within"; ref?: number };

// UI_SPEC §8 default workspaces. `ref` indexes an earlier entry; omitted = previous one.
const DEFAULTS: Record<Role, Spec[]> = {
  trader: [{ func: "CH" }, { func: "DOM", dir: "right" }, { func: "OC", dir: "right" }, { func: "POS", dir: "below", ref: 0 }, { func: "BLT", dir: "within" }, { func: "AI", dir: "right", ref: 3 }],
  risk_manager: [{ func: "RISK" }, { func: "SCN", dir: "right" }, { func: "POS", dir: "below", ref: 0 }, { func: "BLT", dir: "right" }, { func: "CTX", dir: "below", ref: 2 }, { func: "N", dir: "right" }],
  analyst: [{ func: "STRAT" }, { func: "BT", dir: "right" }, { func: "CH", dir: "below", ref: 0 }, { func: "DL", dir: "right" }, { func: "AI", dir: "right", ref: 1 }],
  admin: [{ func: "SYS" }, { func: "ADM", dir: "right" }, { func: "AUD", dir: "below", ref: 0 }],
};

export function loadDefaultLayout(a: DockviewApi, role: Role): void {
  const ids: string[] = [];
  for (const [i, s] of DEFAULTS[role].entries()) {
    const params: PanelProps = { func: s.func, args: {} };
    const id = `${s.func}-${++seq}`;
    const ref = ids[s.ref ?? i - 1];
    a.addPanel<PanelProps>({
      id,
      component: "fn",
      title: panelTitle(params),
      params,
      ...(ref && s.dir ? { position: { referencePanel: ref, direction: s.dir } } : {}),
    });
    ids.push(id);
  }
}
