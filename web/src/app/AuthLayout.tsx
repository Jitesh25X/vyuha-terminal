import type { ReactNode } from "react";

export function AuthLayout({ title, children }: { title: string; children: ReactNode }) {
  return (
    <main className="h-full flex items-center justify-center">
      <div className="w-80 border border-line bg-panel">
        <div className="h-[22px] px-2 flex items-center bg-panel-header border-b border-line text-amber font-label tracking-widest">
          VYUHA ▸ {title}
        </div>
        <div className="p-4 space-y-3">{children}</div>
      </div>
    </main>
  );
}

export const inputClass = "w-full h-6 px-2 bg-bg border border-line text-amber focus:border-accent";
export const buttonClass = "w-full h-6 border border-amber text-amber hover:bg-grid disabled:opacity-50";
