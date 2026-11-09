#ifndef LIVE_H
#define LIVE_H
/* Live link to the Hermes Vita bridge on the home network (plain HTTP, device token, see bridge/bridge.ts). */
typedef struct { char id[48], agent[24], title[72]; } lthread_t;
typedef struct { char run[48], ask[48], agent[24], q[128], opt[2][24]; int nopt; } lappr_t;
typedef struct { char role[12], agent[24], text[200]; } lmsg_t;
typedef struct { lthread_t th[12]; int nth; lappr_t ap[4]; int nap; int nrun; lmsg_t msg[12]; int nmsg; char hstate[12]; char htool[24]; } ldata_t;
enum { LIVE_OFF = 0, LIVE_UNPAIRED, LIVE_CONNECTING, LIVE_ONLINE, LIVE_ERROR };

void live_trace(const char *line);          /* one line into log.txt in the app data folder */
void live_start(void);                      /* once; starts the network thread */
int live_state(void);
const char *live_status(void);              /* last result, one line */
void live_pair(const char *code);
void live_send(const char *agent, const char *thread, const char *text);
void live_answer(const char *run, const char *ask, const char *option);
void live_watch(const char *thread, const char *agent);   /* whose messages to poll, and who a spoken message goes to */
void live_rec(int on);                      /* hold-to-talk: 1 starts recording, 0 stops and sends it */
int live_voice(void);                       /* 0 idle, 1 recording, 2 transcribing, 3 playing a spoken answer */
void live_speech(int on);                   /* spoken answers on or off (Hermes Pocket) */
int live_speech_state(void);
void live_stop_speech(void);
typedef struct { char id[48], title[64]; int n; } lsess_t;
int live_sessions(lsess_t *out, int max);   /* the recent Hermes sessions, newest first; returns the count */
void live_refresh_sessions(void);
void live_new_chat(void);                   /* clear this conversation and start a new one */
void live_open(const char *id);
/* app update from the PC: 0 not checked, 1 up to date, 2 newer build available, 3 downloading, 4 saved to downloads, 5 failed */
int live_update_state(void);
void live_update_check(void);               /* ask the bridge for the newest build id */
void live_update_download(void);            /* save it as ux0:/downloads/Hermes_Pocket.vpk; installing is one X in VitaShell */             /* continue an earlier session */
int live_copy(ldata_t *out);                /* newest data; returns its version, 0 = none yet */
#endif
