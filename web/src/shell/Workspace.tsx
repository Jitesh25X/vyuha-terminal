import { createDockview, type DockviewPanelApi, type DockviewTheme } from "dockview";
import { lazy, Suspense, useEffect, useRef, useState, type ComponentType, type LazyExoticComponent } from "react";
import { createPortal } from "react-dom";
import { findFunction } from "../commands/registry";
import type { PanelProps } from "../commands/types";
import type { Role } from "../lib/api/types";
import { loadDefaultLayout, setWorkspaceApi } from "./layout";

// Dockview 8 ships its React bindings separately (dockview-react, not an approved dependency),
// so the layout uses dockview-core's DOM API and React renders each panel through a portal.

const theme: DockviewTheme = { name: "vyuha", className: "dockview-theme-vyuha", colorScheme: "dark" };

const lazyPanels = new Map<string, LazyExoticComponent<ComponentType<PanelProps>>>();
function panelFor(func: string) {
  let c = lazyPanels.get(func);
  if (!c) {
    const def = findFunction(func);
    c = lazy(def ? def.load : () => import("../panels/stub"));
    lazyPanels.set(func, c);
  }
  return c;
}

interface Mounted {
  element: HTMLElement;
  params: PanelProps;
  api: DockviewPanelApi;
}

function FunctionPanel({ params, api }: Omit<Mounted, "element">) {
  const ref = useRef<HTMLDivElement>(null);
  // Keyboard-first: the active panel takes focus so its own shortcuts work without a click.
  useEffect(() => {
    const focus = () => {
      if (api.isActive && !ref.current?.contains(document.activeElement)) ref.current?.focus();
    };
    focus();
    const d = api.onDidActiveChange(focus);
    return () => d.dispose();
  }, [api]);
  const Panel = panelFor(params.func);
  return (
    <div ref={ref} tabIndex={-1} className="h-full overflow-auto outline-none" data-func={params.func} data-panel-id={api.id}>
      <Suspense fallback={<div className="p-3 text-muted">loading {params.func}…</div>}>
        <Panel {...params} />
      </Suspense>
    </div>
  );
}

export function Workspace({ role }: { role: Role }) {
  const host = useRef<HTMLDivElement>(null);
  const [mounted, setMounted] = useState<ReadonlyMap<string, Mounted>>(new Map());

  useEffect(() => {
    if (!host.current) return;
    const api = createDockview(host.current, {
      theme,
      createComponent: ({ id }) => {
        const element = document.createElement("div");
        element.style.height = "100%";
        return {
          element,
          init: ({ params, api: panelApi }) =>
            setMounted((m) => new Map(m).set(id, { element, params: params as PanelProps, api: panelApi })),
          dispose: () =>
            setMounted((m) => {
              const next = new Map(m);
              next.delete(id);
              return next;
            }),
        };
      },
    });
    setWorkspaceApi(api);
    loadDefaultLayout(api, role);
    return () => {
      setWorkspaceApi(null);
      api.dispose();
    };
  }, [role]);

  return (
    <div ref={host} className="flex-1 min-h-0 min-w-0">
      {[...mounted].map(([id, m]) => createPortal(<FunctionPanel params={m.params} api={m.api} />, m.element, id))}
    </div>
  );
}
