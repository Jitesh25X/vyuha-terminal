import { findFunction } from "./registry";
import type { FunctionDef } from "./types";

// Command grammar (UI_SPEC §2):  [security] [segment] function [key=value ...]
// Special form: HELP <FUNC>.

export const SEGMENTS = ["EQ", "FUT", "OPT", "IDX", "NSE", "BSE"] as const;

export interface Command {
  fn: FunctionDef;
  security?: string;
  segment?: string;
  args: Record<string, string>;
}

export type ParseResult = { ok: true; cmd: Command } | { ok: false; error: string };

const SECURITY = /^(#\d+|[A-Z0-9][A-Z0-9&_.-]*)$/;
const ARG = /^([a-z][a-z0-9_]*)=(\S+)$/i;

export function parseCommand(input: string): ParseResult {
  const tokens = input.trim().split(/\s+/).filter(Boolean);
  if (tokens.length === 0) return { ok: false, error: "Type a function, e.g. HELP" };

  const args: Record<string, string> = {};
  const words: string[] = [];
  for (const t of tokens) {
    const m = ARG.exec(t);
    if (m) {
      if (words.length === 0) return { ok: false, error: `Arguments go after the function: ${t}` };
      args[m[1]!.toLowerCase()] = m[2]!;
    } else if (Object.keys(args).length > 0) {
      return { ok: false, error: `Unexpected "${t}" after arguments` };
    } else {
      words.push(t.toUpperCase());
    }
  }

  if (words[0] === "HELP" && words.length <= 2) {
    const help = findFunction("HELP")!;
    if (words.length === 2) {
      const topic = findFunction(words[1]!);
      if (!topic) return { ok: false, error: `Unknown function ${words[1]}` };
      args["topic"] = topic.mnemonic;
    }
    return { ok: true, cmd: { fn: help, args } };
  }

  if (words.length > 3) return { ok: false, error: "Too many words: [security] [segment] function" };
  const fn = findFunction(words[words.length - 1]!);
  if (!fn) return { ok: false, error: `Unknown function ${words[words.length - 1]}` };

  let rest = words.slice(0, -1);
  let segment: string | undefined;
  const last = rest[rest.length - 1];
  if (last && (SEGMENTS as readonly string[]).includes(last)) {
    segment = last;
    rest = rest.slice(0, -1);
  }
  if (rest.length > 1) return { ok: false, error: `Unexpected "${rest[1]}"` };
  const security = rest[0];
  if (security !== undefined && !SECURITY.test(security)) return { ok: false, error: `Invalid security ${security}` };

  if (security === undefined && (fn.needs === "security" || fn.needs === "underlying" || fn.needs === "index")) {
    const what = fn.needs === "security" ? "a security" : `an ${fn.needs}`;
    return { ok: false, error: `${fn.mnemonic} needs ${what}, e.g. ${fn.needs === "security" ? "RELIANCE" : "NIFTY"} ${fn.mnemonic}` };
  }
  if (security !== undefined && fn.needs === "none") {
    return { ok: false, error: `${fn.mnemonic} does not take a security` };
  }

  const cmd: Command = { fn, args };
  if (security !== undefined) cmd.security = security;
  if (segment !== undefined) cmd.segment = segment;
  return { ok: true, cmd };
}

/** Functions whose mnemonic or title starts with the last typed word, for command-bar suggestions. */
export function completeFunction(input: string, all: readonly FunctionDef[]): FunctionDef[] {
  const last = input.trim().split(/\s+/).pop()?.toUpperCase() ?? "";
  if (!last || last.includes("=")) return [];
  return all.filter((f) => f.mnemonic.startsWith(last) || f.title.toUpperCase().startsWith(last));
}
