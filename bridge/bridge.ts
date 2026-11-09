// Hermes Vita bridge: the only door between a PS Vita on the home network and Hermes Agent on this PC.
// Plain HTTP on the LAN (the Vita has no modern TLS), so it is narrow on purpose: a one-time pairing code, a device token that can be
// revoked, a short allow-list of routes, size and rate limits, and an audit line for every write. Hermes' API key never leaves this process.
// Run: bun bridge/bridge.ts pair | serve | devices | revoke <name>
import { createHash, randomBytes, randomInt } from "node:crypto";
import { appendFileSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { networkInterfaces } from "node:os";
import { join } from "node:path";

export type Device = { name: string; hash: string; at: number; last?: number };
type Pair = { code: string; until: number };
export type Deps = { dir: string; now?: () => number; pluginKey?: string; hermesGet?: (path: string) => Promise<any>; speak?: (text: string) => Promise<Uint8Array | null>; hermes?: (messages: { role: string; content: string }[]) => Promise<string>; transcribe?: (wav: Uint8Array) => Promise<string> };

const sha = (s: string) => createHash("sha256").update(s).digest("hex");
const clip = (s: unknown, n: number) => String(s ?? "").replace(/\s+/g, " ").slice(0, n);
const PAIR_TTL = 5 * 60_000, MAX_TEXT = 2000;

export function createBridge(d: Deps) {
  const now = d.now ?? Date.now, devF = join(d.dir, "devices.json"), pairF = join(d.dir, "pair.json"), auditF = join(d.dir, "audit.jsonl");
  mkdirSync(d.dir, { recursive: true });
  const read = <T,>(f: string, dflt: T): T => { try { return JSON.parse(readFileSync(f, "utf8")); } catch { return dflt; } };
  const save = (f: string, v: unknown) => { writeFileSync(`${f}.tmp`, JSON.stringify(v)); renameSync(`${f}.tmp`, f); };
  const audit = (ip: string, who: string, what: string) => appendFileSync(auditF, JSON.stringify({ at: now(), ip, who, what }) + "\n");
  // Hermes Pocket: one short conversation per device with Hermes' own OpenAI-compatible API (the key stays in this process).
  type Chat = { turns: { role: "user" | "assistant"; text: string; at?: number }[]; state: "idle" | "thinking" | "failed"; n: number; said: number; say?: { id: number; pcm: Promise<Uint8Array | null> } };
  const chats = new Map<string, Chat>();
  const chatOf = (n: string) => chats.get(n) ?? chats.set(n, { turns: [], state: "idle", n: 0, said: 0 }).get(n)!;
  // What Hermes does on the PC itself (desktop, CLI, gateway), reported by the hermes-vita plugin. Turns of the API server are the bridge's own and are skipped.
  type Desk = { state: "idle" | "thinking" | "working" | "failed"; tool: string; at: number; turns: { role: "user" | "assistant"; text: string; at: number }[] };
  const desk: Desk = { state: "idle", tool: "", at: 0, turns: [] };
  const DESK_FRESH = 180_000;   // a turn that reports nothing for three minutes is over
  const deskLive = () => desk.state !== "idle" && now() - desk.at < DESK_FRESH;
  const speakAll = (text: string) => { const t = speakable(text); if (!d.speak || !t) return; for (const dev of devices()) { const c = chatOf(dev.name); c.say = { id: ++c.n, pcm: d.speak(t).catch(() => null) }; } };
  /** Loopback only, with the plugin key: the plugin tells the bridge what Hermes is doing. */
  function pluginEvent(ip: string, key: string | null, body: any): { status: number; json: unknown } {
    if (!["127.0.0.1", "::1", "::ffff:127.0.0.1"].includes(ip)) return { status: 403, json: { error: "local only" } };
    if (!key || !d.pluginKey || key !== d.pluginKey) return { status: 401, json: { error: "bad key" } };
    if (clip(body?.platform, 24) === "api_server") return { status: 200, json: { ok: true, skipped: true } };
    const ev = clip(body?.event, 24), t = now();
    if (ev === "turn_start") { desk.state = "thinking"; desk.tool = ""; desk.at = t; const u = clip(body?.text, 600); if (u) desk.turns.push({ role: "user", text: u, at: t }); }
    else if (ev === "tool_start") { desk.state = "working"; desk.tool = clip(body?.tool, 40); desk.at = t; }
    else if (ev === "tool_end") { desk.state = "thinking"; desk.tool = ""; desk.at = t; }
    else if (ev === "turn_end") { desk.state = "idle"; desk.tool = ""; desk.at = t; const a = String(body?.text ?? ""); if (a.trim()) { desk.turns.push({ role: "assistant", text: clip(a, 600), at: t }); speakAll(a); } }
    else if (ev === "error") { desk.state = "failed"; desk.at = t; }
    else if (ev === "stop") { desk.state = "idle"; desk.tool = ""; desk.at = t; }
    else return { status: 400, json: { error: "unknown event" } };
    desk.turns = desk.turns.slice(-20);
    return { status: 200, json: { ok: true } };
  }
  const misses = new Map<string, { n: number; since: number }>();   // wrong pairing codes and tokens per address

  const locked = (ip: string) => { const m = misses.get(ip); return !!m && now() - m.since < 60_000 && m.n >= 5; };
  const miss = (ip: string) => { const m = misses.get(ip); if (!m || now() - m.since >= 60_000) misses.set(ip, { n: 1, since: now() }); else m.n++; };

  /** The operator asks for a code; it works once, for five minutes. */
  const newCode = () => { const code = String(randomInt(0, 1_000_000)).padStart(6, "0"); save(pairF, { code, until: now() + PAIR_TTL } satisfies Pair); return code; };
  const devices = () => read<Device[]>(devF, []);
  const revoke = (name: string) => { const all = devices(), keep = all.filter((x) => x.name !== name); save(devF, keep); return all.length - keep.length; };

  function pair(ip: string, body: any) {
    if (locked(ip)) return { status: 429, json: { error: "too many tries, wait a minute" } };
    const p = read<Pair | null>(pairF, null), code = clip(body?.code, 12), name = clip(body?.name, 40).replace(/[^\w .-]/g, "") || "vita";
    if (!p || now() > p.until || code !== p.code) { miss(ip); audit(ip, "-", "pair refused"); return { status: 401, json: { error: "wrong or expired code" } }; }
    save(pairF, null);                                   // one use
    const token = `vita_${randomBytes(24).toString("base64url")}`;
    save(devF, [...devices().filter((x) => x.name !== name), { name, hash: sha(token), at: now() } satisfies Device]);
    audit(ip, name, "paired");
    return { status: 200, json: { token, name } };
  }

  function who(ip: string, auth: string | null): Device | null {
    if (locked(ip)) return null;
    const t = auth?.toLowerCase().startsWith("bearer ") ? auth.slice(7).trim() : "", h = sha(t), all = devices(), dev = all.find((x) => x.hash === h);
    if (!dev) { miss(ip); return null; }
    dev.last = now(); save(devF, all);
    return dev;
  }

  /** What Hermes says aloud: the first sentences of the answer, no code, links or markup, about 240 characters. */
  const speakable = (a: string) => {
    const t = a.replace(/```[\s\S]*?```/g, " ").replace(/`([^`]*)`/g, "$1").replace(/https?:\/\/\S+/g, " ").replace(/[*_#>|~\[\]]/g, "").replace(/\s+/g, " ").trim();
    if (t.length <= 240) return t;
    const cut = t.slice(0, 240), end = Math.max(cut.lastIndexOf(". "), cut.lastIndexOf("! "), cut.lastIndexOf("? "));
    return end > 80 ? cut.slice(0, end + 1) : cut.replace(/\s+\S*$/, "");
  };
  /** The next spoken answer for this device, once: raw 48 kHz mono 16-bit PCM. 204 = nothing new (or still being made). */
  async function speech(ip: string, auth: string | null): Promise<{ status: number; pcm?: Uint8Array }> {
    const dev = who(ip, auth); if (!dev) return { status: 401 };
    const c = chatOf(dev.name); if (!c.say || c.said === c.say.id) return { status: 204 };
    const got = await Promise.race([c.say.pcm, new Promise<"wait">((r) => setTimeout(() => r("wait"), 5000))]);
    if (got === "wait") return { status: 204 };
    c.said = c.say.id; return got ? { status: 200, pcm: got } : { status: 204 };
  }


  /** One request. Only these routes exist; everything else is a 404. */
  async function handle(method: string, path: string, ip: string, auth: string | null, body: any): Promise<{ status: number; json: unknown }> {
    if (method === "POST" && path === "/v1/pair") return pair(ip, body);
    const dev = who(ip, auth);
    if (!dev) return { status: 401, json: { error: "not paired" } };
    if (method === "POST" && path === "/v1/hermes/ask") {
      const text = clip(body?.text, MAX_TEXT), c = chatOf(dev.name);
      if (!d.hermes) return { status: 503, json: { error: "Hermes API is not configured" } };
      if (!text) return { status: 400, json: { error: "text needed" } };
      if (c.state === "thinking") return { status: 409, json: { error: "Hermes is still answering" } };
      c.turns.push({ role: "user", text, at: now() }); c.turns = c.turns.slice(-20); c.state = "thinking";
      d.hermes(c.turns.map((t) => ({ role: t.role, content: t.text }))).then((a) => { c.turns.push({ role: "assistant", text: a, at: now() }); c.turns = c.turns.slice(-20); c.state = "idle"; const t = speakable(a); if (d.speak && t) c.say = { id: ++c.n, pcm: d.speak(t).catch(() => null) }; }).catch(() => { c.state = "failed"; });
      audit(ip, dev.name, "POST /v1/hermes/ask");
      return { status: 200, json: { state: "thinking" } };
    }
    if (method === "POST" && path === "/v1/hermes/new") { const c = chatOf(dev.name); if (c.state === "thinking") return { status: 409, json: { error: "Hermes is still answering" } }; c.turns = []; desk.turns = []; c.state = "idle"; c.say = undefined; audit(ip, dev.name, "POST /v1/hermes/new"); return { status: 200, json: { ok: true } }; }
    if (method === "GET" && path === "/v1/hermes/sessions") {
      if (!d.hermesGet) return { status: 503, json: { error: "Hermes API is not configured" } };
      try { const j = await d.hermesGet("/api/sessions?limit=12"); return { status: 200, json: (j?.data ?? []).filter((x: any) => !x.hidden && !x.archived && !x.is_internal_child).slice(0, 12).map((x: any) => ({ id: clip(x.id, 60), title: clip(x.title || x.preview || "(untitled)", 60), n: Number(x.message_count) || 0, at: Math.round(Number(x.last_active) || 0) })) }; }
      catch { return { status: 502, json: { error: "Hermes API did not answer" } }; }
    }
    if (method === "POST" && path === "/v1/hermes/open") {   // continue an earlier session: its conversation becomes this device's context
      const id = String(body?.id ?? ""), c = chatOf(dev.name);
      if (!d.hermesGet) return { status: 503, json: { error: "Hermes API is not configured" } };
      if (!/^[\w-]{1,64}$/.test(id)) return { status: 400, json: { error: "bad id" } };
      if (c.state === "thinking") return { status: 409, json: { error: "Hermes is still answering" } };
      try {
        const j = await d.hermesGet(`/api/sessions/${id}/messages`), t = now();
        const turns = (j?.data ?? []).filter((m: any) => (m.role === "user" || m.role === "assistant") && typeof m.content === "string" && m.content.trim() && !m.tool_calls && m.display_kind !== "internal_notification")
          .slice(-20).map((m: any) => ({ role: m.role as "user" | "assistant", text: clip(m.content, 2000), at: t }));
        c.turns = turns; c.state = "idle"; c.say = undefined; audit(ip, dev.name, "POST /v1/hermes/open"); return { status: 200, json: { ok: true, turns: turns.length } };
      } catch { return { status: 502, json: { error: "Hermes API did not answer" } }; }
    }
    if (method === "GET" && path === "/v1/hermes/chat") { const c = chatOf(dev.name); return { status: 200, json: { state: deskLive() ? desk.state : c.state, tool: deskLive() ? desk.tool : "", messages: [...c.turns.map((t) => ({ ...t, at: t.at ?? 0 })), ...desk.turns].sort((a, b) => a.at - b.at).slice(-12).map((t) => ({ role: t.role === "user" ? "user" : "assistant", agent: "hermes", text: clip(t.text, 600), ts: 0 })) } }; }
    return { status: 404, json: { error: "not found" } };
  }
  /** The same data as flat lines for the Vita's tiny C parser: S|state, W|tool, M|role|agent|text, H|id|title|count, END. */
  const flat = (v: string) => clip(v, 600).replace(/\|/g, "/");
  async function lines(method: string, path: string, ip: string, auth: string | null): Promise<{ status: number; text: string }> {
    const m = path.match(/^\/v1\/lines\/(hermes|sessions)$/);
    if (method !== "GET" || !m) return { status: 404, text: "not found\n" };
    const r = await handle("GET", m[1] === "hermes" ? "/v1/hermes/chat" : "/v1/hermes/sessions", ip, auth, {}), j = r.json as any;
    if (r.status !== 200) return { status: r.status, text: `E|${flat(j?.error ?? "error")}\n` };
    if (m[1] === "sessions") return { status: 200, text: [...j.map((x: any) => `H|${flat(x.id)}|${flat(x.title)}|${x.n}`), "END"].join("\n") + "\n" };
    return { status: 200, text: [`S|${j.state}`, `W|${flat(j.tool ?? "")}`, ...j.messages.map((x: any) => `M|${flat(x.role)}|${flat(x.agent)}|${flat(x.text)}`), "END"].join("\n") + "\n" };
  }
  /** Hold-to-talk: a short WAV from the Vita becomes text. The caller (the Vita) then sends it like any typed message. */
  const MAX_WAV = 1_200_000;   // 16 kHz mono 16 bit is about 32 KB a second: roughly 35 seconds
  async function voice(ip: string, auth: string | null, wav: Uint8Array): Promise<{ status: number; json: unknown }> {
    const dev = who(ip, auth);
    if (!dev) return { status: 401, json: { error: "not paired" } };
    if (!d.transcribe) return { status: 503, json: { error: "speech is not configured" } };
    if (wav.length < 2000 || wav.length > MAX_WAV || String.fromCharCode(...wav.slice(0, 4)) !== "RIFF") return { status: 400, json: { error: "a short WAV recording is needed" } };
    try { const text = clip(await d.transcribe(wav), MAX_TEXT); audit(ip, dev.name, `voice ${wav.length} bytes`); return { status: 200, json: { text } }; }
    catch { return { status: 502, json: { error: "could not transcribe" } }; }
  }
  return { handle, voice, speech, pluginEvent, lines, newCode, devices, revoke, setHermes: (f: Deps["hermes"]) => { d.hermes = f; }, setTranscribe: (f: Deps["transcribe"]) => { d.transcribe = f; }, setSpeak: (f: Deps["speak"]) => { d.speak = f; }, setHermesGet: (f: Deps["hermesGet"]) => { d.hermesGet = f; } };
}

if (import.meta.main) {
  const home = process.env.HOME ?? "", dir = process.env.VITA_DATA ?? join(home, ".hermes-vita"); 
  mkdirSync(dir, { recursive: true, mode: 0o700 });
  const keyFile = join(dir, "plugin-key");
  if (!existsSync(keyFile)) writeFileSync(keyFile, randomBytes(24).toString("base64url"), { mode: 0o600 });
  const pluginKey = readFileSync(keyFile, "utf8").trim();
  const b = createBridge({ dir, pluginKey });
  const hermesKey = process.env.HERMES_API_KEY ?? (() => { try { return /^API_SERVER_KEY=(.+)$/m.exec(readFileSync(join(home, ".hermes/.env"), "utf8"))?.[1]?.trim(); } catch { return undefined; } })();
  const hermesUrl = process.env.HERMES_API_URL ?? "http://127.0.0.1:8642";
  if (hermesKey) b.setHermesGet(async (path) => { const r = await fetch(`${hermesUrl}${path}`, { headers: { authorization: `Bearer ${hermesKey}` }, signal: AbortSignal.timeout(10_000) }); if (!r.ok) throw new Error(`hermes ${r.status}`); return r.json(); });
  if (hermesKey) b.setHermes(async (messages) => {
    const r = await fetch(`${hermesUrl}/v1/chat/completions`, { method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${hermesKey}` }, body: JSON.stringify({ model: "hermes-agent", messages }), signal: AbortSignal.timeout(300_000) });
    if (!r.ok) throw new Error(`hermes ${r.status}`);
    return String((await r.json() as any).choices?.[0]?.message?.content ?? "").trim() || "(no answer)";
  });
  // Speech to text: any OpenAI-compatible server (STT_URL, e.g. a local faster-whisper server, or OpenAI).
  if (process.env.STT_URL) b.setTranscribe(async (wav) => {
    const f = new FormData(); f.set("file", new Blob([wav as BlobPart], { type: "audio/wav" }), "voice.wav"); f.set("model", process.env.STT_MODEL ?? "whisper-1");
    const r = await fetch(process.env.STT_URL!, { method: "POST", headers: process.env.STT_KEY ? { authorization: `Bearer ${process.env.STT_KEY}` } : {}, body: f, signal: AbortSignal.timeout(120_000) });
    if (!r.ok) throw new Error(`stt ${r.status}`);
    return String((await r.json() as any).text ?? "").trim();
  });
  // Spoken answers: Piper (a local voice, nothing leaves the PC), then ffmpeg to 48 kHz mono PCM for the Vita. scripts/install-piper.sh sets it up.
  const piperDir = join(home, ".local/share/piper"), piperBin = process.env.PIPER_BIN ?? join(piperDir, "piper/piper"), piperVoice = process.env.PIPER_VOICE ?? join(piperDir, "en_US-hfc_female-medium.onnx");
  if (existsSync(piperBin) && existsSync(piperVoice) && process.env.VITA_SPEAK !== "0") b.setSpeak(async (text) => {
    const p = Bun.spawn([piperBin, "--model", piperVoice, "--output-raw"], { stdin: new TextEncoder().encode(text), stdout: "pipe", stderr: "ignore" });
    const raw = new Uint8Array(await new Response(p.stdout).arrayBuffer()); if (!raw.length) return null;
    const f = Bun.spawn(["ffmpeg", "-v", "error", "-f", "s16le", "-ar", "22050", "-ac", "1", "-i", "-", "-af", "volume=1.6", "-ar", "48000", "-ac", "1", "-f", "s16le", "-"], { stdin: raw, stdout: "pipe", stderr: "ignore" });
    const pcm = new Uint8Array(await new Response(f.stdout).arrayBuffer()); return pcm.length && pcm.length < 1_600_000 ? pcm : null;
  });
  const lanIp = () => { for (const l of Object.values(networkInterfaces())) for (const a of l ?? []) if (a.family === "IPv4" && !a.internal) return a.address; return "<this PC's address>"; };
  const cmd = process.argv[2], port = Number(process.env.VITA_PORT ?? 4410);
  if (cmd === "pair") { const code = b.newCode(); console.log(`Pairing code ${code} (works once, 5 minutes).\nOn the Vita: Hermes Pocket > LINK > X, then type:  ${lanIp()}${port === 4410 ? "" : ":" + port} ${code}`); }
  else if (cmd === "devices") console.log(b.devices().map((x) => `${x.name}  paired ${new Date(x.at).toISOString()}  last ${x.last ? new Date(x.last).toISOString() : "never"}`).join("\n") || "none");
  else if (cmd === "revoke") console.log(`revoked ${b.revoke(process.argv[3] ?? "")}`);
  else if (cmd === "serve") {
    Bun.serve({ port, hostname: process.env.VITA_HOST ?? "0.0.0.0", maxRequestBodySize: 1_300_000, async fetch(req, srv) {
      const ip = srv.requestIP(req)?.address ?? "?", url = new URL(req.url);
      if (req.method === "POST" && url.pathname === "/v1/voice") { const r = await b.voice(ip, req.headers.get("authorization"), new Uint8Array(await req.arrayBuffer())); return Response.json(r.json, { status: r.status }); }
      if (req.method === "GET" && url.pathname === "/v1/hermes/speech/next") { const r = await b.speech(ip, req.headers.get("authorization")); return r.pcm ? new Response(new Blob([r.pcm as BlobPart]), { headers: { "content-type": "audio/pcm" } }) : new Response(null, { status: r.status }); }
      if (Number(req.headers.get("content-length") ?? 0) > 8192) return Response.json({ error: "too large" }, { status: 413 });
      const body = req.method === "POST" ? await req.json().catch(() => ({})) : {};
      if (req.method === "POST" && url.pathname === "/_plugin/event") { const r = b.pluginEvent(ip, req.headers.get("x-hermes-vita-key"), body); return Response.json(r.json, { status: r.status }); }
      if (url.pathname.startsWith("/v1/lines/")) { const r = await b.lines(req.method, url.pathname, ip, req.headers.get("authorization")); return new Response(r.text, { status: r.status, headers: { "content-type": "text/plain" } }); }
      try { const r = await b.handle(req.method, url.pathname, ip, req.headers.get("authorization"), body); return Response.json(r.json, { status: r.status }); }
      catch { return Response.json({ error: "bridge error" }, { status: 500 }); }
    } });
    console.log(`Hermes Vita bridge on :${port}. Hermes API ${hermesKey ? "on" : "NOT configured (set API_SERVER_KEY in ~/.hermes/.env)"}; speech to text ${process.env.STT_URL ? "on" : "off (set STT_URL)"}; spoken answers ${existsSync(piperBin) && existsSync(piperVoice) && process.env.VITA_SPEAK !== "0" ? "on" : "off (scripts/install-piper.sh)"}.`);
  } else console.log("usage: bun bridge/bridge.ts pair | serve | devices | revoke <name>");
}
