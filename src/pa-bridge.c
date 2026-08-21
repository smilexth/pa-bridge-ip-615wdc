// pa-bridge — Asterisk AudioSocket → BUENA/CTS IP- series network speakers
//
// Asterisk dials AudioSocket, which streams 8 kHz 16-bit mono PCM in 320-byte
// frames. TSDK_BroadPcmVoice() wants 8 kHz 16-bit mono PCM in multiples of 320
// bytes. The two formats are identical, so this bridge is mostly plumbing:
// accept the TCP stream, hand each audio payload to the SDK, stop on hangup.
//
// The SDK runs in server mode (TSDK_Init(..., TRUE, ...)), meaning *this*
// process is what the speakers register to — it replaces the vendor's Windows
// TbsServer.exe. Point each terminal's "Server IP" at the host running this.
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <pthread.h>

// The vendor headers expect the caller to supply the Win32 calling-convention
// macros and to pull in their typedefs first — their own demo does exactly this.
#if defined(WIN32)
#  define CALLBACK __stdcall
#  define WINAPI   __stdcall
#else
#  define CALLBACK
#  define WINAPI
#endif

#include "win_types.h"
#include "cts_sdk.h"

#define AS_TYPE_TERMINATE 0x00
#define AS_TYPE_UUID      0x01
#define AS_TYPE_AUDIO     0x10
#define AS_TYPE_ERROR     0xff

#define PCM_FRAME   320       // 20 ms of 8 kHz 16-bit mono — one AudioSocket frame
#define BROAD_GROUP 1         // broadcast group this bridge owns (1..MAX_BROAD_GROUP)
#define SDK_MAGIC   0x50414252 /* "PABR" — echoed back to the callback as usr_data */

// The SDK's own registration listener is a compiled-in constant (29883) and there is
// no API to change it, but the TM35 firmware dials 29783. Rather than require an
// iptables rule on every host, the bridge accepts 29783 itself and pumps the bytes
// through to the SDK on loopback.
#define FW_PORT     29783
#define SDK_PORT    29883
#define CTL_PORT    9093       // loopback-only control channel (vol / status / get)
#define DEFAULT_VOL 60         // 0..100, the speaker's own amplifier level

static volatile sig_atomic_t g_stop;
// SDK convention: CERR_SUCCESS is 0 and every failure is a negative enSdkErrCode.
static int  g_listen_port = 9092;

// Terminals learned from the SDK's own connect events. dwTermID is NOT the "Term ID"
// shown in ConfigTool: the header defines it as zone<<16 | device, so ConfigTool's
// zone 1 / device 1 is 0x00010001, and passing 1 earns -802 CERR_INVALID_PARAMETER.
// Reading it from the event removes the guesswork entirely.
static DWORD g_term_ids[MAX_BROAD_TERM];
static int   g_term_count;
static pthread_mutex_t g_term_lock = PTHREAD_MUTEX_INITIALIZER;

// The speaker's own amplifier level, 0 (mute) .. 100. Applied to every terminal
// as it connects, so a reboot cannot leave it at some forgotten level, and
// changeable at runtime through the control port.
static int g_volume = DEFAULT_VOL;

static volatile sig_atomic_t g_group_dirty;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void remember_term(DWORD id)
{
    pthread_mutex_lock(&g_term_lock);
    int known = 0;
    for (int i = 0; i < g_term_count; i++) if (g_term_ids[i] == id) known = 1;
    if (!known && g_term_count < MAX_BROAD_TERM) g_term_ids[g_term_count++] = id;
    pthread_mutex_unlock(&g_term_lock);
    g_group_dirty = 1;
}

// Push every known terminal into the broadcast group. A terminal that has not
// connected yet is rejected, so this is retried on each connect and before each call.
static int sync_group(void)
{
    TSdkGroupTermList list;
    memset(&list, 0, sizeof list);         // unused slots must be zero-filled
    pthread_mutex_lock(&g_term_lock);
    int n = g_term_count;
    for (int i = 0; i < n; i++) list.TermList[i] = g_term_ids[i];
    pthread_mutex_unlock(&g_term_lock);

    if (n == 0) return CERR_SUCCESS;       // nothing to do yet; not an error

    int rc = TSDK_SetupGroup(BROAD_GROUP, &list);
    if (rc != CERR_SUCCESS)
        fprintf(stderr, "[sdk] SetupGroup(%d terminals) -> %d\n", n, rc);
    else
        fprintf(stderr, "[sdk] group %d now holds %d terminal(s)\n", BROAD_GROUP, n);
    return rc;
}

// Push the current volume to one terminal. The SDK answers asynchronously in the
// callback, so a non-zero return here only means the request could not be sent.
// Use the COARSE 0..10 amp level, not the 0..100 "Ex" value.
//
// Measured on TM35 / BIOS 5.7.9: TSDK_Req_SetAudLevelEx(value) is accepted
// (result 0) but the firmware only really follows the coarse level it derives,
// and that derivation is broken -- value 100 became level 10 and was deafening,
// while 60, 40 and 30 all became level 1 and were inaudible. There is no useful
// range in between. TSDK_Req_SetAudLevel() sets the level directly and behaves,
// so the 0..100 the operator types is simply scaled to the 0..10 the hardware has.
static void apply_volume(DWORD id)
{
    int step = (g_volume + 5) / 10;              // 0..100 -> 0..10, nearest
    if (step > 10) step = 10;
    if (step < 0)  step = 0;

    TSdkAudioLevel lvl;
    memset(&lvl, 0, sizeof lvl);
    lvl.eCapLevel     = SDK_ACAP_LEVEL_2;        // mic sensitivity: irrelevant, this
    lvl.eCapTalkLevel = SDK_ACAP_TALK_LEVEL_2;   // model has no microphone
    lvl.eAmpLevel     = (enSdkAudAmpLevel)step;  // 0 = mute, 10 = maximum
    int rc = TSDK_Req_SetAudLevel(id, &lvl);
    if (rc != CERR_SUCCESS)
        fprintf(stderr, "[vol] SetAudLevel(id=%u, level %d) -> %d\n", id, step, rc);
    else
        fprintf(stderr, "[vol] requested %d/100 = amp level %d on terminal %u\n",
                g_volume, step, id);
}

static void apply_volume_all(void)
{
    pthread_mutex_lock(&g_term_lock);
    int n = g_term_count;
    DWORD ids[MAX_BROAD_TERM];
    for (int i = 0; i < n; i++) ids[i] = g_term_ids[i];
    pthread_mutex_unlock(&g_term_lock);
    for (int i = 0; i < n; i++) apply_volume(ids[i]);
}

// ---- control channel: one line in, one line out, loopback only --------------
static void *ctl_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never exposed to the LAN
    a.sin_port = htons(CTL_PORT);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 4) < 0) {
        fprintf(stderr, "[ctl ] cannot listen on %d: %s\n", CTL_PORT, strerror(errno));
        close(srv);
        return NULL;
    }
    fprintf(stderr, "[ctl ] listening on 127.0.0.1:%d (vol N | get | status)\n", CTL_PORT);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        char line[128] = {0}, reply[256];
        ssize_t n = read(c, line, sizeof line - 1);
        if (n > 0) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            int v;
            if (sscanf(line, "vol %d", &v) == 1 && v >= 0 && v <= 100) {
                g_volume = v;
                apply_volume_all();
                snprintf(reply, sizeof reply, "ok volume=%d\n", g_volume);
            } else if (!strcmp(line, "get")) {
                pthread_mutex_lock(&g_term_lock);
                int cnt = g_term_count;
                DWORD ids[MAX_BROAD_TERM];
                for (int i = 0; i < cnt; i++) ids[i] = g_term_ids[i];
                pthread_mutex_unlock(&g_term_lock);
                for (int i = 0; i < cnt; i++) TSDK_Req_GetAudLevel(ids[i]);
                snprintf(reply, sizeof reply, "ok queried %d terminal(s), see log\n", cnt);
            } else if (!strcmp(line, "status")) {
                pthread_mutex_lock(&g_term_lock);
                int cnt = g_term_count;
                pthread_mutex_unlock(&g_term_lock);
                snprintf(reply, sizeof reply, "terminals=%d volume=%d\n", cnt, g_volume);
            } else {
                snprintf(reply, sizeof reply, "usage: vol <0-100> | get | status\n");
            }
            (void)!write(c, reply, strlen(reply));
        }
        close(c);
    }
    close(srv);
    return NULL;
}

// ---- firmware-port shim: accept on 29783, pump to the SDK on 29883 ----------
// One relayed connection: two sockets and two pump threads. The last thread out
// closes both descriptors. Without the refcount the fds leak on every reconnect,
// and these terminals reconnect often enough to exhaust the process's fd limit
// in a day -- at which point accept() starts failing and the speaker goes quiet
// with nothing in the log to explain it.
struct relay {
    int a, b;
    int refs;
    pthread_mutex_t lock;
};

struct pump { struct relay *r; int from, to; };

static void relay_release(struct relay *r)
{
    pthread_mutex_lock(&r->lock);
    int left = --r->refs;
    pthread_mutex_unlock(&r->lock);
    if (left == 0) {
        close(r->a);
        close(r->b);
        pthread_mutex_destroy(&r->lock);
        free(r);
    }
}

static void *pump_thread(void *arg)
{
    struct pump *p = arg;
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(p->from, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t off = 0; off < n; ) {
            ssize_t w = write(p->to, buf + off, (size_t)(n - off));
            if (w <= 0) goto out;
            off += w;
        }
    }
out:
    shutdown(p->to, SHUT_WR);
    relay_release(p->r);
    free(p);
    return NULL;
}

static void *shim_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(FW_PORT);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 8) < 0) {
        fprintf(stderr, "[shim] cannot listen on %d: %s\n", FW_PORT, strerror(errno));
        close(srv);
        return NULL;
    }
    fprintf(stderr, "[shim] listening :%d -> 127.0.0.1:%d\n", FW_PORT, SDK_PORT);

    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int c = accept(srv, (struct sockaddr *)&ca, &cl);
        if (c < 0) { if (errno == EINTR) continue; break; }

        int u = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ua = {0};
        ua.sin_family = AF_INET; ua.sin_port = htons(SDK_PORT);
        ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (u < 0 || connect(u, (struct sockaddr *)&ua, sizeof ua) < 0) {
            fprintf(stderr, "[shim] upstream connect failed\n");
            if (u >= 0) close(u);
            close(c);
            continue;
        }
        fprintf(stderr, "[shim] %s connected\n", inet_ntoa(ca.sin_addr));

        pthread_t t;
        struct relay *r  = malloc(sizeof *r);
        struct pump  *p1 = malloc(sizeof *p1), *p2 = malloc(sizeof *p2);
        if (!r || !p1 || !p2) {
            free(r); free(p1); free(p2);
            close(c); close(u);
            continue;
        }
        r->a = c; r->b = u; r->refs = 2;
        pthread_mutex_init(&r->lock, NULL);
        p1->r = r; p1->from = c; p1->to = u;
        p2->r = r; p2->from = u; p2->to = c;
        pthread_create(&t, NULL, pump_thread, p1); pthread_detach(t);
        pthread_create(&t, NULL, pump_thread, p2); pthread_detach(t);
    }
    close(srv);
    return NULL;
}

// The SDK documents this callback as "must not block". Keep it to bookkeeping
// and logging; never call back into the broadcast API from here.
static int CALLBACK sdk_callback(enSdkCbType type, LPVOID param, DWORD size, int usr)
{
    (void)size; (void)usr;

    switch (type) {
    case CB_Event_TermRegister: {          // return 0 to accept the registration
        const TSdkEventTermRegister *r = param;
        fprintf(stderr, "[sdk] register: id=%u (zone %u dev %u) ip=%s name=%s\n",
                r->dwTermID, r->dwTermID >> 16, r->dwTermID & 0xffff, r->TermIp, r->TermName);
        remember_term(r->dwTermID);
        return 0;
    }
    case CB_Event_TermConnect: {           // return 0 to allow the connection
        // A terminal only becomes addressable once it has connected, so the group is
        // populated from these events rather than from anything typed by hand.
        const TSdkEventTermConnect *c = param;
        fprintf(stderr, "[sdk] connected: id=%u ip=%s name=%s\n",
                c->dwTermID, c->TermIp, c->TermName);
        remember_term(c->dwTermID);
        apply_volume(c->dwTermID);
        return 0;
    }
    case CB_Event_TermCnnLost:
        fprintf(stderr, "[sdk] terminal connection lost\n");
        return 0;
    case CB_Post_TermState:
        fprintf(stderr, "[sdk] terminal state changed\n");
        return 0;
    case CB_Asw_SetAudLevel: {
        const TSdkAswSetAudLevel *r = param;
        fprintf(stderr, "[vol] terminal %u set -> result %d (value %d, level %d)\n",
                r->dwTermID, r->nResult, r->nAmpValue, (int)r->eAmpLevel);
        return 0;
    }
    case CB_Asw_GetAudLevel: {
        const TSdkAswGetAudLevel *r = param;
        fprintf(stderr, "[vol] terminal %u currently at value %d (level %d)\n",
                r->dwTermID, r->nAmpValue, (int)r->Level.eAmpLevel);
        return 0;
    }
    default:
        (void)param;
        return 0;
    }
}

static int read_exact(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0) return 0;               // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n; len -= (size_t)n;
    }
    return 1;
}

// One Asterisk call. Frames are: type(1) | length(2, big-endian) | payload.
static void serve_call(int fd)
{
    unsigned char hdr[3], payload[65536];
    unsigned char acc[PCM_FRAME * 8];      // re-framing buffer, holds < 320 between frames
    size_t acc_len = 0;
    int broadcasting = 0;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));

    for (;;) {
        int r = read_exact(fd, hdr, sizeof hdr);
        if (r <= 0) break;

        unsigned int len = ((unsigned)hdr[1] << 8) | hdr[2];
        if (len > sizeof payload) {
            fprintf(stderr, "[as ] oversized frame (%u bytes), dropping call\n", len);
            break;
        }
        if (len && read_exact(fd, payload, len) <= 0) break;

        switch (hdr[0]) {
        case AS_TYPE_UUID:
            fprintf(stderr, "[as ] call started, uuid len %u\n", len);
            if (g_group_dirty) { sync_group(); g_group_dirty = 0; }
            int rc = TSDK_StartBroadVoice(BROAD_GROUP);
            if (rc != CERR_SUCCESS) {
                fprintf(stderr, "[sdk] StartBroadVoice -> %d\n", rc);
                goto done;
            }
            broadcasting = 1;
            break;

        case AS_TYPE_AUDIO:
            // Asterisk's frame size is whatever the channel happens to produce —
            // a softphone call arrives as 160-byte (10 ms) frames while file
            // playback gives 320 — but TSDK_BroadPcmVoice only accepts multiples
            // of 320. So accumulate and emit in exact 320-byte units; anything
            // left over waits for the next frame. Dropping odd-sized frames
            // instead means a real call is silent while test tones work.
            if (!broadcasting) break;
            // The length field is 16-bit, so a peer can legitimately announce a
            // frame far larger than the accumulator. Refuse those outright: the
            // previous "reset and copy anyway" guard still memcpy'd len bytes
            // into a 2.5 KB stack buffer, which is a stack smash for anything
            // that can reach the AudioSocket port.
            if (len > sizeof acc) {
                fprintf(stderr, "[as ] %u-byte audio frame exceeds the %zu-byte buffer, dropped\n",
                        len, sizeof acc);
                break;
            }
            if (acc_len + len > sizeof acc) acc_len = 0;      // resync, never overrun
            memcpy(acc + acc_len, payload, len);
            acc_len += len;
            if (acc_len >= PCM_FRAME) {
                size_t send = (acc_len / PCM_FRAME) * PCM_FRAME;
                int rc = TSDK_BroadPcmVoice(BROAD_GROUP, acc, (int)send);
                if (rc != CERR_SUCCESS)
                    fprintf(stderr, "[sdk] BroadPcmVoice(%zu) -> %d\n", send, rc);
                acc_len -= send;
                memmove(acc, acc + send, acc_len);
            }
            break;

        case AS_TYPE_ERROR:
            fprintf(stderr, "[as ] error frame from Asterisk\n");
            break;

        case AS_TYPE_TERMINATE:
            goto done;

        default:
            break;                          // unknown types are ignored by design
        }
    }

done:
    // A partial tail (<320 bytes) is dropped on purpose: padding it with silence
    // would add a click, and 20 ms of audio at the very end of a page is nothing.
    if (broadcasting) TSDK_StopBroadVoice(BROAD_GROUP);
    close(fd);
    fprintf(stderr, "[as ] call ended\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr,
            "usage: %s [audiosocket-port]\n"
            "  Terminals are learned from the SDK's own connect events; point each\n"
            "  speaker's Center Server at this host and they join the page group.\n"
            "  audiosocket-port defaults to 9092.\n"
            "  PA_BRIDGE_VOLUME=0..100 sets the speaker volume applied on connect.\n"
            "  Runtime control on 127.0.0.1:%d -- 'vol <0-100>', 'get', 'status'.\n",
            argv[0], CTL_PORT);
        return 2;
    }
    if (argc > 1) g_listen_port = atoi(argv[1]);
    const char *env = getenv("PA_BRIDGE_VOLUME");
    if (env) {
        int v = atoi(env);
        if (v >= 0 && v <= 100) g_volume = v;
        else fprintf(stderr, "PA_BRIDGE_VOLUME=%s ignored, expected 0..100\n", env);
    }
    fprintf(stderr, "[vol ] speaker volume will be set to %d/100 on connect\n", g_volume);

    // sigaction without SA_RESTART on purpose: the accept() below must return
    // EINTR on SIGTERM, otherwise glibc restarts it and the process never exits.
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int rc = TSDK_Init(sdk_callback, TRUE, SDK_MAGIC);
    if (rc != CERR_SUCCESS) {
        fprintf(stderr, "TSDK_Init -> %d\n", rc);
        return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, shim_thread, NULL); pthread_detach(t);
    pthread_create(&t, NULL, ctl_thread,  NULL); pthread_detach(t);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); TSDK_DeInit(); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)g_listen_port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 4) < 0) {
        perror("bind/listen"); close(srv); TSDK_DeInit(); return 1;
    }
    fprintf(stderr, "[as ] AudioSocket listening on :%d\n", g_listen_port);

    // One call at a time, deliberately: a PA speaker can only say one thing at
    // once, and serialising here means no locking around the SDK.
    while (!g_stop) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        serve_call(fd);
    }

    close(srv);
    TSDK_DeInit();
    fprintf(stderr, "bye\n");
    return 0;
}
