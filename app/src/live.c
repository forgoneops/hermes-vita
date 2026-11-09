#include "live.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_DIR "ux0:data/pocket-hermes"
#define CFG_FILE CFG_DIR "/config.txt"
#define OUT_CAP 12288
#define POLL_US 1500000UL   /* the companion should react quickly */

static struct { char host[64]; int port; char token[96]; } cfg = { "", 4410, "" };
static volatile int state = LIVE_OFF, started = 0, job = 0, ver = 0;
static char status[96] = "not started";
static char j_a[96], j_b[96], j_c[512], watch[48], watch_agent[24];
static volatile int rec_want = 0, voice_state = 0;
#define REC_RATE 16000
#define REC_MAX_SAMPLES (REC_RATE * 14)
static short rec_buf[REC_MAX_SAMPLES];
static ldata_t shared;
static SceUID mtx;
static lsess_t sess[12]; static int nsess = 0;
static volatile int upd = 0;   /* see live.h */

int live_state(void) { return state; }
const char *live_status(void) { return status; }
void live_trace(const char *s) {   /* a startup log on the card: the next crash tells where it stopped */
    sceIoMkdir(CFG_DIR, 0777); FILE *f = fopen(CFG_DIR "/log.txt", "a"); if (!f) return; fprintf(f, "%u %s\n", sceKernelGetProcessTimeLow() / 1000u, s); fclose(f);
}
static void say(const char *s) { snprintf(status, sizeof status, "%s", s); }

static void cfg_load(void) {
    FILE *f = fopen(CFG_FILE, "r"); char line[160];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host=", 5)) snprintf(cfg.host, sizeof cfg.host, "%s", line + 5);
        else if (!strncmp(line, "port=", 5)) cfg.port = atoi(line + 5);
        else if (!strncmp(line, "token=", 6)) snprintf(cfg.token, sizeof cfg.token, "%s", line + 6);
    }
    fclose(f);
}
static void cfg_save(void) {
    sceIoMkdir(CFG_DIR, 0777);
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "host=%s\nport=%d\ntoken=%s\n", cfg.host, cfg.port, cfg.token);
    fclose(f);
}

/* One request, one connection. Returns the HTTP status, or -1 when the bridge cannot be reached. */
static int last_total = 0;   /* bytes of the last response body */
static int http_ex(const char *path, const char *body, int blen, const char *ctype, int auth, char *out, int cap) {
    char url[200], bearer[140]; int code = -1, total = 0;
    snprintf(url, sizeof url, "http://%s:%d%s", cfg.host, cfg.port, path);
    int tpl = sceHttpCreateTemplate("PocketOps/1", SCE_HTTP_VERSION_1_1, SCE_TRUE);
    if (tpl < 0) return -1;
    sceHttpSetConnectTimeOut(tpl, 4 * 1000 * 1000); sceHttpSetSendTimeOut(tpl, (strstr(ctype, "audio") ? 30 : 5) * 1000 * 1000); sceHttpSetRecvTimeOut(tpl, (strstr(ctype, "audio") ? 120 : 8) * 1000 * 1000);   /* Whisper on the PC can take a while */
    int conn = sceHttpCreateConnectionWithURL(tpl, url, SCE_FALSE), req = -1;
    if (conn >= 0) req = sceHttpCreateRequestWithURL(conn, body ? SCE_HTTP_METHOD_POST : SCE_HTTP_METHOD_GET, url, body ? (unsigned long long)blen : 0);
    if (req >= 0) {
        if (auth) { snprintf(bearer, sizeof bearer, "Bearer %s", cfg.token); sceHttpAddRequestHeader(req, "Authorization", bearer, SCE_HTTP_HEADER_OVERWRITE); }
        if (body) sceHttpAddRequestHeader(req, "Content-Type", ctype, SCE_HTTP_HEADER_OVERWRITE);
        if (sceHttpSendRequest(req, body, body ? (unsigned int)blen : 0) >= 0 && sceHttpGetStatusCode(req, &code) >= 0) {
            int n;
            while (total < cap - 1 && (n = sceHttpReadData(req, out + total, cap - 1 - total)) > 0) total += n;
        } else code = -1;
    }
    out[total] = 0; last_total = total;
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    return code;
}

static int http(const char *path, const char *body, int auth, char *out, int cap) {
    return http_ex(path, body, body ? (int)strlen(body) : 0, "application/json", auth, out, cap);
}

static void jesc(const char *in, char *out, int cap) {   /* a string into JSON text, quotes and control characters safe */
    int o = 0;
    for (; *in && o < cap - 7; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n' || c == '\r' || c == '\t') out[o++] = ' ';
        else if (c < 0x20) continue;
        else out[o++] = c;
    }
    out[o] = 0;
}
static int split(char *s, char **f, int max) { int n = 0; f[n++] = s; for (; *s && n < max; s++) if (*s == '|') { *s = 0; f[n++] = s + 1; } return n; }
static void cp(char *d, int cap, const char *s) { snprintf(d, cap, "%s", s); }

/* HermesVita: one chat with Hermes itself (the bridge talks to Hermes' own API). S|state, then M| lines. */
static void poll(void) {
    static char buf[OUT_CAP]; ldata_t d; memset(&d, 0, sizeof d);
    int code = http("/v1/lines/hermes", NULL, 1, buf, sizeof buf);
    if (code == 401) { state = LIVE_UNPAIRED; say("not paired (token refused)"); return; }
    if (code != 200) { state = LIVE_ERROR; say(code < 0 ? "bridge not reachable" : "bridge error"); return; }
    cp(d.th[0].id, sizeof d.th[0].id, "hermes"); cp(d.th[0].agent, sizeof d.th[0].agent, "hermes"); cp(d.th[0].title, sizeof d.th[0].title, "Hermes Agent"); d.nth = 1;
    cp(d.hstate, sizeof d.hstate, "idle");
    char *save = NULL, *line, *f[6];
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int n = split(line, f, 4);
        if (f[0][0] == 'S' && n >= 2) cp(d.hstate, sizeof d.hstate, f[1]);
        else if (f[0][0] == 'W' && n >= 2) cp(d.htool, sizeof d.htool, f[1]);
        else if (f[0][0] == 'M' && n >= 4 && d.nmsg < 12) { lmsg_t *m = &d.msg[d.nmsg++]; cp(m->role, sizeof m->role, f[1]); cp(m->agent, sizeof m->agent, f[2]); cp(m->text, sizeof m->text, f[3]); }
    }
    sceKernelLockMutex(mtx, 1, NULL); shared = d; ver++; sceKernelUnlockMutex(mtx, 1);
    if (state != LIVE_ONLINE) say("online");
    state = LIVE_ONLINE;
}

static void run_job(int j) {
    static char buf[OUT_CAP]; char body[900], e1[600], e2[300], path[160]; int code;
    if (j == 1) {                                    /* pair: a_ = "code" or "host[:port] code" */
        char *sp = strrchr(j_a, ' ');
        if (sp) {                                    /* the PC address goes first */
            *sp = 0; char *colon = strchr(j_a, ':');
            if (colon) { *colon = 0; cfg.port = atoi(colon + 1) > 0 ? atoi(colon + 1) : 4410; }
            cp(cfg.host, sizeof cfg.host, j_a); memmove(j_a, sp + 1, strlen(sp + 1) + 1);
        }
        if (!cfg.host[0]) { say("type the PC address and the code: 192.168.1.20 123456"); poll(); return; }
        jesc(j_a, e1, sizeof e1);
        snprintf(body, sizeof body, "{\"code\":\"%s\",\"name\":\"vita\"}", e1);
        code = http("/v1/pair", body, 0, buf, sizeof buf);
        char *t = strstr(buf, "\"token\":\"");
        if (code == 200 && t) { t += 9; char *q = strchr(t, '"'); if (q) { *q = 0; cp(cfg.token, sizeof cfg.token, t); cfg_save(); state = LIVE_CONNECTING; say("paired"); } }
        else say(code == 429 ? "too many tries, wait a minute" : code == 401 ? "wrong or expired code" : "pairing failed (bridge reachable?)");
    } else if (j == 2) {                             /* send: a_ = agent, b_ = thread, c_ = text */
        jesc(j_a, e2, sizeof e2); jesc(j_c, e1, sizeof e1);
        snprintf(path, sizeof path, "/v1/hermes/ask");
        snprintf(body, sizeof body, "{\"text\":\"%s\"}", e1); (void)e2;
        code = http(path, body, 1, buf, sizeof buf);
        say(code == 200 ? "sent" : code == 409 ? "still answering" : code == 503 ? "Hermes API is off on the PC" : "not sent");
    } else if (j == 4) {                             /* sessions list: H|id|title|count lines */
        code = http("/v1/lines/sessions", NULL, 1, buf, sizeof buf);
        if (code == 200) {
            lsess_t tmp[12]; int n = 0; char *save = NULL, *line, *f[6];
            for (line = strtok_r(buf, "\n", &save); line && n < 12; line = strtok_r(NULL, "\n", &save)) {
                if (split(line, f, 4) >= 4 && f[0][0] == 'H') { cp(tmp[n].id, sizeof tmp[n].id, f[1]); cp(tmp[n].title, sizeof tmp[n].title, f[2]); tmp[n].n = atoi(f[3]); n++; }
            }
            sceKernelLockMutex(mtx, 1, NULL); memcpy(sess, tmp, n * sizeof(lsess_t)); nsess = n; sceKernelUnlockMutex(mtx, 1);
            say("sessions loaded");
        } else say("could not load sessions");
        return;
    } else if (j == 7) {                             /* update check: V|build|size */
        code = http("/v1/lines/app", NULL, 1, buf, sizeof buf);
        char *v = strstr(buf, "V|");
        if (code == 200 && v) { char *f2[4]; char *nl = strchr(v, '\n'); if (nl) *nl = 0; if (split(v, f2, 3) >= 2) { upd = strcmp(f2[1], BUILD_ID) ? 2 : 1; say(upd == 2 ? "a newer build is on the PC" : "this is the newest build"); } }
        else { upd = 5; say(code == 404 ? "no build on the PC (run scripts/build-vpk.sh)" : "update check failed"); }
        return;
    } else if (j == 8) {                             /* download the VPK into ux0:/downloads */
        extern int live_download_vpk(void);
        upd = live_download_vpk() ? 4 : 5; say(upd == 4 ? "saved: VitaShell > downloads > HermesVita.vpk > X" : "download failed");
        return;
    } else if (j == 5) {                             /* new chat */
        code = http("/v1/hermes/new", "{}", 1, buf, sizeof buf);
        say(code == 200 ? "new chat" : code == 409 ? "Hermes is still answering" : "not done");
    } else if (j == 6) {                             /* open a session: a_ = id */
        jesc(j_a, e1, sizeof e1); snprintf(body, sizeof body, "{\"id\":\"%s\"}", e1);
        code = http("/v1/hermes/open", body, 1, buf, sizeof buf);
        say(code == 200 ? "session opened" : code == 409 ? "Hermes is still answering" : "could not open it");
    } else if (j == 3) {                             /* answer: a_ = run, b_ = ask, c_ = option */
        jesc(j_b, e2, sizeof e2); jesc(j_c, e1, sizeof e1);
        snprintf(path, sizeof path, "/v1/run/%s/answer", j_a);
        snprintf(body, sizeof body, "{\"askId\":\"%s\",\"option\":\"%s\"}", e2, e1);
        code = http(path, body, 1, buf, sizeof buf);
        say(code == 200 ? "answered" : "nothing was waiting");
    }
    poll();                                           /* read back: what the server says now */
}


static void wav_header(unsigned char *h, int samples) {   /* 44 bytes: PCM, mono, 16 kHz, 16 bit */
    int data = samples * 2, riff = 36 + data, rate = REC_RATE, brate = REC_RATE * 2;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    int fmt = 16; short pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &brate, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}
/* Hold-to-talk: record while rec_want, then bridge /v1/voice returns the text, and it is sent like a typed message. */
static void record_and_send(void) {
    static unsigned char wav[44 + REC_MAX_SAMPLES * 2]; static char buf[2048]; char body[900], e1[600], path[160];
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, 256, REC_RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) { voice_state = 0; say("microphone not available"); return; }
    voice_state = 1; say("listening...");
    int n = 0;
    while (rec_want && n + 256 <= REC_MAX_SAMPLES) { if (sceAudioInInput(port, rec_buf + n) < 0) break; n += 256; }
    sceAudioInReleasePort(port);
    rec_want = 0;                                    /* one recording per press, even when the button is still held at the limit */
    if (n < REC_RATE / 3) { voice_state = 0; say("too short, hold SELECT while speaking"); return; }
    voice_state = 2; say("transcribing...");
    wav_header(wav, n); memcpy(wav + 44, rec_buf, (size_t)n * 2);
    int code = http_ex("/v1/voice", (const char *)wav, 44 + n * 2, "audio/wav", 1, buf, sizeof buf);
    char *t = strstr(buf, "\"text\":\"");
    if (code != 200 || !t) { voice_state = 0; say(code == 401 ? "not paired" : code == 503 ? "speech is off on the PC" : "could not transcribe"); return; }
    t += 8; char *q = t; while (*q && !(*q == '"' && q[-1] != '\\')) q++; *q = 0;
    if (!t[0]) { voice_state = 0; say("heard nothing"); return; }
    char heard[64]; snprintf(heard, sizeof heard, "heard: %.50s", t); say(heard);
    jesc(t, e1, sizeof e1);
    snprintf(path, sizeof path, "/v1/hermes/ask");
    snprintf(body, sizeof body, "{\"text\":\"%s\"}", e1);
    code = http(path, body, 1, buf, sizeof buf);
    voice_state = 0;
    say(code == 200 ? "sent by voice" : code == 409 ? "still answering" : "not sent");
    poll();
}

/* Spoken answers: the bridge makes the voice on the PC (Piper) and sends raw 48 kHz mono 16-bit PCM once per answer. */
#define SPEECH_CAP 1700000
static char spbuf[SPEECH_CAP];
static volatile int speech_on = 1, speech_stop = 0;
void live_speech(int on) { speech_on = on; if (!on) speech_stop = 1; }
int live_speech_state(void) { return speech_on; }
void live_stop_speech(void) { speech_stop = 1; }
int live_download_vpk(void) {
    int code = http_ex("/v1/app/vpk", NULL, 0, "", 1, spbuf, SPEECH_CAP);
    if (code != 200 || last_total < 100000 || spbuf[0] != 'P' || spbuf[1] != 'K') return 0;    /* a VPK is a zip */
    sceIoMkdir("ux0:/downloads", 0777);
    FILE *f = fopen("ux0:/downloads/HermesVita.vpk", "wb"); if (!f) return 0;
    int ok = fwrite(spbuf, 1, last_total, f) == (size_t)last_total; fclose(f); return ok;
}
static void speech_poll(void) {
    int code = http_ex("/v1/hermes/speech/next", NULL, 0, "", 1, spbuf, SPEECH_CAP);
    if (code != 200 || last_total < 4096) return;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_MONO);
    if (port < 0) { char m[48]; snprintf(m, sizeof m, "audio port %08x", (unsigned)port); live_trace(m); return; }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    short *pcm = (short *)spbuf; int n = last_total / 2; static short tail[1024];
    speech_stop = 0; voice_state = 3;
    for (int i = 0; i < n && !speech_stop && !rec_want; i += 1024) {
        if (i + 1024 <= n) sceAudioOutOutput(port, pcm + i);
        else { memset(tail, 0, sizeof tail); memcpy(tail, pcm + i, (n - i) * 2); sceAudioOutOutput(port, tail); }
    }
    sceAudioOutReleasePort(port); voice_state = 0; speech_stop = 0;
}

static int net_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    static char netmem[256 * 1024]; unsigned long last = 0;
    live_trace("net thread start");
    int a = sceSysmoduleLoadModule(SCE_SYSMODULE_NET), b = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);   /* SceHttp is not usable until this module is loaded */
    char m[64]; snprintf(m, sizeof m, "modules net=%08x http=%08x", (unsigned)a, (unsigned)b); live_trace(m);
    SceNetInitParam p = { netmem, sizeof netmem, 0 };
    int c = sceNetInit(&p), d = sceNetCtlInit(), e = sceHttpInit(64 * 1024);
    snprintf(m, sizeof m, "init net=%08x ctl=%08x http=%08x", (unsigned)c, (unsigned)d, (unsigned)e); live_trace(m);
    cfg_load();
    state = cfg.token[0] ? LIVE_CONNECTING : LIVE_UNPAIRED; say(cfg.token[0] ? "connecting" : "press X: PC address and code, e.g. 192.168.1.20 123456");
    for (;;) {
        int j = job;
        if (rec_want && state == LIVE_ONLINE) { record_and_send(); last = sceKernelGetProcessTimeLow(); }
        else if (j) { run_job(j); job = 0; last = sceKernelGetProcessTimeLow(); }
        else if (cfg.token[0] && state != LIVE_UNPAIRED && sceKernelGetProcessTimeLow() - last > POLL_US) { poll();
            if (state == LIVE_ONLINE && speech_on) speech_poll();
            last = sceKernelGetProcessTimeLow(); }
        sceKernelDelayThread(150000);
    }
    return 0;
}

void live_start(void) {
    if (started) return;
    started = 1; mtx = sceKernelCreateMutex("live", 0, 0, NULL);
    SceUID t = sceKernelCreateThread("live_net", net_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL); else say("cannot start network thread");
}
int live_sessions(lsess_t *out, int max) { int n; sceKernelLockMutex(mtx, 1, NULL); n = nsess < max ? nsess : max; memcpy(out, sess, n * sizeof(lsess_t)); sceKernelUnlockMutex(mtx, 1); return n; }
static int post(int j, const char *a, const char *b, const char *c) {
    if (job || !started) return 0;
    cp(j_a, sizeof j_a, a ? a : ""); cp(j_b, sizeof j_b, b ? b : ""); cp(j_c, sizeof j_c, c ? c : ""); job = j; return 1;
}
void live_refresh_sessions(void) { post(4, "", "", ""); }
void live_new_chat(void) { if (post(5, "", "", "")) say("new chat..."); }
void live_open(const char *id) { if (post(6, id, "", "")) say("opening..."); }
int live_update_state(void) { return upd; }
void live_update_check(void) { if (upd != 3 && post(7, "", "", "")) say("checking for an update..."); }
void live_update_download(void) { if (upd == 2 && post(8, "", "", "")) { upd = 3; say("downloading..."); } }
void live_pair(const char *code) { if (post(1, code, "", "")) say("pairing..."); }
void live_send(const char *agent, const char *thread, const char *text) { if (post(2, agent, thread, text)) say("sending..."); }
void live_answer(const char *run, const char *ask, const char *option) { if (post(3, run, ask, option)) say("answering..."); }
void live_watch(const char *thread, const char *agent) { cp(watch, sizeof watch, thread ? thread : ""); cp(watch_agent, sizeof watch_agent, agent ? agent : ""); }
void live_rec(int on) { rec_want = on; }
int live_voice(void) { return voice_state; }
int live_copy(ldata_t *out) { int v; if (!started) return 0; sceKernelLockMutex(mtx, 1, NULL); *out = shared; v = ver; sceKernelUnlockMutex(mtx, 1); return v; }
