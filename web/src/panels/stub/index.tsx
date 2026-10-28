import { findFunction } from "../../commands/registry";
import type { PanelProps } from "../../commands/types";

/** Placeholder for registered functions whose panel lands in a later task. */
export default function StubPanel({ func, security, segment }: PanelProps) {
  const def = findFunction(func);
  return (
    <div className="p-3 space-y-2">
      <div className="text-amber">
        {func} {security} {segment} <span className="text-muted">— {def?.title}</span>
      </div>
      <p className="text-muted">{def?.help}</p>
      <p className="text-muted">Not built yet. Type HELP {func} for details.</p>
    </div>
  );
}
