#include <psp2/ctrl.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "live.h"

/*
 * Hermes Pocket: the Vita as a voice panel for Hermes Agent on your PC.
 * An animated ink portrait with ten states, a TALK page (hold SELECT or the on-screen bar and talk, TRIANGLE types, the whole conversation),
 * a PANEL page (eight prompt cards), a SESSIONS page (new chat, continue an earlier session), a LINK page (pairing, status).
 * Everything goes through the bridge (bridge/bridge.ts); Hermes' API key never leaves the PC. Spoken answers: SQUARE turns them on or off.
 * Extra cards: ux0:data/pocket-hermes/presets.txt, one "LABEL|prompt" per line.
 */

#define SW 960
#define SH 544

/* Night look, ember accent: ember = identity, spark (gold) = the second voice, green = signal. Sharp edges, one chamfered corner. */
#define C_BG      RGBA8(14, 15, 18, 255)
#define C_PANEL   RGBA8(23, 25, 30, 255)
#define C_PANEL2  RGBA8(35, 37, 44, 255)
#define C_LINE    RGBA8(72, 76, 86, 255)
#define C_TEXT    RGBA8(242, 240, 245, 255)
#define C_MUTED   RGBA8(176, 176, 188, 255)
#define C_GOLD    RGBA8(255, 107, 44, 255)      /* ember: identity, accents, frames */
#define C_CYAN    RGBA8(255, 210, 63, 255)      /* spark: you, the second voice */
#define C_RED     RGBA8(232, 70, 70, 255)       /* failure */
#define C_GREEN   RGBA8(88, 224, 150, 255)
#define PAPER     RGBA8(23, 25, 30, 255)        /* the stage */
#define PAPER2    RGBA8(92, 40, 30, 255)        /* the stage's pixel halftone, dim ember */
#define INK       RGBA8(14, 15, 18, 255)        /* text on a filled accent */
#define CHAMFER   12

enum { S_OFFLINE, S_UNPAIRED, S_IDLE, S_SLEEPING, S_LISTENING, S_HEARING, S_THINKING, S_WORKING, S_SPEAKING, S_ERROR, S_COUNT };
static const char *const state_name[S_COUNT] = { "OFFLINE", "NOT PAIRED", "READY", "ASLEEP", "LISTENING", "TRANSCRIBING", "THINKING", "WORKING", "SPEAKING", "ERROR" };
static const char *const state_hint[S_COUNT] = {
    "The PC bridge is not answering", "Open LINK and enter the pairing code", "Hold SELECT and talk, or pick a card",
    "Press any button", "Release SELECT to send", "Turning your voice into text", "Hermes is thinking",
    "Hermes has been at it a while", "Hermes just answered", "Hermes could not finish this one" };

typedef struct { char label[24]; char prompt[200]; } preset_t;
static preset_t presets[8] = {
    { "STATUS",    "Give me a short status: what are you working on right now and what is waiting for me?" },
    { "MY DAY",    "Summarize my day so far in five lines, from my notes and sessions." },
    { "ATTENTION", "What needs my attention first? Name at most three things." },
    { "MORNING",   "Run my morning brief and tell me the result in a few lines." },
    { "GIT",       "Check git status of my main project and tell me what is uncommitted." },
    { "NOTES",     "Read my newest notes and tell me what I wrote last." },
    { "CONTINUE",  "Continue with what you were doing." },
    { "STOP",      "Stop what you are doing and tell me where you stopped." },
};
static int npre = 8;

static vita2d_pgf *font;
static vita2d_font *ttf;      /* Noto Sans Mono (OFL), has the Polish letters the default Vita font lacks */
#define PX(s) ((unsigned)((s) * 21.0f + 0.5f))
static ldata_t live;
static int live_ver = 0;
static int tab = 0, sel = 0, scroll = 0;
static int ime_open = 0, ime_purpose = 0;
static uint16_t ime_input[256];
static unsigned int start_since = 0, speak_until = 0, last_input_ms = 0, think_since = 0;
static unsigned int last_hash = 0;
static int hash_init = 0;
static const char *const tab_name[4] = { "TALK", "PANEL", "SESSIONS", "LINK" };
static lsess_t sess[12]; static int nsess = 0, ssel = 0;   /* ssel 0 = NEW CHAT, 1.. = sessions */

static unsigned int now_ms(void) { return sceKernelGetProcessTimeLow() / 1000u; }

/* ---------------------------------------------------------------- drawing */

static int text_w(float s, const char *v) { return ttf ? vita2d_font_text_width(ttf, PX(s), v) : font ? vita2d_pgf_text_width(font, s, v) : 0; }
static void text(float x, float y, unsigned int c, float s, const char *v) { if (ttf) vita2d_font_draw_text(ttf, (int)x, (int)y, c, PX(s), v); else if (font) vita2d_pgf_draw_text(font, x, y, c, s, v); }
static void rect(float x, float y, float w, float h, unsigned int c) { vita2d_draw_rectangle(x, y, w, h, c); }
static void disc(float x, float y, float r, unsigned int c) { rect(x - r, y - r, 2 * r, 2 * r, c); }   /* square: no round shapes in this look */
static void frame(float x, float y, float w, float h, unsigned int c) { rect(x, y, w, 2, c); rect(x, y + h - 2, w, 2, c); rect(x, y, 2, h, c); rect(x + w - 2, y, 2, h, c); }
static void diamond(float cx, float cy, int r, unsigned int c, int filled)
{
    for (int dy = -r; dy <= r; ++dy) { int half = r - (dy < 0 ? -dy : dy); if (filled || half < 2) rect(cx - half, cy + dy, 2 * half + 1, 1, c); else { rect(cx - half, cy + dy, 2, 1, c); rect(cx + half - 1, cy + dy, 2, 1, c); } }
}
/* angular panel: hairline edge, key corners, the top right corner cut */
static void panel(float x, float y, float w, float h, unsigned int fill, unsigned int accent)
{
    rect(x, y, w, h, fill); rect(x, y, w, 1, C_LINE); rect(x, y + h - 1, w, 1, C_LINE); rect(x, y, 1, h, C_LINE); rect(x + w - 1, y, 1, h, C_LINE);
    for (int i = 0; i < CHAMFER; ++i) { rect(x + w - CHAMFER + i, y + i, CHAMFER - i, 1, C_BG); rect(x + w - CHAMFER + i, y + i, 2, 1, accent); }
    rect(x, y, 14, 2, accent); rect(x, y, 2, 14, accent); rect(x, y + h - 2, 14, 2, accent); rect(x, y + h - 14, 2, 14, accent); rect(x + w - 14, y + h - 2, 14, 2, accent); rect(x + w - 2, y + h - 14, 2, 14, accent);
}
static void fluted_rule(float x, float y, float w)
{
    int n = 0; for (float i = 0; i < w; i += 6, ++n) { int beat = (n % 6) == 0; rect(x + i, y, 2, beat ? 6 : 3, beat ? C_GOLD : C_LINE); }
}

static void text_fit(float x, float y, unsigned int c, float s, int maxw, const char *v)
{
    char b[168]; size_t n = strlen(v); if (n > 160) { n = 160; while (n > 1 && (((unsigned char)v[n]) & 0xc0) == 0x80) --n; }
    memcpy(b, v, n); b[n] = 0;
    while (n > 1 && text_w(s, b) > maxw) {
        --n; while (n > 1 && (((unsigned char)b[n]) & 0xc0) == 0x80) --n;
        memcpy(b + n, "...", 4);
    }
    text(x, y, c, s, b);
}

static void dotline(float x0, float y0, float x1, float y1, float r, unsigned int c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy); int n = (int)(len / (r * 0.6f)) + 1;
    for (int i = 0; i <= n; ++i) disc(x0 + dx * i / n, y0 + dy * i / n, r, c);
}

/* an arc of dots; angles in degrees, 0 = right, 90 = up */
static void dotarc(float cx, float cy, float R, float a0, float a1, float r, unsigned int c)
{
    int n = (int)(fabsf(a1 - a0) * 0.5f) + 2;
    for (int i = 0; i <= n; ++i) { float a = (a0 + (a1 - a0) * i / n) * 3.14159265f / 180.0f; disc(cx + R * cosf(a), cy - R * sinf(a), r, c); }
}

/* square halftone: pixel squares that shrink away from a corner */
static void halftone(float x, float y, float w, float h, int right, unsigned int c)
{
    for (int j = 0; j < (int)(h / 10); ++j) for (int i = 0; i < (int)(w / 10); ++i) {
        float d = (float)(j) / (h / 10.0f) * 0.7f + (float)(right ? (w / 10 - 1 - i) : i) / (w / 10.0f) * 0.5f;
        float s = 7.0f * (d - 0.55f) * 2.2f; if (s < 1.0f) continue; if (s > 7.0f) s = 7.0f;
        rect(x + i * 10 + (10 - s) / 2, y + j * 10 + (10 - s) / 2, s, s, c);
    }
}

/* ------------------------------------------------------------- the girl */

/* The portrait is art made with Codex (art/raw/hermes-girl-*.png, tinted ember by tools/art.ts, cut to assets/hermes/plate.png by
 * art/make-plate.sh). The code gives it life: a slow breath, scanlines and a moving scan bar, a dim for sleep and offline, an amber pulse
 * for errors, rings at the headphone and an orb above the offered palm that says what Hermes is doing. */
static vita2d_texture *t_plate, *t_open, *t_talk;   /* eyes closed, eyes open, speaking: three frames from Codex, same pose */
#define PW 468.0f
#define PH 702.0f
static void ring(float cx, float cy, float R, unsigned int c) { diamond(cx, cy, (int)R, c, 0); }

static void draw_girl(float sx, float sy, float w, float h, int st)
{
    float t = (float)now_ms() / 1000.0f, k = (w / PW) * (1.0f + 0.006f * sinf(t * 1.3f)), shake = (st == S_ERROR) ? 2.5f * sinf(t * 38.0f) : 0.0f;
    int dead = (st == S_OFFLINE || st == S_UNPAIRED);
    float top = sy - 20.0f;
    vita2d_texture *tex = t_plate;
    if (st == S_LISTENING || st == S_HEARING || st == S_WORKING || st == S_ERROR) tex = t_open;
    else if (st == S_SPEAKING) tex = (sinf(t * 13.0f) + 0.6f * sinf(t * 5.3f) > 0.0f) ? t_talk : t_open;
    else if (st == S_IDLE && fmodf(t, 7.0f) > 5.8f) tex = t_open;                         /* she opens her eyes now and then */
    else if (st == S_THINKING && fmodf(t, 3.0f) > 2.2f) tex = t_open;
    if (!tex) tex = t_plate;
    if (tex) vita2d_draw_texture_scale_rotate_hotspot(tex, sx + w / 2 + shake, top + 0.667f * 600.0f, k, k, 0, PW / 2, 600.0f);
    else text(sx + 40, sy + 120, C_MUTED, 0.6f, "portrait missing");
    /* palm and ear in screen space */
    float ox = sx + 66, oy = sy + 235, ex = sx + 219, ey = sy + 129;
    /* the picture dims when she sleeps or the PC is away */
    if (st == S_SLEEPING) rect(sx, sy, w, h, RGBA8(10, 10, 20, 130));
    if (dead) rect(sx, sy, w, h, RGBA8(14, 15, 18, 170));
    /* retro: scanlines and a slow scan bar */
    for (float y = sy; y < sy + h; y += 3.0f) rect(sx, y, w, 1, RGBA8(0, 0, 0, 55));
    float bar = fmodf(t * 70.0f, h + 120.0f) - 60.0f;
    for (int i = 0; i < 24; ++i) { float y = sy + bar - i * 2.0f; if (y > sy && y < sy + h) rect(sx, y, w, 2, RGBA8(255, 107, 44, (unsigned)(46 - i * 2))); }
    if (st == S_ERROR) { unsigned a = (unsigned)(40 + 50 * (0.5f + 0.5f * sinf(t * 7.0f))); rect(sx, sy, w, 4, RGBA8(232, 70, 70, a)); rect(sx, sy + h - 4, w, 4, RGBA8(232, 70, 70, a)); }
    /* rings at the headphone while she listens or speaks */
    if (st == S_LISTENING || st == S_HEARING) for (int i = 0; i < 3; ++i) { float p = fmodf(t * 1.1f + i / 3.0f, 1.0f); ring(ex, ey, 10 + (1.0f - p) * 40, p > 0.5f ? C_CYAN : C_GOLD); }
    if (st == S_SPEAKING) for (int i = 0; i < 3; ++i) { float p = fmodf(t * 1.5f + i / 3.0f, 1.0f); ring(ex, ey, 10 + p * 40, p < 0.5f ? C_CYAN : C_GOLD); }
    /* the orb above the palm: diamonds */
    unsigned int c = dead ? C_LINE : C_GOLD, e = dead ? C_LINE : C_CYAN;
    if (st == S_IDLE || st == S_SLEEPING) { diamond(ox, oy, 20, c, 0); diamond(ox, oy, 5 + (int)(2 * sinf(t * 1.6f)), e, 1); }
    else if (st == S_LISTENING) { diamond(ox, oy, 20, c, 0); diamond(ox, oy, 6, e, 1); }
    else if (st == S_SPEAKING) { diamond(ox, oy, 9, e, 1); for (int i = 0; i < 3; ++i) { float p = fmodf(t * 1.5f + i / 3.0f, 1.0f); diamond(ox, oy, 12 + (int)(p * 26), p < 0.5f ? e : c, 0); } }
    else if (st == S_HEARING) { diamond(ox, oy, 20, c, 0); for (int i = 0; i < 3; ++i) diamond(ox - 12 + i * 12, oy, (fmodf(t * 3.0f, 3.0f) > i) ? 4 : 2, e, 1); }
    else if (st == S_THINKING) { diamond(ox, oy, 16, c, 0); for (int i = 0; i < 3; ++i) { float a = t * 2.2f + i * 2.094f; diamond(ox + 28 * cosf(a), oy + 28 * sinf(a), 4, e, 1); } }
    else if (st == S_WORKING) { diamond(ox, oy, 20, c, 0); diamond(ox, oy, 6, e, 1); for (int i = 0; i < 8; ++i) { float a = t * 3.0f + i * 0.785f; diamond(ox + 32 * cosf(a), oy + 32 * sinf(a), 4 - (i % 3), (i % 2) ? c : e, 1); } }
    else if (st == S_ERROR) { diamond(ox, oy, 20, C_RED, 0); dotline(ox - 9, oy - 9, ox + 9, oy + 9, 2, C_RED); dotline(ox - 9, oy + 9, ox + 9, oy - 9, 2, C_RED); }
    else { diamond(ox, oy, 20, c, 0); text(ox - 6, oy + 8, c, 0.8f, "?"); }
    if (st == S_SLEEPING) { text(sx + 200, sy + 120, C_TEXT, 0.9f, "z"); text(sx + 218, sy + 104, C_TEXT, 0.7f, "z"); }
}

/* ------------------------------------------------------------- the state */

static int girl_state(void)
{
    int st = live_state(), v = live_voice(); unsigned int n = now_ms();
    if (v == 1) return S_LISTENING;
    if (v == 2) return S_HEARING;
    if (v == 3) return S_SPEAKING;
    if (st == LIVE_OFF || st == LIVE_UNPAIRED) return S_UNPAIRED;
    if (st != LIVE_ONLINE) return S_OFFLINE;
    if (!strcmp(live.hstate, "failed")) return S_ERROR;
    if (!strcmp(live.hstate, "working") || live.htool[0]) return S_WORKING;
    if (!strcmp(live.hstate, "thinking")) return (n - think_since > 12000u) ? S_WORKING : S_THINKING;
    if (n < speak_until) return S_SPEAKING;
    if (n - last_input_ms > 60000u) return S_SLEEPING;
    return S_IDLE;
}

/* new data: when the last line is a new answer from Hermes, she speaks for a few seconds */
static void take_data(void)
{
    ldata_t d; int v = live_copy(&d);
    if (!v || v == live_ver) return;
    int was_thinking = !strcmp(live.hstate, "thinking");
    live = d; live_ver = v;
    unsigned int h = (unsigned)live.nmsg;
    if (live.nmsg > 0) for (const char *p = live.msg[live.nmsg - 1].text; *p; ++p) h = h * 31u + (unsigned char)*p;
    if (!strcmp(live.hstate, "thinking") && !was_thinking) think_since = now_ms();
    if (hash_init && h != last_hash && live.nmsg > 0 && strcmp(live.msg[live.nmsg - 1].role, "user")) speak_until = now_ms() + 6000u;
    last_hash = h; hash_init = 1; scroll = 0;
}

/* ------------------------------------------------------------ the transcript */

typedef struct { char s[124]; unsigned char role; } wline_t;
static wline_t wl[260]; static int nwl = 0, wl_ver = -1;
#define TX 380
#define TW 536
#define LH 23

static void push_line(const char *s, int role) { if (nwl < 260) { snprintf(wl[nwl].s, sizeof wl[nwl].s, "%s", s); wl[nwl].role = (unsigned char)role; nwl++; } }

static void rewrap(void)
{
    nwl = 0;
    for (int i = 0; i < live.nmsg; ++i) {
        int role = strcmp(live.msg[i].role, "user") ? 1 : 0; const char *p = live.msg[i].text; char cur[124] = "", word[100], cand[240];
        while (*p) {
            while (*p == ' ') ++p; if (!*p) break;
            size_t n = 0; while (p[n] && p[n] != ' ' && n < 90) ++n;
            while (n > 1 && n < strlen(p) && p[n] && (((unsigned char)p[n]) & 0xc0) == 0x80) --n;    /* never cut a UTF-8 sequence */
            memcpy(word, p, n); word[n] = 0; p += n;
            snprintf(cand, sizeof cand, "%s%s%s", cur, cur[0] ? " " : "", word);
            if (cur[0] && (strlen(cand) >= 120 || text_w(0.62f, cand) > TW - 18)) { push_line(cur, role); snprintf(cur, sizeof cur, "%s", word); }
            else snprintf(cur, sizeof cur, "%s", cand);
        }
        if (cur[0]) push_line(cur, role);
    }
    wl_ver = live_ver;
}

/* ------------------------------------------------------------ pages */

static int online(void) { return live_state() == LIVE_ONLINE; }
static int mic_down = 0;     /* the on-screen bar is held */
#define TABX(i) (300 + (i) * 92)

static void draw_header(int st)
{
    rect(0, 0, SW, 56, C_PANEL);
    diamond(32, 28, 13, C_GOLD, 1); diamond(32, 28, 5, C_PANEL, 1);
    text(56, 38, C_TEXT, 1.0f, "HERMES"); text(56 + text_w(1.0f, "HERMES") + 14, 37, C_MUTED, 0.68f, "POCKET");
    text(272, 37, C_GOLD, 0.7f, "L");
    for (int i = 0; i < 4; ++i) {
        int on = tab == i; text(TABX(i), 37, on ? C_TEXT : C_MUTED, 0.72f, tab_name[i]);
        if (on) rect(TABX(i), 44, text_w(0.72f, tab_name[i]), 3, C_GOLD);
    }
    text(TABX(3) + 62, 37, C_GOLD, 0.7f, "R");
    unsigned int c = online() ? C_GREEN : (st == S_UNPAIRED ? C_GOLD : C_RED);
    rect(706, 7, 230, 42, C_BG); frame(706, 7, 230, 42, c);
    diamond(728, 28, 8, c, online());
    text(748, 26, c, 0.72f, online() ? "PC ONLINE" : st == S_UNPAIRED ? "NOT PAIRED" : "PC OFFLINE");
    text_fit(748, 42, C_MUTED, 0.58f, 180, online() ? "HOME NETWORK" : live_status());
    fluted_rule(0, 56, SW);
}

static void draw_stage(int st)
{
    float x = 24, y = 80, w = 312, h = 400;
    rect(x, y, w, h, PAPER);
    vita2d_enable_clipping(); vita2d_set_clip_rectangle((int)x + 1, (int)y + 1, (int)(x + w) - 1, (int)(y + h) - 1);
    draw_girl(x, y, w, h, st);
    rect(x, y + h - 74, w, 74, RGBA8(14, 15, 18, 215));
    vita2d_disable_clipping();
    panel(x, y, w, h, RGBA8(0, 0, 0, 0), C_GOLD);
    fluted_rule(x + 2, y + h - 76, w - 4);
    unsigned int c = (st == S_ERROR) ? C_RED : (st == S_OFFLINE || st == S_UNPAIRED || st == S_SLEEPING) ? C_MUTED : C_GOLD;
    diamond(x + 22, y + h - 44, 7, c, st != S_OFFLINE && st != S_UNPAIRED);
    text(x + 40, y + h - 36, c, 0.9f, state_name[st]);
    char tool[48]; if (st == S_WORKING && live.htool[0]) { snprintf(tool, sizeof tool, "Using the tool: %s", live.htool); text_fit(x + 16, y + h - 14, C_MUTED, 0.5f, (int)w - 32, tool); }
    else text_fit(x + 16, y + h - 14, C_MUTED, 0.5f, (int)w - 32, state_hint[st]);
}

static void draw_talk(void)
{
    panel(356, 80, 580, 400, C_PANEL, C_GOLD);
    if (wl_ver != live_ver) rewrap();
    int vis = 14, y0 = 112, from = nwl - vis - scroll; if (from < 0) from = 0;
    if (nwl == 0) {
        text(TX, 210, C_TEXT, 0.75f, online() ? "SAY SOMETHING TO HERMES" : "PAIR WITH THE PC FIRST (LINK)");
        text(TX, 240, C_MUTED, 0.55f, "Hold SELECT, or the bar below, and talk.");
    }
    for (int i = from, r = 0; i < nwl && r < vis; ++i, ++r) {
        int y = y0 + r * LH;
        rect(368, y - 16, 3, LH, wl[i].role ? C_GOLD : C_CYAN);
        text(TX - 4, y, wl[i].role ? C_TEXT : C_CYAN, 0.62f, wl[i].s);
    }
    if (!strcmp(live.hstate, "thinking") || !strcmp(live.hstate, "working")) { char b[24]; int n = 1 + (int)(now_ms() / 400u) % 3; snprintf(b, sizeof b, "HERMES IS ON IT%.*s", n, "..."); text(TX - 4, 424, C_GOLD, 0.55f, b); }
    if (scroll > 0) text(760, 104, C_MUTED, 0.5f, "OLDER");
    frame(838, 90, 86, 22, C_LINE); text(838 + (86 - text_w(0.5f, "O  NEW")) / 2.0f, 106, C_MUTED, 0.5f, "O  NEW");
    int rec = live_voice() == 1;
    rect(368, 436, 556, 32, rec ? C_GOLD : C_PANEL2); frame(368, 436, 556, 32, rec ? C_GOLD : C_LINE);
    const char *m = rec ? "RECORDING  /  RELEASE TO SEND" : live_voice() == 2 ? "TRANSCRIBING..." : "HOLD HERE OR SELECT TO TALK  /  TRIANGLE TYPE  /  SQUARE VOICE";
    text(368 + (556 - text_w(0.6f, m)) / 2.0f, 457, rec ? INK : C_TEXT, 0.6f, m);
}

static int card_x(int i) { return 368 + (i % 2) * 290; }
static int card_y(int i) { return 92 + (i / 2) * 84; }

static void draw_panel(void)
{
    panel(356, 80, 580, 400, C_PANEL, C_GOLD);
    for (int i = 0; i < npre; ++i) {
        int x = card_x(i), y = card_y(i), on = (i == sel);
        panel(x, y, 278, 74, on ? C_PANEL2 : C_BG, on ? C_GOLD : C_LINE);
        diamond(x + 20, y + 26, 6, on ? C_GOLD : C_LINE, on);
        text(x + 38, y + 32, on ? C_TEXT : C_MUTED, 0.8f, presets[i].label);
        text_fit(x + 18, y + 58, C_MUTED, 0.5f, 246, presets[i].prompt);
    }
    text(368, 446, C_MUTED, 0.55f, online() ? "D-PAD CHOOSE   /   X SEND TO HERMES   /   TRIANGLE WRITE YOUR OWN" : "PAIR WITH THE PC FIRST (LINK)");
}

#define SROW(i) (96 + (i) * 29)
static void draw_sessions(void)
{
    panel(356, 80, 580, 400, C_PANEL, C_GOLD);
    nsess = live_sessions(sess, 12);
    for (int i = 0; i <= nsess && i < 13; ++i) {
        int y = SROW(i), on = (i == ssel);
        rect(368, y, 556, 26, on ? C_PANEL2 : C_PANEL); if (on) { frame(368, y, 556, 26, C_GOLD); rect(368, y, 5, 26, C_GOLD); }
        if (i == 0) { diamond(388, y + 13, 6, C_GOLD, 1); text(404, y + 19, on ? C_GOLD : C_TEXT, 0.7f, "NEW CHAT"); }
        else { char n[16]; snprintf(n, sizeof n, "%d", sess[i - 1].n); diamond(388, y + 13, 5, on ? C_GOLD : C_LINE, 0); text_fit(404, y + 19, on ? C_TEXT : C_MUTED, 0.6f, 440, sess[i - 1].title); text(924 - text_w(0.5f, n), y + 18, C_MUTED, 0.5f, n); }
    }
    if (nsess == 0) text(404, SROW(1) + 19, C_MUTED, 0.55f, "No sessions yet, or the list is still loading.");
    text(368, 468, C_MUTED, 0.5f, "UP / DOWN CHOOSE   /   X OPEN AND CONTINUE   /   O REFRESH");
}

static void draw_link(void)
{
    panel(356, 80, 580, 400, C_PANEL, C_GOLD);
    int st = live_state();
    const char *s = st == LIVE_ONLINE ? "CONNECTED" : st == LIVE_UNPAIRED || st == LIVE_OFF ? "NOT PAIRED" : st == LIVE_CONNECTING ? "CONNECTING" : "ERROR";
    diamond(384, 126, 9, st == LIVE_ONLINE ? C_GREEN : C_GOLD, st == LIVE_ONLINE);
    text(406, 136, C_TEXT, 1.0f, s);
    text(384, 168, C_MUTED, 0.6f, live_status());
    text(384, 232, C_TEXT, 0.65f, "1   ON THE PC:   bun plugins/vita/bridge.ts serve");
    text(384, 262, C_TEXT, 0.65f, "2   ON THE PC:   bun plugins/vita/bridge.ts pair");
    text(384, 292, C_TEXT, 0.65f, "3   PRESS X, TYPE:  <PC ADDRESS> <CODE>");
    text(384, 340, C_MUTED, 0.55f, "The code works once, for five minutes. The PC address is in");
    text(384, 364, C_MUTED, 0.55f, "ux0:data/pocket-hermes/config.txt (host=, port=). Example: 192.168.1.20 123456");
    text(384, 400, C_MUTED, 0.55f, "Your voice goes to the PC for transcription; nothing leaves your home network.");
    rect(384, 424, 210, 32, online() ? C_PANEL2 : C_GOLD); frame(384, 424, 210, 32, online() ? C_LINE : C_GOLD);
    text(384 + (210 - text_w(0.65f, "X   PAIRING CODE")) / 2.0f, 446, online() ? C_MUTED : INK, 0.65f, "X   PAIRING CODE");
}

static void key_hint(float *x, const char *key, const char *label)
{
    float kw = (float)text_w(0.6f, key) + 14.0f;
    frame(*x, 512, kw, 22, C_LINE); text(*x + 7, 528, C_TEXT, 0.6f, key); text(*x + kw + 7, 528, C_MUTED, 0.6f, label);
    *x += kw + 7.0f + (float)text_w(0.6f, label) + 18.0f;
}

static void draw_footer(void)
{
    float x = 24;
    fluted_rule(0, 490, SW); rect(0, 496, SW, 48, C_PANEL);
    text(24, 508, C_GOLD, 0.5f, "LIVE  /  HOME NETWORK  /  YOUR VOICE AND PROMPTS GO ONLY TO YOUR PC");
    key_hint(&x, "L / R", "PAGE"); key_hint(&x, "HOLD SELECT", "TALK"); key_hint(&x, "TRIANGLE", "TYPE"); key_hint(&x, "O", "NEW CHAT"); key_hint(&x, "X", "OK"); key_hint(&x, "HOLD START", "EXIT");
}

static void draw_frame(void)
{
    int st = girl_state();
    vita2d_start_drawing(); vita2d_clear_screen();
    rect(0, 0, SW, SH, C_BG);
    draw_header(st); draw_stage(st);
    if (tab == 0) draw_talk(); else if (tab == 1) draw_panel(); else if (tab == 2) draw_sessions(); else draw_link();
    draw_footer();
    vita2d_end_drawing();
    if (ime_open) vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------------ keyboard */

static void open_ime(int purpose)
{
    SceImeDialogParam p; ime_purpose = purpose;
    memset(ime_input, 0, sizeof ime_input); sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_POLISH | SCE_IME_LANGUAGE_ENGLISH; p.languagesForced = SCE_FALSE;   /* Polish first, English on the IME key */
    p.type = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT; p.option = 0; p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = purpose == 1 ? u"PC address and code, e.g. 192.168.1.20 123456" : u"Message to Hermes";
    p.maxTextLength = purpose == 1 ? 40 : 200; p.initialText = u""; p.inputTextBuffer = ime_input;
    ime_open = sceImeDialogInit(&p) >= 0;
}

static void utf16_to_utf8(const uint16_t *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < cap; ++i) {
        uint32_t c = in[i];
        if (c >= 0xd800 && c <= 0xdbff && in[i + 1] >= 0xdc00 && in[i + 1] <= 0xdfff) c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out[o++] = (char)(0xe0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[o++] = (char)(0x80 | (c & 0x3f)); }
        else { out[o++] = (char)(0xf0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3f)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[o++] = (char)(0x80 | (c & 0x3f)); }
    }
    out[o] = 0;
}

static void update_ime(void)
{
    if (!ime_open || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SceImeDialogResult r; memset(&r, 0, sizeof r); sceImeDialogGetResult(&r);
    if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        static char typed[800]; utf16_to_utf8(ime_input, typed, sizeof typed);
        if (ime_purpose == 1) live_pair(typed); else if (typed[0]) { live_send("hermes", "hermes", typed); tab = 0; }
    }
    sceImeDialogTerm(); ime_open = 0;
}

static void load_presets(void)
{
    FILE *f = fopen("ux0:data/pocket-hermes/presets.txt", "r"); char line[260]; int n = 0;
    if (!f) return;
    while (n < 8 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; char *bar = strchr(line, '|'); if (!bar || bar == line) continue;
        *bar = 0; snprintf(presets[n].label, sizeof presets[n].label, "%s", line); snprintf(presets[n].prompt, sizeof presets[n].prompt, "%s", bar + 1); ++n;
    }
    fclose(f); if (n > 0 && n < 8) npre = (n > npre) ? n : npre;
    if (n == 8) npre = 8;
}

static void send_preset(int i) { if (online() && i >= 0 && i < npre) { live_send("hermes", "hermes", presets[i].prompt); tab = 0; } }

int main(void)
{
    SceCtrlData pad; SceTouchData touch; uint32_t prev = 0; int was_touch = 0, last_ty = 0, drag_acc = 0;

    sceAppUtilInit(&(SceAppUtilInitParam){0}, &(SceAppUtilBootParam){0});
    sceCommonDialogSetConfigParam(&(SceCommonDialogConfigParam){0});
    live_trace("main start"); vita2d_init(); vita2d_set_clear_color(C_BG); live_trace("vita2d ok");
    font = vita2d_load_default_pgf(); ttf = vita2d_load_font_file("app0:/assets/hermes/font.ttf"); live_trace(ttf ? "font ok (ttf)" : font ? "font ok (pgf only)" : "font MISSING");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    t_plate = vita2d_load_JPEG_file("app0:/assets/hermes/plate.jpg"); t_open = vita2d_load_JPEG_file("app0:/assets/hermes/plate_open.jpg"); t_talk = vita2d_load_JPEG_file("app0:/assets/hermes/plate_talk.jpg");
    live_trace(t_plate && t_open && t_talk ? "portrait ok (3 frames)" : "portrait MISSING");
    load_presets(); last_input_ms = now_ms();
    live_start(); live_trace("loop");

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~prev; if (pad.buttons) last_input_ms = now_ms();
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int touching = touch.reportNum > 0, tx = touching ? touch.report[0].x / 2 : 0, ty = touching ? touch.report[0].y / 2 : 0;
        int tapped = touching && !was_touch;
        if (touching) last_input_ms = now_ms();
        take_data();

        /* voice: SELECT held, or the bar held with a finger */
        mic_down = touching && tab == 0 && tx >= 368 && tx <= 924 && ty >= 436 && ty <= 468;
        int held = !ime_open && ((pad.buttons & SCE_CTRL_SELECT) || mic_down);
        static int rec_on = 0;
        if (held && !rec_on && online()) { live_stop_speech(); live_rec(1); rec_on = 1; }
        if (!held && rec_on) { live_rec(0); rec_on = 0; }
        prev = pad.buttons;

        if (!ime_open) {
            if (pad.buttons & SCE_CTRL_START) { if (!start_since) start_since = now_ms(); if (now_ms() - start_since > 1500u) break; } else start_since = 0;   /* hold START to quit: a quick PS+START still takes a screenshot */
            int was_tab = tab;
            if (pressed & SCE_CTRL_LTRIGGER) tab = (tab + 3) % 4;
            if (pressed & SCE_CTRL_RTRIGGER) tab = (tab + 1) % 4;
            if (tapped && ty < 56) for (int i = 0; i < 4; ++i) if (tx >= TABX(i) - 8 && tx <= TABX(i) + 80) tab = i;
            if (tab == 2 && was_tab != 2) { live_refresh_sessions(); ssel = 0; }
            if (tab == 0) {
                if ((pressed & SCE_CTRL_UP) && scroll < nwl - 14) scroll++;
                if ((pressed & SCE_CTRL_DOWN) && scroll > 0) scroll--;
                if (touching && was_touch && ty > 92 && ty < 432 && tx > 356) { drag_acc += ty - last_ty; while (drag_acc >= LH) { if (scroll < nwl - 14) scroll++; drag_acc -= LH; } while (drag_acc <= -LH) { if (scroll > 0) scroll--; drag_acc += LH; } }
                else drag_acc = 0;
                if ((pressed & SCE_CTRL_CROSS) && !online()) open_ime(1);
                if ((pressed & SCE_CTRL_CIRCLE) || (tapped && tx >= 838 && tx <= 924 && ty >= 90 && ty <= 112)) { if (online()) { live_stop_speech(); live_new_chat(); } }
            } else if (tab == 2) {
                if ((pressed & SCE_CTRL_UP) && ssel > 0) ssel--;
                if ((pressed & SCE_CTRL_DOWN) && ssel < nsess) ssel++;
                if (pressed & SCE_CTRL_CIRCLE) live_refresh_sessions();
                int pick = -1;
                if (pressed & SCE_CTRL_CROSS) pick = ssel;
                if (tapped) for (int i = 0; i <= nsess && i < 13; ++i) if (tx >= 368 && tx <= 924 && ty >= SROW(i) && ty <= SROW(i) + 26) { if (ssel == i) pick = i; else ssel = i; }
                if (pick == 0 && online()) { live_new_chat(); tab = 0; }
                else if (pick > 0 && pick <= nsess && online()) { live_open(sess[pick - 1].id); tab = 0; }
            } else if (tab == 1) {
                if ((pressed & SCE_CTRL_UP) && sel >= 2) sel -= 2;
                if ((pressed & SCE_CTRL_DOWN) && sel + 2 < npre) sel += 2;
                if ((pressed & SCE_CTRL_LEFT) && (sel % 2)) sel--;
                if ((pressed & SCE_CTRL_RIGHT) && !(sel % 2) && sel + 1 < npre) sel++;
                if (pressed & SCE_CTRL_CROSS) { if (online()) send_preset(sel); else open_ime(1); }
                if (tapped) for (int i = 0; i < npre; ++i) if (tx >= card_x(i) && tx <= card_x(i) + 278 && ty >= card_y(i) && ty <= card_y(i) + 74) { if (sel == i) send_preset(i); else sel = i; }
            } else {
                if ((pressed & SCE_CTRL_CROSS) || (tapped && tx >= 384 && tx <= 594 && ty >= 424 && ty <= 456)) open_ime(1);
            }
            if ((pressed & SCE_CTRL_TRIANGLE) && online()) open_ime(2);
            if (pressed & SCE_CTRL_SQUARE) live_speech(!live_speech_state());          /* spoken answers on or off */
        }
        was_touch = touching; last_ty = ty;

        draw_frame(); update_ime();
        sceKernelDelayThread(16000);
    }
    live_rec(0);
    vita2d_wait_rendering_done();
    if (ttf) vita2d_free_font(ttf);
    if (font) vita2d_free_pgf(font);
    vita2d_fini(); sceKernelExitProcess(0);
    return 0;
}
