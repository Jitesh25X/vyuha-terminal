import { Command } from "cmdk";
import { forwardRef, useRef, useState, type KeyboardEvent } from "react";
import { completeFunction, parseCommand } from "../commands/parse";
import { FUNCTIONS } from "../commands/registry";
import { useUi } from "../stores/ui";
import { focusActivePanel, openFunction } from "./layout";

// Command line (UI_SPEC §2): fuzzy over functions now; instruments join in M1 with the master.

export const CommandBar = forwardRef<HTMLInputElement>(function CommandBar(_, inputRef) {
  const [value, setValue] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [open, setOpen] = useState(false);
  const mode = useRef<"replace" | "tab">("replace");
  const history = useUi((s) => s.history);
  const pushHistory = useUi((s) => s.pushHistory);

  const parsed = value.trim() ? parseCommand(value) : null;
  const completions = value.trim() ? completeFunction(value, FUNCTIONS).slice(0, 8) : [];

  function run(text: string) {
    const r = parseCommand(text);
    if (!r.ok) {
      setValue(text);
      setError(r.error);
      return;
    }
    pushHistory(text.trim().toUpperCase());
    setValue("");
    setError(null);
    setOpen(false);
    openFunction(r.cmd, mode.current);
  }

  function complete(mnemonic: string) {
    const words = value.trim().split(/\s+/);
    words[words.length - 1] = mnemonic;
    run(words.join(" "));
  }

  function onKeyDown(e: KeyboardEvent) {
    if (e.key === "Enter") mode.current = e.shiftKey ? "tab" : "replace";
    if (e.key === "Escape") {
      e.preventDefault();
      if (value) {
        setValue("");
        setError(null);
      } else {
        setOpen(false);
        focusActivePanel();
      }
    }
  }

  return (
    <Command label="Command bar" shouldFilter={false} loop onKeyDown={onKeyDown} className="relative flex-1 max-w-3xl">
      <Command.Input
        ref={inputRef}
        value={value}
        onValueChange={(v) => {
          setValue(v);
          setError(null);
        }}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        placeholder="RELIANCE EQ CH  ·  HELP"
        spellCheck={false}
        autoComplete="off"
        className="w-full h-5 px-2 bg-bg border border-line text-amber placeholder:text-muted uppercase focus:border-accent"
      />
      {open && (
        <Command.List className="absolute z-50 left-0 right-0 top-6 bg-panel-header border border-line max-h-80 overflow-auto">
          {error && <div role="alert" className="px-2 py-1 text-down">{error}</div>}
          {parsed?.ok && (
            <Command.Item value={`run:${value}`} onSelect={() => run(value)} className="px-2 py-0.5 aria-selected:bg-grid">
              <span className="text-amber">{value.trim().toUpperCase()}</span> <span className="text-muted">— {parsed.cmd.fn.title}</span>
            </Command.Item>
          )}
          {completions.map((f) => (
            <Command.Item key={f.mnemonic} value={`fn:${f.mnemonic}`} onSelect={() => complete(f.mnemonic)} className="px-2 py-0.5 aria-selected:bg-grid">
              <span className="text-amber inline-block w-14">{f.mnemonic}</span>
              {f.title} <span className="text-muted">— {f.help}</span>
            </Command.Item>
          ))}
          {!value.trim() &&
            history.map((h) => (
              <Command.Item key={h} value={`hist:${h}`} onSelect={() => run(h)} className="px-2 py-0.5 aria-selected:bg-grid">
                <span className="text-muted">↺</span> {h}
              </Command.Item>
            ))}
          {!error && parsed && !parsed.ok && completions.length === 0 && (
            <div className="px-2 py-1 text-muted">{parsed.error}</div>
          )}
        </Command.List>
      )}
    </Command>
  );
});
