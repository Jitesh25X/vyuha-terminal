import { FUNCTIONS, findFunction } from "../../commands/registry";
import type { PanelProps } from "../../commands/types";

const KEYS: readonly [string, string][] = [
  ["` or Ctrl+K", "Focus command bar"],
  ["Enter / Shift+Enter", "Open in focused panel / in a new tab"],
  ["Esc", "Clear command bar, back to workspace"],
  ["↑ / ↓ (empty bar)", "Command history"],
  ["F1", "Help"],
  ["Ctrl+1…9", "Focus panel N"],
];

const NEEDS: Record<string, string> = {
  none: "—",
  security: "security",
  optional: "optional",
  underlying: "underlying",
  index: "index",
};

export default function HelpPanel({ args }: PanelProps) {
  const topic = args["topic"] ? findFunction(args["topic"]) : undefined;
  return (
    <section aria-label="Help" className="p-3 space-y-4 overflow-auto h-full">
      <h2 className="text-amber font-label text-sm tracking-wide">HELP</h2>
      {topic && (
        <div aria-label={`Help for ${topic.mnemonic}`} className="border border-line p-2">
          <div className="text-amber">
            {topic.mnemonic} — {topic.title}
          </div>
          <p>{topic.help}</p>
          <p className="text-muted">Needs: {NEEDS[topic.needs]}</p>
        </div>
      )}
      <table className="w-full border-collapse">
        <caption className="text-left text-muted pb-1">Keyboard</caption>
        <tbody>
          {KEYS.map(([k, v]) => (
            <tr key={k} className="border-b border-grid">
              <td className="text-amber pr-4 whitespace-nowrap">{k}</td>
              <td>{v}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <table className="w-full border-collapse">
        <caption className="text-left text-muted pb-1">Functions — [security] [segment] FUNCTION [key=value]</caption>
        <tbody>
          {FUNCTIONS.map((f) => (
            <tr key={f.mnemonic} className="border-b border-grid">
              <td className="text-amber pr-4">{f.mnemonic}</td>
              <td className="pr-4 whitespace-nowrap">{f.title}</td>
              <td className="pr-4 text-muted">{NEEDS[f.needs]}</td>
              <td className="text-muted">{f.help}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  );
}
