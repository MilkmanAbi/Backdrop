// Backdrop app: talks to backdrop-ui.exe (Window.this.assetInterface), which relays wallpaper / filter / icon commands
// to the running Backdrop core and handles widgets itself.
const api = Window.this.assetInterface;
const $ = (s) => document.$(s);
const root = document.documentElement;

let state = { running: false };
let widgets = { enabled: false, items: [], gallery: [] };

// ---------------------------------------------------------------- theme
function applyTheme() {
  const t = JSON.parse(api.theme());
  root.classList.toggle("dark", t.dark);
  root.style.variable("accent", t.accent);
}

// ---------------------------------------------------------------- navigation
function show(page) {
  for (const b of document.$$("nav button[data-page]")) b.classList.toggle("current", b.attributes["data-page"] === page);
  for (const p of document.$$("section.page")) p.classList.toggle("current", p.id === page);
}
document.on("click", "nav button[data-page]", (e, b) => show(b.attributes["data-page"]));

// ---------------------------------------------------------------- helpers
const setSwitch = (el, on) => el.classList.toggle("on", !!on);
const isOn = (el) => el.classList.contains("on");
const hhmm = (m) => String(Math.floor(m / 60)).padStart(2, "0") + ":" + String(m % 60).padStart(2, "0");
function send(cmd) { api.send(cmd); refresh(); setTimeout(refresh, 400); }   // some changes land a moment later (Explorer)

// ---------------------------------------------------------------- core state
function refresh() {
  state = JSON.parse(api.state());
  root.classList.toggle("offline", !state.running);
  for (const el of document.$$("#wallpaper button, #wallpaper select, #filter button, #filter select, #filter input, #icons button"))
    el.state.disabled = !state.running;
  if (!state.running) return;

  const wp = state.wallpaper;
  setSwitch($("#wpOn"), wp.on);
  $("#wpFile").textContent = wp.file ? api.fileName(wp.file) : "No video chosen yet";
  $("#wpFit").value = String(wp.fit);
  $("#wpPrep").hidden = wp.preparing < 0;
  $("#wpPct").textContent = wp.preparing >= 0 ? wp.preparing + "%" : "";

  const f = state.filter;
  setSwitch($("#fOn"), f.on);
  $("#fKey").textContent = f.hotkey ? "Tints the whole screen · " + f.hotkey : "Tints the whole screen";
  const chips = $("#fTypes");
  if (chips.children.length !== f.names.length)
    chips.innerHTML = f.names.map((n, i) => `<button data-type="${i}">${n}</button>`).join("");
  for (const c of chips.children) c.classList.toggle("on", +c.attributes["data-type"] === f.type);
  if (!sliding) { $("#fInt").value = f.intensity; $("#fPct").textContent = f.intensity + "%"; }
  setSwitch($("#fLogin"), f.atLogin);
  setSwitch($("#fSched"), f.schedule);
  $("#fFrom").value = String(f.from - f.from % 15);
  $("#fTo").value = String(f.to - f.to % 15);
  for (const s of document.$$("#fFrom, #fTo")) s.state.disabled = !f.schedule;

  const b = state.blur;
  setSwitch($("#bOn"), b.on);
  $("#bKey").textContent = "Softens the whole screen, together with any filter" + (b.hotkey ? " · " + b.hotkey : "");
  if (!blurSliding) { $("#bStr").value = b.strength; $("#bPct").textContent = b.strength + "%"; }

  setSwitch($("#iOn"), state.icons.shown);
  $("#iKey").textContent = "Same as right-click desktop › View › Show desktop icons" + (state.icons.hotkey ? " · " + state.icons.hotkey : "");
}

// ---------------------------------------------------------------- wallpaper
$("#wpOn").on("click", () => send("wallpaper=" + (isOn($("#wpOn")) ? "off" : "on")));
$("#wpChoose").on("click", () => api.send("choose"));
$("#wpFit").on("change", () => send("fit=" + $("#wpFit").value));
$("#startCore").on("click", () => { api.startCore(); setTimeout(refresh, 1200); });

// ---------------------------------------------------------------- colour filter
let sliding = false, slideTimer = 0;
$("#fOn").on("click", () => send("filter=" + (isOn($("#fOn")) ? "off" : "on")));
$("#fTypes").on("click", "button", (e, b) => send("filter.type=" + b.attributes["data-type"]));
$("#fInt").on("input", () => {   // live while dragging; settle the state afterwards
  sliding = true;
  $("#fPct").textContent = $("#fInt").value + "%";
  api.send("filter.intensity=" + $("#fInt").value);
  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => { sliding = false; refresh(); }, 400);
});
$("#fLogin").on("click", () => send("filter.atlogin=" + (isOn($("#fLogin")) ? "0" : "1")));
$("#fSched").on("click", () => send("filter.schedule=" + (isOn($("#fSched")) ? "0" : "1")));
for (const id of ["#fFrom", "#fTo"]) {
  const sel = $(id);
  let html = "";
  for (let m = 0; m < 1440; m += 15) html += `<option value="${m}">${hhmm(m)}</option>`;
  sel.innerHTML = html;
  sel.on("change", () => send((id === "#fFrom" ? "filter.from=" : "filter.to=") + sel.value));
}
$("#fPanel").on("click", () => api.send("filter.panel"));

// ---------------------------------------------------------------- blur
let blurSliding = false, blurTimer = 0;
$("#bOn").on("click", () => send("blur=" + (isOn($("#bOn")) ? "off" : "on")));
$("#bStr").on("input", () => {   // live while dragging (the core turns it on and caps it)
  blurSliding = true;
  $("#bPct").textContent = $("#bStr").value + "%";
  api.send("blur.strength=" + $("#bStr").value);
  if (!isOn($("#bOn"))) api.send("blur=on");
  clearTimeout(blurTimer);
  blurTimer = setTimeout(() => { blurSliding = false; refresh(); }, 400);
});

// ---------------------------------------------------------------- desktop icons
$("#iOn").on("click", () => { setSwitch($("#iOn"), !isOn($("#iOn"))); api.send("icons=toggle"); setTimeout(refresh, 600); });

// ---------------------------------------------------------------- widgets
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

function refreshWidgets() {
  widgets = JSON.parse(api.widgets());
  setSwitch($("#wOn"), widgets.enabled);
  setSwitch($("#wLogin"), widgets.atLogin);
  for (const id of ["#wLogin", "#wEdit"]) $(id).state.disabled = !widgets.enabled;
  $("#wList").innerHTML = widgets.items.map((w) => `
    <div class="row" data-id="${esc(w.id)}">
      <i>${w.builtin ? "&#xF0E2;" : "&#xE8A5;"}</i>
      <div class="text"><div class="title">${esc(w.name || w.id)}</div>
        <div class="sub${w.missing ? " missing" : ""}">${w.missing ? "File not found" : w.builtin ? "Built in" : "Added by you"}</div></div>
      <button class="remove" title="Remove"><i>&#xE74D;</i></button>
      <button class="switch${w.enabled ? " on" : ""}"><span></span></button>
    </div>`).join("");
  $("#wGallery").innerHTML = widgets.gallery.map((g) => `
    <div class="tile" data-id="${esc(g.id)}"><div class="title">${esc(g.name)}</div><button>Add</button></div>`).join("");
}

$("#wOn").on("click", () => { api.widgetsEnable(!isOn($("#wOn"))); refreshWidgets(); });
$("#wLogin").on("click", () => { api.widgetsAtLogin(!isOn($("#wLogin"))); refreshWidgets(); });
$("#wEdit").on("click", () => { api.widgetsArrange(); });
$("#wList").on("click", "button.switch", (e, b) => { api.widgetEnable(b.parentElement.attributes["data-id"], !isOn(b)); refreshWidgets(); });
$("#wList").on("click", "button.remove", (e, b) => { api.widgetRemove(b.parentElement.attributes["data-id"]); refreshWidgets(); });
$("#wGallery").on("click", "button", (e, b) => { api.widgetAddBuiltin(b.parentElement.attributes["data-id"]); refreshWidgets(); });
$("#wAdd").on("click", () => {
  const r = api.widgetAddFile();
  if (r && r[0] === "!") Window.this.modal(<alert caption="Backdrop">{r.slice(1)}</alert>);
  refreshWidgets();
});
$("#wFolder").on("click", () => api.widgetsFolder());

// ---------------------------------------------------------------- about
$("#aVer").textContent = "Version " + api.version();
$("#aGit").on("click", () => api.openLink("https://github.com/MilkmanAbi/Backdrop"));
$("#aSciter").on("click", () => api.openLink("https://sciter.com/"));

// ---------------------------------------------------------------- start
applyTheme();
refresh();
refreshWidgets();
Window.this.on("activate", (e) => { if (e.reason) { applyTheme(); refresh(); refreshWidgets(); } });   // changed elsewhere (tray, hotkeys, a widget's menu)
