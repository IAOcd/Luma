// Luma — "Top in Luma" (the songs most heard, anonymous) and a shared "Your
// year". They read Luma's server (luma-api), the same one the launcher uses.
// Nothing here needs an account: both are public by choice of whoever shares.
// Sharing, and joining the charts, are done from the launcher.
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

  /* ---------- Top in Luma: the songs most heard (anonymous, 3 people at least) ---------- */
  const topSongs = $("#topSongs"), topArtists = $("#topArtists"), topStatus = $("#topStatus"), topLead = $("#topLead");
  let period = "month", chart = null, chartFor = null, chartBusy = false;
  const people = n => n === 1 ? t("top.person") : t("top.people").replace("{n}", n);
  function row(rank, main, sub, n) {
    return `<li><span class="trank">${rank}</span><span class="tname"><b>${esc(main)}</b>${sub ? `<span>${esc(sub)}</span>` : ""}</span><span class="tcount">${esc(people(n))}</span></li>`;
  }
  function paintTop() {
    if (!topSongs) return;
    if (!chart) { topSongs.innerHTML = ""; topArtists.innerHTML = ""; return; }
    topSongs.innerHTML = chart.songs.map((e, i) => row(i + 1, e.title, e.artist, e.listeners)).join("");
    topArtists.innerHTML = chart.artists.map((e, i) => row(i + 1, e.artist, "", e.listeners)).join("");
    topLead.textContent = chart.people > 0 ? t("top.lead_n").replace("{n}", chart.people) : t("top.lead");
    topStatus.textContent = chart.songs.length || chart.artists.length ? "" : t("top.empty");
  }
  async function loadTop() {
    if (chartBusy || chartFor === period) return;
    chartBusy = true; chart = null; paintTop(); topStatus.textContent = t("gal.loading");
    try {
      const r = await fetch(`${API}/v1/charts?period=${period}`);
      if (!r.ok) throw new Error(r.status);
      chart = await r.json(); chartFor = period;
    } catch (e) { topStatus.textContent = t("gal.error"); chartBusy = false; return; }
    chartBusy = false; paintTop();
  }
  if (topSongs) $("#topPeriod").addEventListener("click", e => {
    const b = e.target.closest("button[data-v]"); if (!b || b.dataset.v === period) return;
    [...$("#topPeriod").children].forEach(x => x.setAttribute("aria-pressed", String(x === b)));
    period = b.dataset.v; loadTop();
  });

  /* ---------- a shared "Your year" (the same card as the launcher's picture) ---------- */
  const yrCanvas = $("#yrCanvas"), yrMsg = $("#yrMsg"), yrSave = $("#yrSave"), yrTitle = $("#yrTitle"), yrEmpty = $("#yrEmpty"), yrLead = $("#yrLead");
  let yrNoLink = false;
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
    yrEmpty.hidden = !yrNoLink;
    yrLead.textContent = t(yrNoLink ? "yr.empty_lead" : "yr.lead");
    if (!year) { yrCanvas.hidden = true; yrSave.hidden = true; yrTitle.textContent = t(yrNoLink ? "yr.empty_title" : "yr.title"); return; }
    yrCanvas.hidden = false; yrSave.hidden = false; yrMsg.textContent = "";
    yrTitle.textContent = year.name ? t("yr.title_name").replace("{name}", year.name) : t("yr.title");
    drawYear(yrCanvas, year);
  }
  async function loadYear(code) {
    yrNoLink = !code;
    if (!code) { year = null; yearCode = null; yrMsg.textContent = ""; paintYear(); return; }
    if (!/^[a-z0-9]{10}$/.test(code)) { year = null; paintYear(); yrMsg.textContent = t("yr.missing"); return; }
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
  // Paste a link (…/y/<code>, …#year/<code>) or just the code
  if ($("#yrPaste")) $("#yrPaste").addEventListener("submit", e => {
    e.preventDefault();
    const m = $("#yrInput").value.trim().match(/(?:\/y\/|#year\/|^)([a-z0-9]{10})(?:[^a-z0-9]|$)/i);
    if (!m) { yrMsg.textContent = t("yr.bad_link"); return; }
    location.hash = "#year/" + m[1].toLowerCase();
  });
  if (yrSave) yrSave.addEventListener("click", () => {
    yrCanvas.toBlob(b => { const a = document.createElement("a"); a.href = URL.createObjectURL(b); a.download = `Luma ${year.year}.png`; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000); }, "image/png");
  });

  /* ---------- when a page opens, or the language changes ---------- */
  document.addEventListener("luma:view", e => {
    const { id, arg } = e.detail;
    if (id === "top") loadTop();
    if (id === "year") loadYear(arg);
  });
  document.addEventListener("luma:lang", () => {
    document.querySelectorAll("[data-i18n-ph]").forEach(el => { el.placeholder = t(el.dataset.i18nPh); });
    paintTop(); paintYear();
  });
})();
