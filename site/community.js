// Luma — the community pages: the style gallery and a shared "Your year".
// They read Luma's server (luma-api), the same one the launcher uses. Nothing
// here needs an account: the gallery and a shared year are public. Publishing,
// reporting and sharing are done from the launcher.
(() => {
  const $ = (s, el = document) => el.querySelector(s);
  const API = /^(localhost|127\.0\.0\.1)$/.test(location.hostname) ? "http://127.0.0.1:8787" : "https://luma-api.gios-app.workers.dev";
  const t = key => { const d = window.LUMA_I18N || {}; const lang = document.documentElement.lang || "en"; return (d[lang] || d.en || {})[key] ?? (d.en || {})[key] ?? key; };
  const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

  /* ---------- letters made of blocks (5 x 7, like Minecraft's) ---------- */
  const GLYPH = {
    L: [16, 16, 16, 16, 16, 16, 31], U: [17, 17, 17, 17, 17, 17, 14], M: [17, 27, 21, 21, 17, 17, 17], A: [14, 17, 17, 31, 17, 17, 17],
    0: [14, 17, 19, 21, 25, 17, 14], 1: [4, 12, 4, 4, 4, 4, 14], 2: [14, 17, 1, 2, 4, 8, 31], 3: [30, 1, 1, 14, 1, 1, 30],
    4: [2, 6, 10, 18, 31, 2, 2], 5: [31, 16, 30, 1, 1, 17, 14], 6: [6, 8, 16, 30, 17, 17, 14], 7: [31, 1, 2, 4, 8, 8, 8],
    8: [14, 17, 17, 14, 17, 17, 14], 9: [14, 17, 17, 15, 1, 2, 12], h: [16, 16, 22, 25, 17, 17, 17], m: [0, 0, 26, 21, 21, 17, 17],
    i: [4, 0, 12, 4, 4, 4, 14], n: [0, 0, 22, 25, 17, 17, 17], ".": [0, 0, 0, 0, 0, 12, 12]
  };
  const glyphW = c => (GLYPH[c] ? 5 : 3);
  const pixelWidth = (text, cell) => Math.max(0, [...text].reduce((w, c) => w + (glyphW(c) + 1) * cell, 0) - cell);
  // Calls "dot(x, y, cell, column)" for each block of the text.
  function eachBlock(text, x0, y0, cell, dot) {
    let x = x0, col = 0;
    for (const c of text) {
      const g = GLYPH[c];
      if (g) for (let r = 0; r < 7; r++) for (let k = 0; k < 5; k++) if (g[r] & (16 >> k)) dot(x + k * cell, y0 + r * cell, cell, col + k);
      x += (glyphW(c) + 1) * cell; col += glyphW(c) + 1;
    }
    return col;
  }

  /* ---------- a Lyric Flow style's preview (an approximation, like the launcher's) ---------- */
  const rgb = (c, a = 1) => `rgba(${Math.round(c[0] * 255)},${Math.round(c[1] * 255)},${Math.round(c[2] * 255)},${a})`;
  const ALBUM = [[0.89, 0.64, 0.49], [0.6, 0.31, 0.53]];  // (an album's colors, for the styles that take them)
  function drawLook(cv, look) {
    const d = Math.min(devicePixelRatio || 1, 2), w = cv.clientWidth || 300, h = cv.clientHeight || 120;
    cv.width = Math.round(w * d); cv.height = Math.round(h * d);
    const x = cv.getContext("2d"); x.setTransform(d, 0, 0, d, 0, 0);
    x.fillStyle = "#08080b"; x.fillRect(0, 0, w, h);
    const L = look || {};
    const c1 = Array.isArray(L.color) ? L.color : [1, 1, 1], c2 = Array.isArray(L.color2) ? L.color2 : [0.7, 0.56, 1];
    const mode = L.colorMode ?? 0, cols = 23;
    const cell = Math.min(w * 0.6 / cols, h * 0.42 / 7), tw = cols * cell, x0 = (w - tw) / 2, y0 = (h - 7 * cell) / 2;
    const colorAt = k => {
      const u = k / (cols - 1);
      if (mode === 1) return ALBUM[0];
      if (mode === 2) return ALBUM[0].map((v, i) => v + (ALBUM[1][i] - v) * u);
      if (mode === 3) return c1.map((v, i) => v + (c2[i] - v) * u);
      if (mode === 4) { const hue = u * 300, f = n => { const k2 = (n + hue / 60) % 6; return 1 - Math.max(0, Math.min(k2, 4 - k2, 1)) * 0.75; }; return [f(5), f(3), f(1)]; }
      return c1;
    };
    const sung = (L.fill ?? 1) > 0 ? 0.72 : 1;
    const glow = (L.glow ?? 0) > 0, glowStrength = L.glowStrength ?? 1;
    const glowColor = k => { const g = L.glowColor ?? 0; return g === 1 ? [1, 1, 1] : g === 2 ? ALBUM[0] : g === 3 && Array.isArray(L.glowCustom) ? L.glowCustom : colorAt(k); };
    // particles
    if ((L.vfx ?? 0) > 0) {
      const n = Math.round(10 * (L.vfxAmount ?? 1));
      for (let i = 0; i < n; i++) {
        const r = Math.abs(Math.sin(i * 12.9898 + (L.vfx || 0) * 78.233)) % 1, r2 = Math.abs(Math.sin(i * 39.3468)) % 1;
        const vc = (L.vfxColor ?? 0) === 0 ? colorAt(Math.floor(r * cols)) : (L.vfxColor === 1 ? [1, 1, 1] : ALBUM[0]);
        x.fillStyle = rgb(vc, 0.5 + 0.5 * r2);
        const s = cell * (0.35 + 0.4 * r2);
        x.fillRect(x0 - cell * 2 + r * (tw + cell * 4), y0 - cell * 2.5 + r2 * (7 * cell + cell * 5), s, s);
      }
    }
    // shadow, outline, then the letters (with their light)
    const shadowOn = !!L.shadow, outline = !!L.outline;
    const oc = Array.isArray(L.outlineColor) ? L.outlineColor : [0.05, 0.05, 0.07], oa = L.outlineAlpha ?? 0.85;
    if (shadowOn) {
      const depth = (L.shadowStyle ?? 0) === 2 ? 3 : 1;
      eachBlock("LUMA", x0, y0, cell, (bx, by, cs, k) => {
        x.fillStyle = (L.shadowStyle ?? 0) === 1 ? rgb(colorAt(k), 0.35) : "rgba(0,0,0,.55)";
        for (let s = 1; s <= depth; s++) x.fillRect(bx + cs * 0.35 * s, by + cs * 0.35 * s, cs, cs);
      });
    }
    if (outline) eachBlock("LUMA", x0, y0, cell, (bx, by, cs) => { x.fillStyle = rgb(oc, oa); x.fillRect(bx - cs * 0.22, by - cs * 0.22, cs * 1.44, cs * 1.44); });
    eachBlock("LUMA", x0, y0, cell, (bx, by, cs, k) => {
      const on = k / (cols - 1) <= sung;
      const c = colorAt(k);
      if (glow && on) { x.shadowColor = rgb(glowColor(k), 0.9); x.shadowBlur = cs * 2.2 * glowStrength; } else x.shadowBlur = 0;
      x.fillStyle = rgb(c, on ? (L.opacity ?? 0.95) : 0.3);
      x.fillRect(bx, by, cs + 0.4, cs + 0.4);
    });
    x.shadowBlur = 0;
  }

  /* ---------- the gallery ---------- */
  const grid = $("#galGrid"), more = $("#galMore"), status = $("#galStatus");
  let sort = "new", next = 0, styles = [], loading = false, loaded = false;
  function card(s) {
    const count = s.installs === 1 ? t("gal.install_one") : t("gal.installs").replace("{n}", s.installs);
    return `<article class="gcard">
      <canvas class="gprev" aria-hidden="true"></canvas>
      <div class="gmeta"><b>${esc(s.name)}</b><span>${esc(s.author || t("gal.anon"))} · ${esc(count)}</span></div>
      <a class="btn btn-glass gopen" href="luma://style/${esc(s.id)}">${esc(t("gal.open"))}</a>
    </article>`;
  }
  function paint() {
    if (!grid) return;
    grid.innerHTML = styles.map(card).join("");
    [...grid.querySelectorAll(".gprev")].forEach((cv, i) => drawLook(cv, styles[i].look));
    more.hidden = next === null || !styles.length;
    status.textContent = loading && !styles.length ? t("gal.loading") : (!loading && loaded && !styles.length ? t("gal.empty") : "");
  }
  async function load(reset) {
    if (loading) return;
    loading = true;
    if (reset) { styles = []; next = 0; }
    paint();
    try {
      const r = await fetch(`${API}/v1/gallery?sort=${sort}&cursor=${next || 0}`);
      if (!r.ok) throw new Error(r.status);
      const data = await r.json();
      styles = styles.concat(data.styles || []);
      next = data.next ?? null;
      loaded = true;
    } catch (e) {
      status.textContent = t("gal.error");
      loading = false;
      return;
    }
    loading = false;
    paint();
  }
  if (grid) {
    $("#galSort").addEventListener("click", e => {
      const b = e.target.closest("button[data-v]"); if (!b || b.dataset.v === sort) return;
      [...$("#galSort").children].forEach(x => x.setAttribute("aria-pressed", String(x === b)));
      sort = b.dataset.v; load(true);
    });
    more.addEventListener("click", () => load(false));
    addEventListener("resize", () => { if (styles.length) [...grid.querySelectorAll(".gprev")].forEach((cv, i) => drawLook(cv, styles[i].look)); });
  }

  /* ---------- a shared "Your year" (the same card as the launcher's picture) ---------- */
  const yrCanvas = $("#yrCanvas"), yrMsg = $("#yrMsg"), yrSave = $("#yrSave"), yrTitle = $("#yrTitle");
  let year = null, yearCode = null;
  const hours = s => s < 3600 ? `${Math.floor(s / 60)} min` : s < 36000 ? `${(s / 3600).toFixed(1)} h` : `${Math.round(s / 3600)} h`;
  function fit(x, text, width) {
    if (x.measureText(text).width <= width) return text;
    let s = text;
    while (s.length && x.measureText(s + "…").width > width) s = s.slice(0, -1);
    return s + "…";
  }
  // The card at 1080 x 1350 ("u" = 1/540 of its width), like the launcher draws it.
  function drawYear(cv, y) {
    const W = 1080, H = 1350, u = W / 540;
    cv.width = W; cv.height = H;
    const x = cv.getContext("2d");
    x.fillStyle = "#000"; x.fillRect(0, 0, W, H);
    const light = (cx, cy, rx, ry, c, a) => {
      x.save(); x.translate(cx, cy); x.scale(1, ry / rx);
      const g = x.createRadialGradient(0, 0, 0, 0, 0, rx); g.addColorStop(0, `rgba(${c},${a})`); g.addColorStop(1, `rgba(${c},0)`);
      x.fillStyle = g; x.beginPath(); x.arc(0, 0, rx, 0, Math.PI * 2); x.fill(); x.restore();
    };
    const stone = "154,143,124", stoneDark = "122,114,102", ivoryC = "233,230,223";
    light(W * .20, H * .16, W * .62, H * .36, stone, .52); light(W * .92, H * .06, W * .46, H * .26, stoneDark, .38);
    light(W * .55, H * .40, W * .70, H * .22, ivoryC, .09); light(W * .08, H * .98, W * .55, H * .30, stoneDark, .30);
    light(W * .95, H * .88, W * .40, H * .26, stone, .19);
    const ivory = "#E9E6DF", dim = "rgba(233,230,223,.59)", faint = "rgba(233,230,223,.32)";
    const font = (wgt, px) => `${wgt} ${px}px Onest, system-ui, sans-serif`;
    const m = u * 38, left = m, right = W - m;
    const pixel = (text, px0, py0, cell, color) => { x.fillStyle = color; eachBlock(text, px0, py0, cell, (bx, by, cs) => x.fillRect(bx, by, cs + .5, cs + .5)); };
    x.textBaseline = "top";
    x.fillStyle = dim; x.font = font(600, u * 15); x.fillText("LUMA", left, u * 40);
    const who = y.name ? y.name : t("yr.someone");
    x.fillStyle = faint; x.font = font(400, u * 12); x.textAlign = "right"; x.fillText(who, right, u * 42); x.textAlign = "left";
    pixel(String(y.year), left, u * 72, u * 8, ivory);
    x.fillStyle = ivory; x.font = font(600, u * 19); x.fillText(t("yr.card_title"), left, u * 138);
    // the hill: a column of blocks per month
    const ground = u * 300, colW = (right - left) / 12, gap = Math.max(1, u * 1.5), blk = (colW - gap) / 3;
    const most = Math.max(0, ...y.months), best = y.months.indexOf(most);
    const letters = t("yr.months");
    for (let i = 0; i < 12; i++) {
      const cx = left + colW * i, rows = most > 0 && y.months[i] > 0 ? Math.max(1, Math.round(y.months[i] / most * 7)) : 0;
      for (let r = 0; r < rows; r++) {
        const tt = rows > 1 ? r / (rows - 1) : 1, a = (i === best ? .22 : .12) + tt * (i === best ? .78 : .48);
        const y1 = ground - r * (blk + gap), y0 = y1 - blk;
        for (let c = 0; c < 3; c++) { x.fillStyle = `rgba(${ivoryC},${a * ((r + c) % 2 ? .86 : 1)})`; x.fillRect(cx + c * (blk + gap * .5), y0, blk - gap * .5, blk); }
      }
      x.fillStyle = i === best ? ivory : faint; x.font = font(400, u * 12); x.textAlign = "center";
      x.fillText(letters[i] || "", cx + (colW - gap) / 2, ground + u * 10); x.textAlign = "left";
    }
    const line = x.createLinearGradient(left, 0, right, 0); line.addColorStop(0, `rgba(${ivoryC},.05)`); line.addColorStop(.5, `rgba(${ivoryC},.25)`); line.addColorStop(1, `rgba(${ivoryC},.05)`);
    x.fillStyle = line; x.fillRect(left, ground + u * 2, right - left, u);
    // the numbers
    let yy = ground + u * 46;
    pixel(hours(y.seconds), left, yy, u * 6, ivory);
    x.fillStyle = dim; x.font = font(400, u * 14); x.fillText(t("yr.card_hours"), left, yy + u * 48);
    let rx = right;
    [[y.artists, "yr.artists"], [y.songs, "yr.songs"], [y.plays, "yr.plays"]].forEach(([v, key]) => {
      const n = String(v), nw = pixelWidth(n, u * 3); x.font = font(400, u * 12); const lw = x.measureText(t(key)).width, cw = Math.max(nw, lw);
      pixel(n, rx - cw + (cw - nw) / 2, yy + u * 12, u * 3, ivory);
      x.fillStyle = dim; x.fillText(t(key), rx - cw + (cw - lw) / 2, yy + u * 40);
      rx -= cw + u * 26;
    });
    yy += u * 86;
    // top songs / top artists
    const g2 = u * 14, cw = (right - left + u * 8 - g2) / 2, l0 = left - u * 4, ch = H - u * 34 - yy;
    [["yr.top_songs", y.topSongs, true], ["yr.top_artists", y.topArtists, false]].forEach(([key, list, songs], col) => {
      const ax = l0 + col * (cw + g2), r = u * 16;
      x.fillStyle = `rgba(${ivoryC},.035)`; x.strokeStyle = `rgba(${ivoryC},.12)`; x.lineWidth = u;
      x.beginPath(); x.roundRect ? x.roundRect(ax, yy, cw, ch, r) : x.rect(ax, yy, cw, ch); x.fill(); x.stroke();
      const px = ax + u * 18;
      x.fillStyle = ivory; x.font = font(600, u * 14); x.fillText(t(key), px, yy + u * 16);
      let ry = yy + u * 46; const rowH = (yy + ch - ry - u * 8) / 5;
      if (!list.length) { x.fillStyle = faint; x.fillText("—", px, ry); }
      list.slice(0, 5).forEach((e, i) => {
        pixel(String(i + 1), px, ry + u * 4, u * 2.4, i === 0 ? ivory : faint);
        const tx = px + u * 22, tw = ax + cw - tx - u * 14;
        x.fillStyle = ivory; x.font = font(600, u * 14); x.fillText(fit(x, songs ? e.title : e.artist, tw), tx, ry);
        x.fillStyle = dim; x.font = font(400, u * 12); x.fillText(fit(x, songs ? (e.artist || hours(e.seconds)) : hours(e.seconds), tw), tx, ry + u * 18);
        ry += rowH;
      });
    });
  }
  function paintYear() {
    if (!yrCanvas) return;
    if (!year) { yrCanvas.hidden = true; yrSave.hidden = true; yrTitle.textContent = t("yr.title"); return; }
    yrCanvas.hidden = false; yrSave.hidden = false; yrMsg.textContent = "";
    yrTitle.textContent = year.name ? t("yr.title_name").replace("{name}", year.name) : t("yr.title");
    drawYear(yrCanvas, year);
  }
  async function loadYear(code) {
    if (!/^[a-z0-9]{10}$/.test(code || "")) { year = null; paintYear(); yrMsg.textContent = t("yr.missing"); return; }
    if (code === yearCode && year) { paintYear(); return; }
    yearCode = code; year = null; paintYear(); yrMsg.textContent = t("gal.loading");
    try {
      const r = await fetch(`${API}/v1/wrapped/${code}`);
      if (r.status === 404) { yrMsg.textContent = t("yr.missing"); return; }
      if (!r.ok) throw new Error(r.status);
      year = await r.json();
      (document.fonts ? document.fonts.ready : Promise.resolve()).then(paintYear);
    } catch (e) { yrMsg.textContent = t("gal.error"); }
  }
  if (yrSave) yrSave.addEventListener("click", () => {
    yrCanvas.toBlob(b => { const a = document.createElement("a"); a.href = URL.createObjectURL(b); a.download = `Luma ${year.year}.png`; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000); }, "image/png");
  });

  /* ---------- when a page opens, or the language changes ---------- */
  document.addEventListener("luma:view", e => {
    const { id, arg } = e.detail;
    if (id === "gallery" && !loaded) load(true);
    if (id === "year") loadYear(arg);
  });
  document.addEventListener("luma:lang", () => { paint(); paintYear(); });
})();
