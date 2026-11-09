import { expect, test } from "bun:test";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createBridge } from "../bridge/bridge";

function make(t = { v: 1_000_000 }) {
  const dir = mkdtempSync(join(tmpdir(), "vita-"));
  const b = createBridge({ dir, pluginKey: "k1", now: () => t.v });
  return { b, dir, t };
}
async function paired(b: ReturnType<typeof make>["b"]) {
  const code = b.newCode(); const r = await b.handle("POST", "/v1/pair", "10.0.0.5", null, { code, name: "vita 1" });
  return `Bearer ${(r.json as any).token}`;
}

test("a code pairs once; a second use and a wrong code are refused", async () => {
  const { b } = make(); const code = b.newCode();
  expect((await b.handle("POST", "/v1/pair", "1.1.1.1", null, { code, name: "v" })).status).toBe(200);
  expect((await b.handle("POST", "/v1/pair", "1.1.1.1", null, { code, name: "v" })).status).toBe(401);
});
test("the code expires after five minutes", async () => {
  const { b, t } = make(); const code = b.newCode(); t.v += 5 * 60_000 + 1;
  expect((await b.handle("POST", "/v1/pair", "1.1.1.1", null, { code, name: "v" })).status).toBe(401);
});
test("five wrong guesses lock the address for a minute, even with the right code", async () => {
  const { b, t } = make(); const code = b.newCode();
  for (let i = 0; i < 5; i++) await b.handle("POST", "/v1/pair", "9.9.9.9", null, { code: "000000", name: "v" });
  expect((await b.handle("POST", "/v1/pair", "9.9.9.9", null, { code, name: "v" })).status).toBe(429);
  t.v += 61_000; expect((await b.handle("POST", "/v1/pair", "9.9.9.9", null, { code, name: "v" })).status).toBe(200);
});
test("no token, a wrong token and a revoked device get nothing; the token is stored hashed", async () => {
  const { b, dir } = make(); const auth = await paired(b);
  expect((await b.handle("GET", "/v1/hermes/chat", "10.0.0.5", null, {})).status).toBe(401);
  expect((await b.handle("GET", "/v1/hermes/chat", "10.0.0.5", "Bearer nope", {})).status).toBe(401);
  expect(readFileSync(join(dir, "devices.json"), "utf8")).not.toContain(auth.slice(7));
  expect((await b.handle("GET", "/v1/hermes/chat", "10.0.0.5", auth, {})).status).toBe(200);   // a paired device reads the chat
  expect(b.revoke("vita 1")).toBe(1);
  expect((await b.handle("GET", "/v1/hermes/chat", "10.0.0.5", auth, {})).status).toBe(401);
});
test("HermesVita: ask starts a turn, the chat shows thinking then the answer; a second ask while thinking is refused; a failure shows as failed", async () => {
  let release!: (a: string) => void, fail!: () => void, seen: unknown[] = [];
  const { b } = make(); const auth = await paired(b);
  expect((await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "hi" })).status).toBe(503);   // no Hermes API set
  b.setHermes((m) => { seen = m; return new Promise((res, rej) => { release = res; fail = () => rej(new Error("x")); }); });
  expect((await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "hello hermes" })).json).toEqual({ state: "thinking" });
  expect((await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "again" })).status).toBe(409);
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|thinking\nW|\nM|user|hermes|hello hermes\nEND\n");
  release("Hello there"); await new Promise((r) => setTimeout(r, 5));
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nM|user|hermes|hello hermes\nM|assistant|hermes|Hello there\nEND\n");
  expect(seen).toEqual([{ role: "user", content: "hello hermes" }]);
  await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "more" }); fail(); await new Promise((r) => setTimeout(r, 5));
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text.startsWith("S|failed\n")).toBe(true);
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", null)).status).toBe(401);
});
test("voice: a short WAV from a paired device becomes text; no token, no speech, bad or oversized audio are refused", async () => {
  const { b } = make(); const auth = await paired(b);
  const wav = new Uint8Array(4000); wav.set([82, 73, 70, 70]);   // "RIFF"
  expect((await b.voice("10.0.0.5", auth, wav)).status).toBe(503);
  b.setTranscribe(async (w) => `got ${w.length} bytes`);
  expect(await b.voice("10.0.0.5", auth, wav)).toEqual({ status: 200, json: { text: "got 4000 bytes" } });
  expect((await b.voice("10.0.0.5", null, wav)).status).toBe(401);
  expect((await b.voice("10.0.0.5", auth, new Uint8Array(4000))).status).toBe(400);            // not a RIFF file
  expect((await b.voice("10.0.0.5", auth, new Uint8Array(1_300_000))).status).toBe(400);       // too long
  b.setTranscribe(async () => { throw new Error("x"); });
  expect((await b.voice("10.0.0.5", auth, wav)).status).toBe(502);
});

test("a spoken answer is made once per answer, without code or links, and needs the pairing token", async () => {
  const { b } = make(); const auth = await paired(b); const said: string[] = [];
  b.setHermes(async () => "Here is the plan. ```ts\nconst x = 1\n``` See https://example.com for more. **Done.**");
  b.setSpeak(async (t) => { said.push(t); return new Uint8Array([1, 2, 3, 4]); });
  expect((await b.speech("10.0.0.5", "Bearer wrong")).status).toBe(401);
  expect((await b.speech("10.0.0.5", auth)).status).toBe(204);                       // nothing yet
  await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "plan?" }); await new Promise((r) => setTimeout(r, 5));
  expect(said).toEqual(["Here is the plan. See for more. Done."]);
  const first = await b.speech("10.0.0.5", auth); expect(first.status).toBe(200); expect(first.pcm?.length).toBe(4);
  expect((await b.speech("10.0.0.5", auth)).status).toBe(204);                       // delivered once
});

test("the plugin reports what Hermes does on the PC: local and keyed only, API server turns skipped, the Vita sees state, tool and the turn", async () => {
  const { b } = make(); const auth = await paired(b); const said: string[] = [];
  b.setSpeak(async (t) => { said.push(t); return new Uint8Array([1, 2, 3, 4]); });
  expect(b.pluginEvent("10.0.0.9", "k1", { event: "turn_start" }).status).toBe(403);      // not from the LAN
  expect(b.pluginEvent("127.0.0.1", "nope", { event: "turn_start" }).status).toBe(401);
  expect(b.pluginEvent("127.0.0.1", "k1", { event: "turn_start", platform: "api_server", text: "mine" }).json).toEqual({ ok: true, skipped: true });
  b.pluginEvent("127.0.0.1", "k1", { event: "turn_start", platform: "desktop", text: "fix the build" });
  b.pluginEvent("127.0.0.1", "k1", { event: "tool_start", tool: "terminal" });
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|working\nW|terminal\nM|user|hermes|fix the build\nEND\n");
  b.pluginEvent("127.0.0.1", "k1", { event: "tool_end" }); b.pluginEvent("127.0.0.1", "k1", { event: "turn_end", platform: "desktop", text: "Fixed it. All green." });
  await new Promise((r) => setTimeout(r, 5));
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nM|user|hermes|fix the build\nM|assistant|hermes|Fixed it. All green.\nEND\n");
  expect(said).toEqual(["Fixed it. All green."]); expect((await b.speech("10.0.0.5", auth)).status).toBe(200);
  expect(b.pluginEvent("127.0.0.1", "k1", { event: "nonsense" }).status).toBe(400);
});

test("a new chat clears this device's conversation; sessions list; opening one loads its turns as context", async () => {
  const { b } = make(); const auth = await paired(b);
  expect((await b.handle("GET", "/v1/hermes/sessions", "10.0.0.5", auth, {})).status).toBe(503);
  b.setHermesGet(async (p) => p.startsWith("/api/sessions?") ? { data: [{ id: "api-1", title: "Plan the week", message_count: 4, last_active: 5 }, { id: "x", hidden: true }] }
    : { data: [{ role: "user", content: "hi" }, { role: "assistant", content: null, tool_calls: [{}] }, { role: "assistant", content: "hello" }, { role: "tool", content: "out" }] });
  expect((await b.lines("GET", "/v1/lines/sessions", "10.0.0.5", auth)).text).toBe("H|api-1|Plan the week|4\nEND\n");
  expect((await b.handle("POST", "/v1/hermes/open", "10.0.0.5", auth, { id: "../etc" })).status).toBe(400);
  expect((await b.handle("POST", "/v1/hermes/open", "10.0.0.5", auth, { id: "api-1" })).json).toEqual({ ok: true, turns: 2 });
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nM|user|hermes|hi\nM|assistant|hermes|hello\nEND\n");
  expect((await b.handle("POST", "/v1/hermes/new", "10.0.0.5", auth, {})).json).toEqual({ ok: true });
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nEND\n");
});

test("lines view: flat text for the Vita, bars in text are replaced, END closes it, no token no lines", async () => {
  const { b } = make(); const auth = await paired(b);
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", null)).status).toBe(401);
  expect((await b.lines("GET", "/v1/lines/overview", "10.0.0.5", auth)).status).toBe(404);   // only the Hermes views exist
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nEND\n");
  b.setHermes(async () => "a | b"); await b.handle("POST", "/v1/hermes/ask", "10.0.0.5", auth, { text: "x|y" }); await new Promise((r) => setTimeout(r, 5));
  expect((await b.lines("GET", "/v1/lines/hermes", "10.0.0.5", auth)).text).toBe("S|idle\nW|\nM|user|hermes|x/y\nM|assistant|hermes|a / b\nEND\n");
});

test("the app can update from this PC: build id and size, the file once paired, nothing without a token", async () => {
  const { b, dir } = make(); const auth = await paired(b); const dist = mkdtempSync(join(tmpdir(), "dist-"));
  const b2 = createBridge({ dir, pluginKey: "k1", dist }); // same devices file, a dist folder
  expect((await b2.lines("GET", "/v1/lines/app", "10.0.0.5", auth)).status).toBe(404);   // nothing built yet
  writeFileSync(join(dist, "HermesVita.build"), "20261109-120000\n"); writeFileSync(join(dist, "HermesVita.vpk"), new Uint8Array([80, 75, 3, 4]));
  expect((await b2.lines("GET", "/v1/lines/app", "10.0.0.5", auth)).text).toBe("V|20261109-120000|4\nEND\n");
  expect((await b2.lines("GET", "/v1/lines/app", "10.0.0.5", null)).status).toBe(401);
  expect((await b2.appVpk("10.0.0.5", auth)).bytes?.length).toBe(4);
  expect((await b2.appVpk("10.0.0.5", null)).status).toBe(401);
});
