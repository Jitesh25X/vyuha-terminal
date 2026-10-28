import { useState, type FormEvent } from "react";
import { authErrorMessage, useLogin } from "../lib/api/auth";
import { AuthLayout, buttonClass, inputClass } from "./AuthLayout";

export function LoginScreen({ notice }: { notice: string | undefined }) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const login = useLogin();

  function submit(e: FormEvent) {
    e.preventDefault();
    if (!login.isPending) login.mutate({ username: username.trim().toLowerCase(), password });
  }

  return (
    <AuthLayout title="SIGN IN">
      <form onSubmit={submit} className="space-y-3">
        {notice && <p className="text-warn">{notice}</p>}
        <label className="block space-y-1">
          <span className="text-muted">Username</span>
          <input
            className={inputClass}
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            autoComplete="username"
            autoFocus
            required
            maxLength={64}
          />
        </label>
        <label className="block space-y-1">
          <span className="text-muted">Password</span>
          <input
            className={inputClass}
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete="current-password"
            required
            maxLength={512}
          />
        </label>
        {login.isError && (
          <p role="alert" className="text-down">
            {authErrorMessage(login.error, "password")}
          </p>
        )}
        <button className={buttonClass} type="submit" disabled={login.isPending}>
          {login.isPending ? "Checking…" : "Continue"}
        </button>
      </form>
    </AuthLayout>
  );
}
