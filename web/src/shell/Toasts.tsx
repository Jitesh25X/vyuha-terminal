import { dismissToast, useToasts } from "../stores/toasts";

const LEVEL = { info: "border-accent text-accent", warn: "border-warn text-warn", error: "border-down text-down" } as const;

export function Toasts() {
  const toasts = useToasts((s) => s.toasts);
  return (
    <div className="fixed bottom-7 right-2 z-50 flex flex-col gap-1 items-end" role="status" aria-live="polite">
      {toasts.map((t) => (
        <button
          key={t.id}
          type="button"
          onClick={() => dismissToast(t.id)}
          className={`bg-panel-header border px-2 py-1 text-xs text-left focus-visible:outline focus-visible:outline-amber ${LEVEL[t.level]}`}
        >
          {t.text}
        </button>
      ))}
    </div>
  );
}
