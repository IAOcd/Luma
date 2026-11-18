// Luma — your profile on the website: sign in with Microsoft (the same account
// as the launcher), see your year, share it, and manage your data.
//
// HOW SIGNING IN WORKS (the standard "OAuth 2.0 with PKCE" for web pages):
//   1. The page sends you to Microsoft's page. Your password is typed there,
//      never here. PKCE = a one-time secret this page makes up, so nobody else
//      can use what Microsoft sends back.
//   2. Microsoft sends you back (through Luma's server) with a "code". The page
//      sends it, with its PKCE secret, to Luma's server, which trades it at
//      Microsoft for a permission for Xbox Live (with personal Microsoft
//      accounts a browser isn't allowed to do this itself).
//   3. The server asks Xbox who you are and gives back a "Luma pass" (the same
//      one the launcher gets). Microsoft's permission isn't kept: only the
//      pass, in this browser (localStorage).
(() => {
  const $ = s => document.querySelector(s);
  const API = /^(localhost|127\.0\.0\.1)$/.test(location.hostname) ? "http://127.0.0.1:8787" : "https://luma-api.gios-app.workers.dev";
  const CLIENT_ID = "602c60cc-fed7-4ec9-aa25-f9f73ca0959b";  // "Luma Launcher" in Azure (public by design)
  const AUTHORITY = "https://login.microsoftonline.com/consumers/oauth2/v2.0";
  const SCOPE = "XboxLive.signin";
  // Microsoft comes back to Luma's server, which passes you here (#signin?code=…):
  // with personal Microsoft accounts the code can't be traded in the browser.
  const REDIRECT = API + "/v1/auth/callback";
  const KEY = "luma-pass";
  const t = key => { const d = window.LUMA_I18N || {}; const lang = document.documentElement.lang || "en"; return (d[lang] || d.en || {})[key] ?? (d.en || {})[key] ?? key; };

  /* ---------- the pass, kept in this browser ---------- */
  function readPass() {
    try {
      const p = JSON.parse(localStorage.getItem(KEY) || "null");
      if (p && typeof p.token === "string" && p.expires * 1000 > Date.now()) return p;
    } catch (e) {}
    return null;
  }
  let pass = readPass();
  const savePass = p => { pass = p; try { localStorage.setItem(KEY, JSON.stringify(p)); } catch (e) {} };
  const forget = () => { pass = null; try { localStorage.removeItem(KEY); } catch (e) {} };

  async function api(method, path, body) {
    const r = await fetch(API + path, {
      method,
      headers: Object.assign({ authorization: "Bearer " + (pass && pass.token) }, body ? { "content-type": "application/json" } : {}),
      body: body ? JSON.stringify(body) : undefined
    });
    if (r.status === 401) { forget(); paintHeader(); paint(); throw new Error("session"); }
    const data = await r.json().catch(() => ({}));
    if (!r.ok) { const e = new Error(data.error || String(r.status)); e.code = data.error; throw e; }
    return data;
  }

  /* ---------- 1. to Microsoft ---------- */
  const b64url = bytes => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  const random = n => b64url(crypto.getRandomValues(new Uint8Array(n)));
  async function signIn() {
    const verifier = random(48), state = random(16);
    const challenge = b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)));
    try { sessionStorage.setItem("luma-signin", JSON.stringify({ verifier, state })); } catch (e) {}
    const q = new URLSearchParams({
      client_id: CLIENT_ID, response_type: "code", response_mode: "query", redirect_uri: REDIRECT, scope: SCOPE,
      code_challenge: challenge, code_challenge_method: "S256", state, prompt: "select_account"
    });
    location.assign(`${AUTHORITY}/authorize?${q}`);
  }

  /* ---------- 2 and 3. back from Microsoft (through Luma's server) ---------- */
  async function finish() {
    if (!location.hash.startsWith("#signin?")) return;
    const q = new URLSearchParams(location.hash.slice("#signin?".length));
    let saved = null;
    try { saved = JSON.parse(sessionStorage.getItem("luma-signin") || "null"); sessionStorage.removeItem("luma-signin"); } catch (e) {}
    history.replaceState(null, "", location.pathname + "#me");  // (the code leaves the address bar)
    if (q.has("error")) { showError(q.get("error") === "access_denied" ? "" : "me.err_generic", q.get("error")); return go(); }
    if (!saved || q.get("state") !== saved.state || !q.get("code")) { showError("me.err_generic", saved ? "state" : "no-session"); return go(); }
    signingIn = true; paint(); go();
    try {
      // Luma's server trades the code (with this browser's PKCE secret) at Microsoft, then at Xbox
      const res = await fetch(API + "/v1/auth/web", {
        method: "POST", headers: { "content-type": "application/json" },
        body: JSON.stringify({ code: q.get("code"), verifier: saved.verifier })
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw Object.assign(new Error("luma"), { code: data.error, detail: data.error || "luma " + res.status });
      savePass(data);
    } catch (e) {
      console.warn("Luma sign-in:", e.detail || e.message);
      showError(e.code === "no_xbox_profile" ? "me.err_no_xbox" : e.code === "child_account" ? "me.err_child" : "me.err_generic", e.detail || e.message);
    }
    signingIn = false; paintHeader(); paint();
  }
  function go() { if (location.hash !== "#me") location.hash = "#me"; else document.dispatchEvent(new CustomEvent("luma:view", { detail: { id: "me" } })); }

  /* ---------- the header button ---------- */
  const btn = $("#meBtn");
  // (the server sends your picture already inside the pass, as "data:image/png;…")
  const avatarUrl = p => p && typeof p.picture === "string" && p.picture.startsWith("data:image/") ? p.picture : "";
  function paintHeader() {
    if (!btn) return;
    if (pass) {
      btn.classList.add("in");
      btn.innerHTML = `${avatarUrl(pass) ? `<img alt="" width="28" height="28">` : ""}<span></span>`;
      if (avatarUrl(pass)) btn.querySelector("img").src = avatarUrl(pass);
      btn.querySelector("span").textContent = pass.gamertag;
    } else {
      btn.classList.remove("in");
      btn.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/></svg><span></span>`;
      btn.querySelector("span").textContent = t("me.signin_short");
    }
  }

  /* ---------- the profile page ---------- */
  const out = $("#meOut"), inn = $("#meIn"), err = $("#meErr"), msg = $("#meMsg"), canvas = $("#meCanvas"), link = $("#meLink");
  let signingIn = false, year = new Date().getFullYear(), summary = null, loadedFor = null, busy = false, wantName = false;
  // (detail: a short technical reason, e.g. Microsoft's "AADSTS…" code: it says what went wrong)
  function showError(key, detail) { if (err) err.textContent = key ? t(key) + (detail ? ` (${detail})` : "") : ""; }
  function paint() {
    if (!out) return;
    out.hidden = !!pass; inn.hidden = !pass;
    $("#meSignIn").disabled = signingIn;
    $("#meSignIn span").textContent = t(signingIn ? "me.signing" : "me.signin");
    if (!pass) return;
    $("#meName").textContent = pass.gamertag;
    const av = $("#meAvatar"); av.hidden = !avatarUrl(pass); if (avatarUrl(pass)) av.src = avatarUrl(pass);
    [...$("#meYear").children].forEach(b => b.setAttribute("aria-pressed", String(+b.dataset.v === year)));
    if (!summary || summary.year !== year) { canvas.hidden = true; return; }
    const empty = !summary.seconds;
    canvas.hidden = empty;
    msg.textContent = empty ? t("me.empty") : "";
    if (!empty && window.LUMA_YEAR) window.LUMA_YEAR.draw(canvas, Object.assign({}, summary, { name: pass.gamertag }));
    $("#meCharts").checked = !!summary.inCharts;
    const shared = summary.share && summary.share.url;
    $("#meShareName").checked = summary.share ? !!summary.share.showName : wantName;
    $("#meShare").textContent = t(shared ? "me.copy" : "me.share");
    $("#meUnshare").hidden = !shared;
    $("#meSave").hidden = empty;
    [$("#meShare"), $("#meCharts"), $("#meShareName")].forEach(el => el.disabled = empty || busy);
    link.textContent = shared ? summary.share.url : "";
  }
  async function load(force) {
    if (!pass || (!force && loadedFor === year)) { paint(); return; }
    msg.textContent = t("gal.loading");
    try { summary = await api("GET", `/v1/me/wrapped?year=${year}`); loadedFor = year; msg.textContent = ""; }
    catch (e) { if (e.message !== "session") msg.textContent = t("gal.error"); }
    paint();
  }
  const say = key => { $("#meNote").textContent = key ? t(key) : ""; };
  async function act(fn, okKey) {
    if (busy) return; busy = true; paint(); say("");
    try { await fn(); if (okKey) say(okKey); } catch (e) { if (e.message !== "session") say(e.code === "nothing_to_share" ? "me.empty" : "gal.error"); }
    busy = false; await load(true);
  }

  if (out) {
    $("#meSignIn").addEventListener("click", signIn);
    $("#meSignOut").addEventListener("click", async () => {
      try { await fetch(API + "/v1/auth/logout", { method: "POST", headers: { authorization: "Bearer " + pass.token } }); } catch (e) {}
      forget(); summary = null; loadedFor = null; paintHeader(); paint();
    });
    $("#meYear").addEventListener("click", e => { const b = e.target.closest("button[data-v]"); if (b) { year = +b.dataset.v; load(); } });
    // (what you chose is read first: act() repaints, which puts back the saved value until the answer comes)
    $("#meCharts").addEventListener("change", e => { const on = e.target.checked; act(() => api("PUT", "/v1/me/charts", { on }), on ? "me.charts_on" : "me.charts_off"); });
    $("#meShareName").addEventListener("change", e => {
      const showName = e.target.checked;
      if (summary && summary.share) act(() => api("POST", "/v1/me/wrapped/share", { year, showName }));
      else if (summary) summary.share = null, wantName = showName;
    });
    $("#meShare").addEventListener("click", () => act(async () => {
      const r = await api("POST", "/v1/me/wrapped/share", { year, showName: wantName });
      try { await navigator.clipboard.writeText(r.url); } catch (e) {}
    }, "me.copied"));
    $("#meUnshare").addEventListener("click", () => act(() => api("DELETE", `/v1/me/wrapped/share?year=${year}`), "me.unshared"));
    $("#meSave").addEventListener("click", () => canvas.toBlob(b => {
      const a = document.createElement("a"); a.href = URL.createObjectURL(b); a.download = `Luma ${year}.png`; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    }, "image/png"));
    $("#meExport").addEventListener("click", () => act(async () => {
      const data = await api("GET", "/v1/me/export");
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: "application/json" }));
      a.download = "Luma - my data.json"; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    }));
    let armed = 0;
    $("#meDelete").addEventListener("click", async () => {
      if (Date.now() - armed > 5000) { armed = Date.now(); $("#meDelete").textContent = t("me.delete_confirm"); setTimeout(() => { $("#meDelete").textContent = t("me.delete"); }, 5000); return; }
      armed = 0;
      try { await api("DELETE", "/v1/me"); } catch (e) { say("gal.error"); return; }
      forget(); summary = null; loadedFor = null; paintHeader(); paint(); showError("me.deleted");
    });
    // the year buttons: this year and the one before
    const now = new Date().getFullYear();
    $("#meYear").innerHTML = [now, now - 1].map(y => `<button type="button" data-v="${y}">${y}</button>`).join("");
  }

  document.addEventListener("luma:view", e => { if (e.detail.id === "me") load(); });
  document.addEventListener("luma:lang", () => { paintHeader(); paint(); });
  paintHeader();
  finish();
})();
