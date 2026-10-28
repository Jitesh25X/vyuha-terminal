import { create } from "zustand";

const MAX_HISTORY = 50;

interface UiStore {
  /** Most recent first. In memory only for now; layout persistence comes with Ctrl+S. */
  history: string[];
  pushHistory: (cmd: string) => void;
}

export const useUi = create<UiStore>((set) => ({
  history: [],
  pushHistory: (cmd) => set((s) => ({ history: [cmd, ...s.history.filter((h) => h !== cmd)].slice(0, MAX_HISTORY) })),
}));
