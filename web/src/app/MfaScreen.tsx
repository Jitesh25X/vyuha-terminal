import { useState, type FormEvent, type ReactNode } from "react";
import { authErrorMessage, useBeginEnroll, useSubmitTotp } from "../lib/api/auth";
import type { MfaChallenge } from "../stores/session";
import { useSession } from "../stores/session";
import { AuthLayout, buttonClass, inputClass } from "./AuthLayout";

/** TOTP step. First login (not enrolled) shows the authenticator QR before asking for a code. */
export function MfaScreen({ mfa }: { mfa: MfaChallenge }) {
  return mfa.enrolled ? <CodeForm mfa={mfa} mode="verify" /> : <Enroll mfa={mfa} />;
}

function Enroll({ mfa }: { mfa: MfaChallenge }) {
  const enroll = useBeginEnroll(mfa.token);
  if (enroll.isPending) return <AuthLayout title="SET UP 2FA"><p className="text-muted">Preparing…</p></AuthLayout>;
  if (enroll.isError) {
    return (
      <AuthLayout title="SET UP 2FA">
        <p role="alert" className="text-down">{authErrorMessage(enroll.error, "totp")}</p>
        <BackButton />
      </AuthLayout>
    );
  }
  // The QR encodes the TOTP seed: it is rendered from memory only and never stored or logged.
  return (
    <CodeForm mfa={mfa} mode="confirm">
      <p className="text-fg">Scan with your authenticator app, then enter the 6-digit code.</p>
      <img
        src={`data:image/png;base64,${enroll.data.qr_png_base64}`}
        alt="Authenticator QR code"
        className="mx-auto w-48 h-48 bg-white p-2"
      />
    </CodeForm>
  );
}

function CodeForm({ mfa, mode, children }: { mfa: MfaChallenge; mode: "verify" | "confirm"; children?: ReactNode }) {
  const [code, setCode] = useState("");
  const submitTotp = useSubmitTotp(mode);

  function submit(e: FormEvent) {
    e.preventDefault();
    if (!submitTotp.isPending && /^\d{6}$/.test(code)) submitTotp.mutate({ mfa_token: mfa.token, totp: code });
  }

  return (
    <AuthLayout title={mode === "verify" ? "2FA" : "SET UP 2FA"}>
      {children}
      <form onSubmit={submit} className="space-y-3">
        <label className="block space-y-1">
          <span className="text-muted">Authenticator code</span>
          <input
            className={`${inputClass} tracking-[0.4em] text-center`}
            value={code}
            onChange={(e) => setCode(e.target.value.replace(/\D/g, "").slice(0, 6))}
            inputMode="numeric"
            autoComplete="one-time-code"
            autoFocus
            required
            pattern="\d{6}"
          />
        </label>
        {submitTotp.isError && (
          <p role="alert" className="text-down">{authErrorMessage(submitTotp.error, "totp")}</p>
        )}
        <button className={buttonClass} type="submit" disabled={submitTotp.isPending || code.length !== 6}>
          {submitTotp.isPending ? "Verifying…" : mode === "verify" ? "Sign in" : "Confirm & sign in"}
        </button>
      </form>
      <BackButton />
    </AuthLayout>
  );
}

function BackButton() {
  return (
    <button type="button" className="text-muted hover:text-fg" onClick={() => useSession.getState().signedOut()}>
      ← Start over
    </button>
  );
}
