// Luma — secciones, animaciones e idioma de la web. Sin librerías.
(() => {
  const root = document.documentElement;
  root.classList.remove("no-js");
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const $ = (s, el = document) => el.querySelector(s);
  const $$ = (s, el = document) => [...el.querySelectorAll(s)];

  /* ---------- palabras sueltas (para animarlas una por una) ---------- */
  function splitWords(el) {
    el.innerHTML = el.textContent.trim().split(/\s+/).map(w => `<span class="w">${w}</span>`).join(" ");
  }
  function prepWords() {
    let i = 0;
    $$(".home-title .ln").forEach(ln => { splitWords(ln); $$(".w", ln).forEach(w => w.style.setProperty("--i", i++)); });
    $$(".sing").forEach(splitWords);
  }

  /* ---------- idioma ---------- */
  const dict = window.LUMA_I18N || {};
  const lang = $(".lang"), pill = $(".lang-pill");
  const meta = {
    en: ["Luma · Music client for Minecraft Bedrock", "Luma is a free music client for Minecraft Bedrock on Windows: the song playing on your PC, its lyrics word by word and a visualizer, inside your game."],
    es: ["Luma · Client de música para Minecraft Bedrock", "Luma es un client de música gratis para Minecraft Bedrock en Windows: la canción que suena en tu PC, su letra palabra por palabra y un visualizador, dentro del juego."],
    pt: ["Luma · Client de música para Minecraft Bedrock", "Luma é um client de música grátis para Minecraft Bedrock no Windows: a música que toca no seu PC, a letra palavra por palavra e um visualizador, dentro do jogo."]
  };
  function placePill(container, pillEl, active, inset) {
    if (!active) return;
    pillEl.style.width = active.offsetWidth + "px";
    pillEl.style.translate = (active.offsetLeft - (inset || 0)) + "px 0";
  }
  function setLang(code, save) {
    const t = dict[code] || dict.en;
    $$("[data-i18n]").forEach(el => { const v = t[el.dataset.i18n]; if (v != null) el.textContent = v; });
    $$("button[data-lang]", lang).forEach(b => b.setAttribute("aria-pressed", String(b.dataset.lang === code)));
    root.lang = code;
    document.title = meta[code][0];
    $("meta[name='description']").setAttribute("content", meta[code][1]);
    prepWords();
    paintSung();
    requestAnimationFrame(() => { placePill(lang, pill, $("button[aria-pressed='true']", lang)); placeDock(); });
    if (save) { try { localStorage.setItem("luma-lang", code); } catch (e) {} }
    document.dispatchEvent(new CustomEvent("luma:lang", { detail: { code } }));
  }

  lang.addEventListener("click", e => { const b = e.target.closest("button[data-lang]"); if (b) setLang(b.dataset.lang, true); });

  /* ---------- secciones (sin scroll: se cambian desde el dock) ---------- */
  const order = $$(".view").map(v => v.id);
  const dock = $(".dock"), dockPill = $(".dock-pill");
  let current = null;
  function placeDock() {
    const a = $(`a[data-go="${current}"]`, dock);
    if (!a || a.classList.contains("dock-get")) { dockPill.style.opacity = "0"; return; }
    dockPill.style.opacity = "1";
    placePill(dock, dockPill, a);
  }
  function go(full, how) {
    // "year/<code>" = the "year" page showing that shared summary
    let [id, arg] = String(full || "").split("/");
    if (!order.includes(id)) { id = "home"; arg = undefined; }
    full = arg ? id + "/" + arg : id;
    document.dispatchEvent(new CustomEvent("luma:view", { detail: { id, arg } }));
    if (id === current) { if (how === "push") history.pushState(null, "", "#" + full); return; }
    const from = current ? $("#" + current) : null, to = $("#" + id);
    const dir = current && order.indexOf(id) < order.indexOf(current) ? -1 : 1;
    root.style.setProperty("--dir", dir);
    if (from) { from.classList.remove("on"); from.classList.add("out"); from.setAttribute("aria-hidden", "true"); from.inert = true;
      setTimeout(() => from.classList.remove("out"), 700); }
    to.classList.remove("out"); to.removeAttribute("aria-hidden"); to.inert = false;
    to.scrollTop = 0;
    // forzar que la entrada se vuelva a animar
    void to.offsetWidth; to.classList.add("on");
    current = id;
    document.body.dataset.view = id;
    $$(".aura i").forEach(l => l.classList.toggle("on", l.dataset.v === id));
    if (typeof lines !== "undefined" && lines.length) lines.forEach((L, k) => { L.to = shapeFor(id, k); });
    $$("a[data-go]", dock).forEach(a => { if (a.dataset.go === id) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current"); });
    placeDock();
    if (how === "push") history.pushState(null, "", "#" + full);
    else if (how === "replace") history.replaceState(null, "", "#" + full);
  }
  $$(".view").forEach(v => { v.inert = true; v.setAttribute("aria-hidden", "true"); });
  document.addEventListener("click", e => {
    const a = e.target.closest("[data-go]");
    if (!a || e.metaKey || e.ctrlKey || e.shiftKey) return;
    e.preventDefault(); go(a.dataset.go, "push");
  });
  addEventListener("hashchange", () => go(location.hash.slice(1), null));
  addEventListener("popstate", () => go(location.hash.slice(1), null));

  /* ---------- pestañas (capturas y preguntas) ---------- */
  function tabs(list, onPick) {
    const btns = $$("[role=tab]", list);
    const pick = (b, focus) => {
      btns.forEach(x => { const on = x === b; x.setAttribute("aria-selected", String(on)); x.tabIndex = on ? 0 : -1; });
      if (focus) b.focus();
      onPick(b);
    };
    btns.forEach((b, k) => { b.tabIndex = k === 0 ? 0 : -1; b.addEventListener("click", () => pick(b)); });
    list.addEventListener("keydown", e => {
      const i = btns.indexOf(document.activeElement); if (i < 0) return;
      const n = e.key === "ArrowDown" || e.key === "ArrowRight" ? 1 : e.key === "ArrowUp" || e.key === "ArrowLeft" ? -1 : 0;
      if (!n) return; e.preventDefault(); e.stopPropagation();
      pick(btns[(i + n + btns.length) % btns.length], true);
    });
  }
  $$(".picks").forEach(list => {
    const fig = $(`.screen[data-group="${list.dataset.group}"]`);
    tabs(list, b => $$("img", fig).forEach(img => {
      const on = img.dataset.shot === b.dataset.shot;
      if (on) { img.loading = "eager"; img.classList.remove("on"); void img.offsetWidth; }
      img.classList.toggle("on", on);
    }));
  });
  tabs($(".qlist"), b => $$(".answer .a").forEach(a => a.classList.toggle("on", a.dataset.a === b.dataset.q)));

  /* ---------- inicio: cosas que aparecen al bajar ---------- */
  const home = $("#home");
  const io = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); } }),
    { root: home, rootMargin: "0px 0px -12% 0px", threshold: .1 });
  $$(".reveal", home).forEach(el => {
    const sibs = [...el.parentElement.children].filter(c => c.classList.contains("reveal"));
    el.style.setProperty("--rd", Math.min(sibs.indexOf(el), 4) * 90 + "ms");
    io.observe(el);
  });
  // los tres principios se "cantan" palabra por palabra según cuánto bajaste
  function paintSung() {
    const vh = innerHeight;
    $$(".sing").forEach(h => {
      const ws = [...h.children];
      if (reduce) { ws.forEach(w => w.className = "w on"); return; }
      const r = h.getBoundingClientRect();
      const lit = Math.min(1, Math.max(0, (vh * .85 - r.top) / (vh * .35))) * ws.length;
      ws.forEach((w, k) => { const c = "w" + (k + 1 <= lit ? " on" : k < lit ? " now" : ""); if (w.className !== c) w.className = c; });
    });
  }
  let ticking = false;
  home.addEventListener("scroll", () => {
    if (ticking) return; ticking = true;
    requestAnimationFrame(() => { paintSung(); document.body.classList.toggle("scrolled", home.scrollTop > 40); ticking = false; });
  }, { passive: true });

  /* ---------- última versión desde GitHub ---------- */
  fetch("https://api.github.com/repos/IAOcd/Luma/releases/latest")
    .then(r => r.ok ? r.json() : null)
    .then(d => { if (d && /^v?\d/.test(d.tag_name)) $$(".ver").forEach(el => el.textContent = d.tag_name.startsWith("v") ? d.tag_name : "v" + d.tag_name); })
    .catch(() => {});

  /* ---------- el pulso de una canción: barras y logo que late ---------- */
  const beat = t => Math.pow(Math.max(0, Math.cos((t % 500) / 500 * Math.PI)), 6);
  const level = (t, k, n) => {
    const u = k / n, low = Math.exp(-u * 3.2);
    return Math.min(1, .18 + .45 * Math.abs(Math.sin(t / (520 + k * 11) + k * .9)) * (1 - u * .5) + .5 * beat(t) * low + .12 * Math.sin(t / 230 + k));
  };
  const cv = $("#bars"), cx = cv.getContext("2d"), logo = $("#bigLogo"), smooth = [];
  function drawBars(t) {
    const d = Math.min(devicePixelRatio || 1, 1.5), w = cv.clientWidth, h = cv.clientHeight;
    if (cv.width !== Math.round(w * d) || cv.height !== Math.round(h * d)) { cv.width = Math.round(w * d); cv.height = Math.round(h * d); }
    const W = cv.width, H = cv.height, n = Math.max(24, Math.floor(W / (14 * d))), gap = W / n, bw = gap * .42;
    cx.clearRect(0, 0, W, H);
    for (let k = 0; k < n; k++) {
      const m = Math.abs(k - (n - 1) / 2) / ((n - 1) / 2);
      const target = level(t, Math.round(m * 40), 40) * (1 - m * .35);
      smooth[k] = (smooth[k] || 0) + (target - (smooth[k] || 0)) * .3;
      const bh = smooth[k] * H * .8;
      cx.fillStyle = `rgba(233,230,223,${.08 + smooth[k] * .24})`;
      cx.beginPath();
      if (cx.roundRect) cx.roundRect(k * gap + (gap - bw) / 2, H - bh, bw, bh, bw / 2); else cx.rect(k * gap + (gap - bw) / 2, H - bh, bw, bh);
      cx.fill();
    }
    logo.style.setProperty("--beat", (1 + beat(t) * .035).toFixed(4));
  }

  // el logo mira un poco hacia el mouse
  const wrap = $("#logoWrap");
  if (!reduce && matchMedia("(hover: hover)").matches) {
    addEventListener("pointermove", e => {
      if (current !== "home" || home.scrollTop > innerHeight * .6) return;
      wrap.style.setProperty("--px", ((e.clientX / innerWidth - .5) * 22).toFixed(1) + "px");
      wrap.style.setProperty("--py", ((e.clientY / innerHeight - .5) * 14).toFixed(1) + "px");
    }, { passive: true });
  }

  /* ---------- demos del inicio (solo se mueven cuando se ven) ---------- */
  const seen = new Set();
  const seer = new IntersectionObserver(es => es.forEach(e => e.isIntersecting ? seen.add(e.target) : seen.delete(e.target)), { root: home });

  // botones de estilo: cambian un data-* del ejemplo
  $$(".chips[data-target]").forEach(group => {
    const target = document.getElementById(group.dataset.target);
    group.addEventListener("click", e => {
      const b = e.target.closest("button[data-v]"); if (!b) return;
      $$("button", group).forEach(x => x.setAttribute("aria-pressed", String(x === b)));
      target.dataset[group.dataset.attr] = b.dataset.v;
    });
  });

  // la letra de ejemplo de Lyric Flow, palabra por palabra
  const flow = $("#flow");
  const words = "we left the lights on so the night could find us".split(" ");
  flow.innerHTML = words.map((w, i) => `<span style="--i:${i}">${w}</span>`).join(" ");
  const fw = [...flow.children];
  let fi = reduce ? fw.length : 0;
  const paintFlow = () => fw.forEach((s, k) => { const c = k < fi ? "on" : k === fi ? "now" : ""; if (s.className !== c) s.className = c; });
  paintFlow(); seer.observe(flow);
  if (!reduce) setInterval(() => { if (current !== "home" || !seen.has(flow) || document.hidden) return; fi = (fi + 1) % (fw.length + 4); paintFlow(); }, 420);

  // el visualizador con sus 7 estilos
  const vz = $("#viz"), vx = vz.getContext("2d"), vs = [];
  seer.observe(vz);
  function drawViz(t) {
    const d = Math.min(devicePixelRatio || 1, 1.5), w = vz.clientWidth, h = vz.clientHeight;
    if (vz.width !== Math.round(w * d) || vz.height !== Math.round(h * d)) { vz.width = Math.round(w * d); vz.height = Math.round(h * d); }
    const W = vz.width, H = vz.height, style = vz.dataset.viz || "bars", n = 40;
    for (let k = 0; k < n; k++) { const tg = level(t, k, n); vs[k] = (vs[k] || 0) + (tg - (vs[k] || 0)) * .3; }
    vx.clearRect(0, 0, W, H);
    const ink = a => `rgba(233,230,223,${a})`;
    if (style === "bars" || style === "pillars" || style === "mirror") {
      const m = style === "pillars" ? 20 : n, gap = W / m, bw = gap * (style === "pillars" ? .7 : .5);
      for (let k = 0; k < m; k++) {
        const v = vs[Math.floor(k * n / m)], bh = v * H * (style === "mirror" ? .42 : .72);
        vx.fillStyle = ink(.35 + v * .6);
        const x = k * gap + (gap - bw) / 2;
        if (style === "mirror") vx.fillRect(x, H / 2 - bh, bw, bh * 2);
        else if (style === "pillars") { for (let y = 0; y < bh; y += bw * .9) vx.fillRect(x, H - y - bw * .8, bw, bw * .8); }
        else vx.fillRect(x, H - bh, bw, bh);
      }
    } else if (style === "wave" || style === "layered" || style === "aurora") {
      const layers = style === "layered" ? 3 : 1;
      for (let L = 0; L < layers; L++) {
        vx.beginPath();
        for (let k = 0; k <= 80; k++) {
          const u = k / 80, v = vs[Math.floor(u * (n - 1))];
          const y = H * (style === "aurora" ? .42 : .55) - Math.sin(u * Math.PI * (3 + L) + t / (700 + L * 200)) * v * H * (.18 + L * .05);
          k ? vx.lineTo(u * W, y) : vx.moveTo(0, y);
        }
        if (style === "aurora") {
          vx.lineTo(W, H); vx.lineTo(0, H); vx.closePath();
          const g = vx.createLinearGradient(0, 0, 0, H); g.addColorStop(0, "rgba(140,230,190,.55)"); g.addColorStop(.6, "rgba(120,110,220,.15)"); g.addColorStop(1, "rgba(0,0,0,0)");
          vx.fillStyle = g; vx.fill();
        } else { vx.strokeStyle = ink(.85 - L * .25); vx.lineWidth = 3 * d; vx.stroke(); }
      }
    } else if (style === "rings") {
      const cxp = W / 2, cyp = H / 2;
      for (let r = 0; r < 6; r++) {
        const v = vs[r * 6], rad = (r + 1) * Math.min(W, H) * .07 + v * Math.min(W, H) * .06;
        vx.beginPath(); vx.arc(cxp, cyp, rad, 0, Math.PI * 2);
        vx.strokeStyle = ink(.25 + v * .6); vx.lineWidth = (2 + v * 4) * d; vx.stroke();
      }
    }
  }

  let last = 0;
  function frame(t) {
    if (current === "home" && !document.hidden && t - last > 33) {
      last = t;
      if (home.scrollTop < innerHeight * .9) drawBars(t);
      if (seen.has(vz)) drawViz(t);
    }
    requestAnimationFrame(frame);
  }
  if (reduce) { drawBars(250); drawViz(250); } else requestAnimationFrame(frame);

  // preparar las capturas en segundo plano, para que abrir una sección no dé tirones
  const idle = window.requestIdleCallback || (f => setTimeout(f, 300));
  addEventListener("load", () => {
    const imgs = $$(".screen img");
    const next = () => { const img = imgs.shift(); if (!img) return; img.loading = "eager"; (img.decode ? img.decode() : Promise.resolve()).catch(() => {}).then(() => idle(next)); };
    idle(next);
  });

  /* ---------- líneas de fondo: muy tenues y casi quietas ---------- */
  // Unas pocas curvas orgánicas detrás de todo. No reaccionan al mouse ni al scroll: solo "respiran"
  // muy despacio y toman otra forma al cambiar de sección. Se dibujan ~12 veces por segundo (casi gratis).
  const field = $("#field"), fx = field.getContext("2d");
  const tint = { home: [233, 230, 223], game: [255, 176, 120], launcher: [240, 150, 205], top: [200, 180, 255], me: [233, 230, 223], faq: [175, 195, 240], download: [240, 232, 215], year: [233, 230, 223] };
  const SEG = 110, NP = 9;
  let lines = [], W = 0, H = 0, dpr = 1;
  function shapeFor(view, k) {
    let seed = (view.charCodeAt(0) * 131 + view.length * 17 + k * 977) % 2147483647;
    const rnd = () => (seed = seed * 16807 % 2147483647) / 2147483647;
    return Float32Array.from([.6 + rnd() * 1.4, rnd() * 6.28, 12 + rnd() * 26, 2 + rnd() * 3, rnd() * 6.28, 3 + rnd() * 10, rnd() * 1.2, (rnd() - .5) * .4, .5 + rnd() * .5]);
  }
  function layout() {
    dpr = Math.min(devicePixelRatio || 1, 1.5);
    W = innerWidth; H = innerHeight;
    field.width = Math.round(W * dpr); field.height = Math.round(H * dpr);
    const n = W < 600 ? 4 : 6, view = current || "home";
    lines = [];
    for (let k = 0; k < n; k++) { const s = shapeFor(view, k); lines.push({ y: H * (.12 + .76 * k / (n - 1)), s, to: s.slice() }); }
  }
  function drawField(t) {
    const [r, g, b] = tint[current] || tint.home;
    fx.setTransform(dpr, 0, 0, dpr, 0, 0);
    fx.clearRect(0, 0, W, H);
    fx.lineJoin = "round";
    fx.strokeStyle = `rgba(${r},${g},${b},.08)`;
    fx.lineWidth = 1;
    for (const L of lines) {
      for (let p = 0; p < NP; p++) L.s[p] += (L.to[p] - L.s[p]) * .08;
      const s = L.s, tt = t * .00006 * s[8];                  // muy lento: casi quieto
      fx.beginPath();
      for (let i = 0; i <= SEG; i++) {
        const u = i / SEG, env = .55 + .45 * Math.sin(u * Math.PI * 2 * s[6] + tt);
        const y = L.y + (u - .5) * H * s[7]
          + Math.sin(u * Math.PI * 2 * s[0] + s[1] + tt) * s[2] * env
          + Math.sin(u * Math.PI * 2 * s[3] + s[4] - tt * 1.5) * s[5] * (1.1 - env);
        i ? fx.lineTo(u * W, y) : fx.moveTo(0, y);
      }
      fx.stroke();
    }
  }
  layout(); drawField(0);
  addEventListener("resize", () => { layout(); drawField(performance.now()); });
  if (!reduce) {
    let lastLine = 0;
    const loop = t => { if (!document.hidden && t - lastLine > 80) { lastLine = t; drawField(t); } requestAnimationFrame(loop); };
    requestAnimationFrame(loop);
  }

  /* ---------- luz en los bordes + anillo al hacer clic ---------- */
  // El fondo queda quieto. Lo que reacciona es solo lo que está bajo el mouse: el borde de la tarjeta
  // se ilumina justo cerca del cursor y lo sigue. Un clic deja un anillo pequeño.
  if (!reduce) {
    addEventListener("pointerdown", e => {
      const r = document.createElement("span");
      r.className = "ripple"; r.style.left = e.clientX + "px"; r.style.top = e.clientY + "px";
      document.body.appendChild(r); r.addEventListener("animationend", () => r.remove());
    }, { passive: true });
  }
  if (!reduce && matchMedia("(hover: hover) and (pointer: fine)").matches) {
    const EDGE = ".card, .stage, .corner, .vizbox, .feat-demo.shot, .screen, .answer, .dock, .btn-glass, .picks button, .qlist button";
    let lit = null;
    document.addEventListener("pointermove", e => {
      const el = e.target.closest(EDGE);
      if (lit && lit !== el) lit.classList.remove("lit");
      lit = el; if (!el) return;
      const r = el.getBoundingClientRect();
      el.style.setProperty("--ex", (e.clientX - r.left).toFixed(0) + "px");
      el.style.setProperty("--ey", (e.clientY - r.top).toFixed(0) + "px");
      el.classList.add("lit");
    }, { passive: true });
    document.documentElement.addEventListener("pointerleave", () => { if (lit) lit.classList.remove("lit"); lit = null; });
  }

  /* ---------- cosas que se mueven con el mouse ---------- */
  if (!reduce && matchMedia("(hover: hover) and (pointer: fine)").matches) {
    // botones magnéticos: se acercan un poco al cursor
    const MAG = ".btn, .dock a, .chips button, .gh, .lang";
    document.addEventListener("pointermove", e => {
      const el = e.target.closest(MAG); if (!el) return;
      const r = el.getBoundingClientRect();
      const dx = (e.clientX - (r.left + r.width / 2)) / r.width, dy = (e.clientY - (r.top + r.height / 2)) / r.height;
      el.style.translate = `${(dx * 8).toFixed(1)}px ${(dy * 6).toFixed(1)}px`;
    }, { passive: true });
    document.addEventListener("pointerout", e => {
      const el = e.target.closest(MAG); if (el && !el.contains(e.relatedTarget)) el.style.translate = "";
    });
    // tarjetas y capturas que se inclinan hacia el mouse
    const TILT = ".card, .feat-demo.shot, .stage, .corner, .vizbox, .screen";
    document.addEventListener("pointermove", e => {
      const el = e.target.closest(TILT); if (!el) return;
      const r = el.getBoundingClientRect();
      const dx = (e.clientX - r.left) / r.width - .5, dy = (e.clientY - r.top) / r.height - .5;
      el.style.setProperty("--ry", (dx * 7).toFixed(2) + "deg");
      el.style.setProperty("--rx", (-dy * 7).toFixed(2) + "deg");
      el.classList.add("tilting");
    }, { passive: true });
    document.addEventListener("pointerout", e => {
      const el = e.target.closest(TILT); if (el && !el.contains(e.relatedTarget)) { el.style.removeProperty("--rx"); el.style.removeProperty("--ry"); el.classList.remove("tilting"); }
    });
  }

  /* ---------- arranque ---------- */
  let saved = null;
  try { saved = localStorage.getItem("luma-lang"); } catch (e) {}
  setLang(dict[saved] ? saved : "en", false);
  go(location.hash.slice(1) || "home", "replace");
  addEventListener("resize", () => { placePill(lang, pill, $("button[aria-pressed='true']", lang)); placeDock(); });
  document.fonts && document.fonts.ready.then(() => { placePill(lang, pill, $("button[aria-pressed='true']", lang)); placeDock(); });
})();
