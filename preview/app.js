/* Aura — дизайн-превью. Переключение экранов + живой запрос к Python AI Service. */

const screens = document.querySelectorAll(".screen");
const railButtons = document.querySelectorAll(".rail-btn");

function show(name) {
  screens.forEach((s) => s.classList.toggle("is-active", s.id === `screen-${name}`));
  railButtons.forEach((b) => b.classList.toggle("is-active", b.dataset.screen === name));
}

railButtons.forEach((b) => b.addEventListener("click", () => show(b.dataset.screen)));
document.querySelectorAll("[data-goto]").forEach((el) =>
  el.addEventListener("click", () => show(el.dataset.goto)));

/* ------------------------------------------------------------------ тосты */
const toast = document.getElementById("toast");
let toastTimer = null;
function notify(message, isError = false) {
  toast.textContent = message;
  toast.classList.toggle("is-error", isError);
  toast.classList.add("is-visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("is-visible"), 3600);
}

/* ------------------------------------------------------------------ чипы/тумблеры */
document.addEventListener("click", (e) => {
  const chip = e.target.closest(".chip");
  if (chip) chip.classList.toggle("is-on");
  const sw = e.target.closest(".switch");
  if (sw) sw.classList.toggle("is-on");
});

/* --------------------------------------------------------------- пузыри чата */
const chatLog = document.getElementById("chat-log");
function appendBubble(text, kind = "mine") {
  const wrap = document.createElement("div");
  wrap.className = `bubble-wrap ${kind}`;
  const time = new Date().toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit" });
  const badge = kind === "agent" ? '<div class="agent-badge"><i></i>Аура</div>' : "";
  wrap.innerHTML = `${badge}<div class="bubble"></div><div class="meta-line">${time}</div>`;
  wrap.querySelector(".bubble").textContent = text;
  chatLog.appendChild(wrap);
  chatLog.scrollTop = chatLog.scrollHeight;
}

const composer = document.getElementById("composer");
document.getElementById("send-btn").addEventListener("click", () => {
  const text = composer.value.trim();
  if (!text) return;
  appendBubble(text, "mine");
  composer.value = "";
});

/* --------------------------------------------------- таймлайн переговоров */
const timeline = document.getElementById("neg-timeline");
function renderTimeline(steps, a2a) {
  if (!timeline) return;
  timeline.innerHTML = "";
  const lines = [];
  (steps || []).forEach((s, i) => lines.push({ n: i + 1, text: s.title, done: true }));
  if (a2a && a2a.place) lines.push({ n: lines.length + 1, text: `Выбрано место: ${a2a.place.name}`, done: true });
  lines.push({ n: lines.length + 1, text: "Ожидание подтверждения…", done: false });
  lines.slice(0, 6).forEach((l) => {
    const item = document.createElement("div");
    item.className = "tl-item";
    item.innerHTML = `<div class="tl-dot ${l.done ? "is-done" : ""}">${l.done ? "✓" : l.n}</div><div class="tl-card"></div>`;
    item.querySelector(".tl-card").textContent = l.text;
    timeline.appendChild(item);
  });
}

/* ------------------------------------------------ живой запрос к AI-сервису */
const agentInput = document.getElementById("agent-input");
const homeInput = document.getElementById("home-input");
const agentButton = document.getElementById("agent-btn");
const agentStatus = document.getElementById("agent-status");

const context = {
  user_id: 1, email: "anna@example.com", display_name: "Анна",
  preferences: { diet: ["vegan"], city: "Керкраде", budget_limit: 3, lat: 50.861, lon: 6.064, preferred_hours: [10, 11, 12, 16, 17, 18] },
  memory: [ { kind: "preference", text: "Люблю тихие кофейни", weight: 2 }, { kind: "fact", text: "Не ем мясо", weight: 1.5 } ],
};
const peer = { user_id: 2, email: "anya@example.com", display_name: "Аня", preferences: { diet: ["gluten_free"], city: "Херлен", lat: 50.888, lon: 5.978 } };

async function askAura(message) {
  if (!message) { notify("Опишите, о чём договориться", true); return; }
  agentButton.classList.add("is-busy"); agentButton.disabled = true;
  agentStatus.textContent = "Аура думает… (POST /v1/agent/run)";
  try {
    const response = await fetch("/v1/agent/run", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ context: { ...context, message }, peers: [peer], execute: true }),
    });
    if (!response.ok) throw new Error(`AI-сервис ответил ${response.status}`);
    const data = await response.json();

    show("chat");
    appendBubble(data.reply || "Готово.", "agent");

    renderTimeline(data.plan && data.plan.steps, data.a2a);
    const slot = data.a2a && data.a2a.slots && data.a2a.slots[0];
    const place = data.a2a && data.a2a.place ? data.a2a.place.name : "";
    const parts = [`намерение: ${data.intent}`, `действий: ${(data.actions || []).length}`,
      `выполнено: ${(data.results || []).filter((r) => r.ok).length}`];
    if (slot) parts.push(`слот: ${new Date(slot.start).toLocaleString("ru-RU")}`);
    if (place) parts.push(`место: ${place}`);
    agentStatus.textContent = parts.join(" · ");
    const neg = document.getElementById("neg-result");
    if (neg && slot) neg.textContent = `${new Date(slot.start).toLocaleString("ru-RU", { weekday: "long", hour: "2-digit", minute: "2-digit" })} — ${place || ""}`;
    notify(data.reply || "Аура выполнила задачу");
  } catch (error) {
    agentStatus.textContent = `Ошибка: ${error.message}`;
    notify(`AI-сервис недоступен: ${error.message}`, true);
  } finally {
    agentButton.classList.remove("is-busy"); agentButton.disabled = false;
  }
}

agentButton.addEventListener("click", () => askAura(agentInput.value.trim()));
document.getElementById("home-ask").addEventListener("click", () => askAura(homeInput.value.trim()));

document.getElementById("login-btn").addEventListener("click", () => {
  notify("Это дизайн-превью: настоящий вход — в Qt-клиенте");
  show("home");
});
