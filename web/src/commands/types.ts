import type { ComponentType } from "react";

/** What a function needs before it can open (UI_SPEC §3 "Needs security"). */
export type Needs = "none" | "security" | "optional" | "underlying" | "index";

export interface PanelProps {
  func: string;
  security?: string;
  segment?: string;
  args: Record<string, string>;
}

export interface FunctionDef {
  mnemonic: string;
  title: string;
  needs: Needs;
  help: string;
  /** Lazy entry point: panels/<FUNC>/index.tsx default export. */
  load: () => Promise<{ default: ComponentType<PanelProps> }>;
}
