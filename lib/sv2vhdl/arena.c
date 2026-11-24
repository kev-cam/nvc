// arena.c -- libarena: nets resolved across an arena shared by several nvc simulations
//
// Loaded with  nvc -r --load=libarena.so  it is both a VHPI plugin and the
// foreign library of arena_pkg.vhd.  The resolver plugin generates, for a net
// whose type says RESOLUTION="arena" and ARENA_BAND="<band>", one resolver
// per local endpoint; those call in here for the endpoints of the other
// simulations in the same arena.
//
// Which arena a band is in is told at launch, by the federation controller
// (arena_ctl.py):
//
//   FED_NAME     this simulation's name in the federation
//   FED_ARENAS   band=arena[@host:port],...   the logical arena of each band
//                and its rendezvous point (default: host = the arena's name,
//                which the arena's network resolves, port 7600)
//   FED_TRACE=1  trace on stderr
//
// A band that is not mapped is resolved inside this simulation only.
//
// The arena is not a process of its own: it is the rendezvous point, where the
// simulations learn about each other, and then they connect to each other
// directly.  The federation is asynchronous:
//
//   * Every local endpoint's driver (and place) is sent to every other
//     simulation as soon as it is known, tagged with its time; a reader thread
//     per peer takes everything in as it arrives.  Each receiver holds a copy
//     of each remote end's scheduled waveform and uses each value from its
//     time on: one for `now` runs the resolvers again in a new delta (the
//     kick signal), one for later waits in the copy and a vhpiCbAfterDelay
//     makes a time step for it.
//   * Each simulation tells the others its current time, whether the delta
//     cycles at that time are complete, when its next event is, and how many
//     values it has sent to and received from each of them (STATUS, sent
//     whenever that changes, at the end of its deltas:
//     vhpiCbRepLastKnownDeltaCycle).
//   * Nobody waits for anyone it does not have to.  A simulation whose deltas
//     are complete moves on to its next event as soon as every other
//     simulation has said its deltas are complete, every value sent has been
//     received (the counts match, pair by pair), and no one's next event is
//     earlier.  Otherwise it waits for the next message.  A simulation whose
//     next event is later stays where it is (its time does not move until it
//     has something to do), and the ones with the earliest next event go on
//     without it.
//
// So the federation makes the same events at the same times as one
// simulation of the whole design; only the delta numbering differs.
//
// Each endpoint has a global index, the same in every simulation:
// simulations in rendezvous order, then each one's endpoints in its own
// order.  A simulation that leaves (its connection closes) drops out of the
// arena and the indices close up; joining a running arena is not supported
// yet (the rendezvous refuses it).
//
// Wire format (little-endian): u32 type, u32 body length, body.
//   to the rendezvous:  JOIN   "name\narena\nport\nnlocal"
//   from it:            PEERS  lines "peer <name> <host> <port> <nlocal>", in
//                              rendezvous order, this simulation too
//                       ERROR  text
//   between peers:      HELLO  name
//                       VALUE  i64 time (fs), u32 what, u32 end, u32 n, n doubles
//                              (what 0: the end's driver, 1..: a place field)
//                       STATUS i64 now (fs; the deltas at now are complete),
//                              i64 next (fs, -1: none), i64 quiet (fs: none of
//                              its ends changes before then, -1: no such
//                              promise), u32 npeers, then per
//                              simulation in rendezvous order u64 values sent
//                              to it, u64 values received from it, i64 its
//                              slack to it (fs, 0: none known)
//
// Slack.  When the resolvers apply the physical delay of each path
// (ARENA_DELAY), they tell this library the delay of every path into each
// local end (arena_slack).  The shortest one from another simulation's ends is
// the slack to that simulation: nothing it emits at T can be heard here before
// T + slack, so a time it reports is known good here a slack further on, and
// this simulation runs ahead of it that far without waiting and without
// speculating -- 10 ft apart is 10 ns of slack, 1 km is 3.3 us.  The slack
// follows the geometry as the ends move (anything slower than light cannot
// make a later emission arrive before an earlier one's bound); a value that
// arrives too late anyway (an end that jumped closer) stops the simulation.
// Without delays the slack is 0 and the simulations keep together delta cycle
// by delta cycle.
//
// Scheduled ahead.  A model that drives an end with  p'driver <= v after d
// says v holds from now + d and that the end does not change before then.
// libarena watches each local end's 'driver (nvc_vhpi_sched_watch), sends v
// tagged now + d as soon as it is scheduled -- each receiver keeps it in its
// copy of the waveform -- and reports the earliest such promise over its
// ends (STATUS quiet): a peer's emissions are then bounded by that too, not
// only by its next event, so the others run up to it + the slack without
// waiting.  Taking a promise back (a later assignment for an earlier time,
// or a value at a time that was promised quiet) stops the simulation.

#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vhpi_user.h"

enum { M_JOIN = 1, M_PEERS = 2, M_ERROR = 3, M_HELLO = 4, M_VALUE = 5, M_STATUS = 7, M_GONE = 99 };

#define MAX_NETS 64
#define MAX_PEERS 256
#define MAX_WHAT 9
#define MAX_ARENAS 32

typedef struct msg {
    struct msg *next;
    uint32_t type, len;
    uint8_t body[];
} msg_t;

typedef struct pend {                   // a value scheduled for later
    struct pend *next;
    int64_t t;
    uint32_t n;
    double v[];
} pend_t;

typedef struct {                        // a driver's history, by emission time
    int64_t *t;
    uint32_t *n;
    double **v;
    int len, cap;
    int trimmed;                        // older entries were dropped: the first one stands for them
} hist_t;

typedef struct {
    double *v;                          // the value now
    uint32_t n, cap;
    pend_t *pending;                    // the rest of the waveform, in time order
    hist_t h;                           // drivers: what was emitted when (delayed resolution)
} slot_t;

typedef struct {
    char name[128], host[256];
    int port, nlocal, sock, alive, self;
    pthread_t th;
    msg_t *head, *tail;                 // what the reader thread took in, oldest first
    slot_t *slots;                      // [nlocal * MAX_WHAT]: each value's waveform
    uint64_t bytes_out, bytes_in;
    uint64_t sent, recv;                // values sent to it, received from it
    int have_status;                    // its last STATUS:
    int64_t st_now, st_next, st_quiet;
    uint64_t *st_sent, *st_recv;        // [npeers]
    int64_t *st_slack;                  // [npeers]: its slack to each simulation
    int64_t slack;                      // this simulation's slack to it (fs)
    int64_t *etime;                     // emission time of each value sent to it not yet
    uint64_t ebase;                     //   known to be received: value ebase + k at etime[k]
    int elen, ecap;
} peer_t;

typedef struct {
    int used, nlocal, joined;           // joined: in the federated arena (else local only)
    int delayed;                        // the resolver applies each path's delay
    char band[64], kick_name[512];
    vhpiHandleT kick;
    int kick_count;
    slot_t *local;                      // [nlocal]: the local ends' driver histories
    int64_t *dly;                       // [nlocal * dly_total]: path delays into the local ends (-1: unknown)
    int dly_total;
    int *watched;                       // [nlocal]: its 'driver is watched as scheduled
    int64_t *promise;                   // [nlocal]: no change before then (-1: none)
    double **last;                      // [nlocal]: the last value sent
    uint32_t *nlast;
} net_t;

typedef struct {
    char band[64], arena[128], rv[384];
} map_t;

static net_t nets[MAX_NETS];
static map_t maps[MAX_ARENAS];
static int nmaps = 0;

// The arena this simulation is in.  One for now: a simulation in two arenas
// needs the time rounds agreed across both, which is not done yet.
static int fed_net = -1;
static char arena_name[128];
static peer_t peers[MAX_PEERS];
static int npeers = 0, me = -1;
static int *remote_peer = NULL, *remote_end = NULL, *remote_global = NULL, nremote = 0, total = 0, my_base = 0;
static int *g_peer = NULL, *g_end = NULL;   // [total]: every end by global index
static int64_t hist_keep = 1000000000000LL; // fs of history kept behind now (1 ms: 300 km)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int trace = 0, finished = 0;
static const char *fed_name = NULL;
static int npending = 0;                // values waiting in the waveform copies
static uint64_t n_status = 0, n_delivers = 0, n_advances = 0, n_puts = 0, n_waits = 0;
static double t_blocked = 0.0;

static uint64_t now_fs(void);

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "libarena: ");
    fprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    exit(2);
}

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

// ---- sockets ----

static int send_all(int s, const void *d, size_t n)
{
    const uint8_t *p = d;
    while (n) {
        ssize_t k = send(s, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static int recv_all(int s, void *d, size_t n)
{
    uint8_t *p = d;
    while (n) {
        ssize_t k = recv(s, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static int send_msg2(int s, uint32_t type, const void *a, uint32_t na, const void *b, uint32_t nb)
{
    uint32_t h[2] = { type, na + nb };
    if (send_all(s, h, 8) || (na && send_all(s, a, na)) || (nb && send_all(s, b, nb))) return -1;
    return 0;
}

static msg_t *recv_msg(int s)
{
    uint32_t h[2];
    if (recv_all(s, h, 8)) return NULL;
    msg_t *m = malloc(sizeof(msg_t) + h[1] + 1);
    m->next = NULL;
    m->type = h[0];
    m->len = h[1];
    if (h[1] && recv_all(s, m->body, h[1])) {
        free(m);
        return NULL;
    }
    m->body[h[1]] = 0;
    return m;
}

static int dial(const char *host, const char *port, int tries)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    for (int attempt = 0; attempt < tries; attempt++) {
        if (getaddrinfo(host, port, &hints, &res) == 0) {
            for (struct addrinfo *a = res; a; a = a->ai_next) {
                int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s < 0) continue;
                if (connect(s, a->ai_addr, a->ai_addrlen) == 0) {
                    freeaddrinfo(res);
                    int one = 1;
                    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    return s;
                }
                close(s);
            }
            freeaddrinfo(res);
        }
        usleep(100000);
    }
    return -1;
}

static void split_hostport(const char *hp, char *host, size_t hn, char *port, size_t pn)
{
    const char *colon = strrchr(hp, ':');
    const char *close_br = strrchr(hp, ']');
    if (!colon || (close_br && colon < close_br)) {      // no port: the default
        size_t hl = strlen(hp);
        if (hp[0] == '[' && hl >= 2 && hp[hl - 1] == ']') {
            hp++;
            hl -= 2;
        }
        if (hl >= hn) die("%s: host too long", hp);
        memcpy(host, hp, hl);
        host[hl] = 0;
        snprintf(port, pn, "7600");
        return;
    }
    size_t hl = colon - hp;
    if (hp[0] == '[' && hl >= 2 && hp[hl - 1] == ']') {
        hp++;
        hl -= 2;
    }
    if (hl >= hn) die("%s: host too long", hp);
    memcpy(host, hp, hl);
    host[hl] = 0;
    snprintf(port, pn, "%s", colon + 1);
}

// ---- reader threads: take in everything a peer sends, as it arrives ----

static void *reader(void *arg)
{
    peer_t *p = arg;
    for (;;) {
        msg_t *m = recv_msg(p->sock);
        if (!m) {
            m = malloc(sizeof(msg_t) + 1);
            m->next = NULL;
            m->type = M_GONE;
            m->len = 0;
        } else {
            p->bytes_in += 8 + m->len;
        }
        pthread_mutex_lock(&mu);
        if (p->tail) p->tail->next = m;
        else p->head = m;
        p->tail = m;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        if (m->type == M_GONE) return NULL;
    }
}

// ---- the arena's membership: global indices ----

static void index_arena(void)
{
    free(remote_peer);
    free(remote_end);
    free(remote_global);
    int n = 0;
    for (int i = 0; i < npeers; i++)
        if (!peers[i].self && peers[i].alive) n += peers[i].nlocal;
    remote_peer = malloc(sizeof(int) * (n + 1));
    remote_end = malloc(sizeof(int) * (n + 1));
    remote_global = malloc(sizeof(int) * (n + 1));
    int g = 0, k = 0;
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self) {
            my_base = g;
            g += nets[fed_net].nlocal;
            continue;
        }
        if (!p->alive) continue;
        for (int j = 0; j < p->nlocal; j++, k++, g++) {
            remote_peer[k] = i;
            remote_end[k] = j;
            remote_global[k] = g;
        }
    }
    nremote = n;
    total = g;
    free(g_peer);
    free(g_end);
    g_peer = malloc(sizeof(int) * (total + 1));
    g_end = malloc(sizeof(int) * (total + 1));
    g = 0;
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (!p->self && !p->alive) continue;
        int nl = p->self ? nets[fed_net].nlocal : p->nlocal;
        for (int j = 0; j < nl; j++, g++) {
            g_peer[g] = i;
            g_end[g] = j;
        }
    }
}

// ---- driver histories ----

static void hist_add(hist_t *h, int64_t t, const double *v, uint32_t n, int64_t now)
{
    while (h->len && h->t[h->len - 1] >= t) {           // a later value replaces what was after it
        h->len--;
        free(h->v[h->len]);
    }
    int drop = 0;                                       // keep one entry from before now - hist_keep
    while (drop + 1 < h->len && h->t[drop + 1] <= now - hist_keep) drop++;
    if (drop) {
        for (int i = 0; i < drop; i++) free(h->v[i]);
        memmove(h->t, h->t + drop, (h->len - drop) * sizeof(int64_t));
        memmove(h->n, h->n + drop, (h->len - drop) * sizeof(uint32_t));
        memmove(h->v, h->v + drop, (h->len - drop) * sizeof(double *));
        h->len -= drop;
        h->trimmed = 1;
    }
    if (h->len == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 16;
        h->t = realloc(h->t, h->cap * sizeof(int64_t));
        h->n = realloc(h->n, h->cap * sizeof(uint32_t));
        h->v = realloc(h->v, h->cap * sizeof(double *));
    }
    h->t[h->len] = t;
    h->n[h->len] = n;
    h->v[h->len] = malloc(n * sizeof(double) + 1);
    memcpy(h->v[h->len], v, n * sizeof(double));
    h->len++;
}

// The entry in force at t; -1: nothing emitted by then (nothing is heard
// from an end before its first emission reaches the listener).
static int hist_at(const hist_t *h, int64_t t)
{
    if (!h->len) return -1;
    int lo = 0, hi = h->len - 1;
    if (h->t[0] > t) return h->trimmed ? 0 : -1;
    while (lo < hi) {                                   // last entry with time <= t
        int mid = (lo + hi + 1) / 2;
        if (h->t[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// ---- joining: the rendezvous, then a connection to every other simulation ----

static void join(int net, const map_t *mp)
{
    char host[256], port[32];
    split_hostport(mp->rv, host, sizeof host, port, sizeof port);
    int rv = dial(host, port, 300);                     // the rendezvous may still be starting: 30 s
    if (rv < 0) die("cannot reach the rendezvous of arena %s", mp->rv);

    // listen on the address this host reaches the rendezvous from: the arena's network
    struct sockaddr_storage la;
    socklen_t ll = sizeof la;
    getsockname(rv, (struct sockaddr *)&la, &ll);
    int ls = socket(la.ss_family, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (la.ss_family == AF_INET6) ((struct sockaddr_in6 *)&la)->sin6_port = 0;
    else ((struct sockaddr_in *)&la)->sin_port = 0;
    if (bind(ls, (struct sockaddr *)&la, ll) || listen(ls, MAX_PEERS)) die("cannot listen: %s", strerror(errno));
    ll = sizeof la;
    getsockname(ls, (struct sockaddr *)&la, &ll);
    int lport = ntohs(la.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&la)->sin6_port
                                               : ((struct sockaddr_in *)&la)->sin_port);
    char j[512];
    int jn = snprintf(j, sizeof j, "%s\n%s\n%d\n%d", fed_name, mp->arena, lport, nets[net].nlocal);
    if (send_msg2(rv, M_JOIN, j, jn, NULL, 0)) die("lost the rendezvous%s", "");
    msg_t *m = recv_msg(rv);
    if (!m || m->type != M_PEERS) die("the rendezvous refused this simulation: %s", m ? (char *)m->body : "closed");
    close(rv);

    char *save = NULL;
    for (char *line = strtok_r((char *)m->body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "peer ", 5)) continue;
        if (npeers >= MAX_PEERS) die("too many simulations in the arena%s", "");
        peer_t *p = &peers[npeers];
        if (sscanf(line + 5, "%127s %255s %d %d", p->name, p->host, &p->port, &p->nlocal) != 4)
            die("bad peer line: %s", line);
        p->self = !strcmp(p->name, fed_name);
        if (p->self) me = npeers;
        p->sock = -1;
        p->slots = calloc((size_t)(p->nlocal > 0 ? p->nlocal : 1) * MAX_WHAT, sizeof(slot_t));
        npeers++;
    }
    free(m);
    for (int i = 0; i < npeers; i++) {
        peers[i].st_sent = calloc(npeers, sizeof(uint64_t));
        peers[i].st_recv = calloc(npeers, sizeof(uint64_t));
        peers[i].st_slack = calloc(npeers, sizeof(int64_t));
    }
    if (me < 0) die("the rendezvous did not list %s", fed_name);

    // connect to those after this one, accept those before (their listeners are up before they join)
    for (int i = me + 1; i < npeers; i++) {
        char ps[16];
        snprintf(ps, sizeof ps, "%d", peers[i].port);
        peers[i].sock = dial(peers[i].host, ps, 100);
        if (peers[i].sock < 0) die("cannot reach simulation %s", peers[i].name);
        if (send_msg2(peers[i].sock, M_HELLO, fed_name, strlen(fed_name), NULL, 0)) die("lost %s", peers[i].name);
    }
    for (int k = 0; k < me; k++) {
        int s = accept(ls, NULL, NULL);
        if (s < 0) die("accept: %s", strerror(errno));
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        msg_t *h = recv_msg(s);
        if (!h || h->type != M_HELLO) die("a simulation did not say who it is%s", "");
        int found = 0;
        for (int i = 0; i < me; i++)
            if (!strcmp(peers[i].name, (char *)h->body) && peers[i].sock < 0) {
                peers[i].sock = s;
                found = 1;
            }
        if (!found) die("unexpected simulation %s", (char *)h->body);
        free(h);
    }
    close(ls);
    for (int i = 0; i < npeers; i++) {
        if (peers[i].self) continue;
        peers[i].alive = 1;
        pthread_create(&peers[i].th, NULL, reader, &peers[i]);
    }
    fed_net = net;
    snprintf(arena_name, sizeof arena_name, "%s", mp->arena);
    index_arena();
    if (trace)
        fprintf(stderr, "arena: %s joined arena %s with %d other simulation(s), %d endpoints (%d here, from %d)\n",
                fed_name, arena_name, npeers - 1, total, nets[net].nlocal, my_base);
}

// ---- the design's side (VHPIDIRECT, as arena_pkg.vhd declares it) ----

static net_t *get_net(int32_t net)
{
    if (net < 0 || net >= MAX_NETS || !nets[net].used) die("arena net not attached%s", "");
    return &nets[net];
}

static int is_fed(int32_t net) { return net == fed_net && !finished; }

void arena_attach(int32_t net, const char *band, int64_t blen, int32_t nlocal, const char *kick, int64_t klen,
                  int32_t delayed)
{
    if (net < 0 || net >= MAX_NETS) die("arena net number out of range%s", "");
    net_t *n = &nets[net];
    if (n->used) return;
    n->used = 1;
    n->nlocal = nlocal;
    n->delayed = delayed;
    n->local = calloc(nlocal > 0 ? nlocal : 1, sizeof(slot_t));
    n->watched = calloc(nlocal > 0 ? nlocal : 1, sizeof(int));
    n->promise = malloc(sizeof(int64_t) * (nlocal > 0 ? nlocal : 1));
    for (int j = 0; j < nlocal; j++) n->promise[j] = -1;
    n->last = calloc(nlocal > 0 ? nlocal : 1, sizeof(double *));
    n->nlast = calloc(nlocal > 0 ? nlocal : 1, sizeof(uint32_t));
    snprintf(n->band, sizeof n->band, "%.*s", (int)blen, band);
    snprintf(n->kick_name, sizeof n->kick_name, "%.*s", (int)klen, kick);
    const map_t *mp = NULL;
    for (int i = 0; i < nmaps; i++)
        if (!strcasecmp(maps[i].band, n->band)) mp = &maps[i];
    if (!mp || !fed_name) {
        if (trace) fprintf(stderr, "arena: band %s not mapped: resolved here only\n", n->band);
        return;
    }
    if (fed_net >= 0) die("this simulation is already in an arena (one arena per simulation for now): %s", n->band);
    n->joined = 1;
    join(net, mp);
}

static void broadcast_at(int64_t t, uint32_t what, uint32_t end, const double *v, int64_t n)
{
    uint8_t hdr[20];
    const uint32_t h[3] = { what, end, (uint32_t)n };
    memcpy(hdr, &t, 8);
    memcpy(hdr + 8, h, 12);
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self || !p->alive) continue;
        if (send_msg2(p->sock, M_VALUE, hdr, 20, v, (uint32_t)(n * sizeof(double)))) {
            p->alive = 0;
            continue;
        }
        p->bytes_out += 28 + n * sizeof(double);
        if (nets[fed_net].delayed) {                    // when it was emitted, until p says it has it
            if (p->elen == p->ecap) {
                p->ecap = p->ecap ? 2 * p->ecap : 64;
                p->etime = realloc(p->etime, p->ecap * sizeof(int64_t));
            }
            p->etime[p->elen++] = t;
        }
        p->sent++;
    }
    n_puts++;
}

static void broadcast(uint32_t what, uint32_t end, const double *v, int64_t n)
{
    broadcast_at((int64_t)now_fs(), what, end, v, n);
}

static void remember(net_t *nt, int32_t j, const double *v, uint32_t n)
{
    if (nt->nlast[j] != n) {
        nt->last[j] = realloc(nt->last[j], (n ? n : 1) * sizeof(double));
        nt->nlast[j] = n;
    }
    memcpy(nt->last[j], v, n * sizeof(double));
}

static int same_as_last(net_t *nt, int32_t j, const double *v, uint32_t n)
{
    return nt->last[j] && nt->nlast[j] == n && !memcmp(nt->last[j], v, n * sizeof(double));
}

// ---- scheduled ahead: a local end's 'driver as it is scheduled ----

extern int nvc_vhpi_sched_watch(const vhpiCharT *inst_path, const vhpiCharT *port,
                                void (*cb)(void *ctx, int64_t when, const double *v, int32_t n), void *ctx);
extern void nvc_vhpi_sched_flush(void);
static int n_scheduled = 0;             // values sent ahead of their time

// Values sent ahead and not yet in effect, per local end, in time order.
typedef struct sched_q { struct sched_q *next; int64_t t; uint32_t n; double v[]; } sched_q_t;
static sched_q_t **ahead = NULL;        // [nlocal]

static void scheduled_cb(void *ctx, int64_t when, const double *v, int32_t n)
{
    const int32_t j = (int32_t)(intptr_t)ctx;
    if (fed_net < 0 || finished) return;
    net_t *nt = &nets[fed_net];
    if (nt->promise[j] >= 0 && when < nt->promise[j]) {
        fprintf(stderr, "libarena: end %d was promised unchanged until %lld fs, but a value is now scheduled "
                "for %lld fs (taking a scheduled transaction back is not allowed: use transport delay)\n", j,
                (long long)nt->promise[j], (long long)when);
        exit(2);
    }
    if (!ahead) ahead = calloc(nt->nlocal > 0 ? nt->nlocal : 1, sizeof(sched_q_t *));
    sched_q_t **pp = &ahead[j], *prev = NULL;
    while (*pp && (*pp)->t < when) { prev = *pp; pp = &(*pp)->next; }
    nt->promise[j] = when;
    if (*pp && (*pp)->t == when) {                      // the same time again: replace it
        sched_q_t *old = *pp;
        *pp = old->next;
        free(old);
    }
    else {                                              // unchanged from what is in force by then:
        const double *before = prev ? prev->v : nt->last[j];   // the promise alone goes in STATUS
        const uint32_t nb = prev ? prev->n : nt->nlast[j];
        if (before && nb == (uint32_t)n && !memcmp(before, v, n * sizeof(double))) return;
    }
    sched_q_t *q = malloc(sizeof(sched_q_t) + n * sizeof(double));
    q->t = when;
    q->n = (uint32_t)n;
    memcpy(q->v, v, n * sizeof(double));
    q->next = *pp;
    *pp = q;
    broadcast_at(when, 0, (uint32_t)j, v, n);
    n_scheduled++;
}

// The generated resolver names each local end's instance and port: watch
// its 'driver.  Without the nvc extension (or for a type it cannot lay out)
// the end's values go out when they take effect, as before.
void arena_watch(int32_t net, int32_t j, const char *inst, int64_t ilen, const char *port, int64_t plen)
{
    net_t *nt = get_net(net);
    if (!is_fed(net) || !nt->delayed || j < 0 || j >= nt->nlocal || nt->watched[j]) return;
    char ib[512], pb[128];
    snprintf(ib, sizeof ib, "%.*s", (int)ilen, inst);
    snprintf(pb, sizeof pb, "%.*s", (int)plen, port);
    const int n = nvc_vhpi_sched_watch((const vhpiCharT *)ib, (const vhpiCharT *)pb, scheduled_cb, (void *)(intptr_t)j);
    nt->watched[j] = n > 0 ? 1 : -1;
    if (trace) fprintf(stderr, "arena: end %d (%s %s): %s\n", j, ib, pb,
                       n > 0 ? "sent as scheduled" : "not watchable: sent when it takes effect");
}

// The earliest time any local end may change (-1: some end has no promise).
static int64_t quiet_until(int64_t now)
{
    net_t *nt = &nets[fed_net];
    int64_t q = -1;
    for (int j = 0; j < nt->nlocal; j++) {
        if (nt->watched[j] != 1 || nt->promise[j] <= now) return -1;
        if (q < 0 || nt->promise[j] < q) q = nt->promise[j];
    }
    return q;
}

void arena_put(int32_t net, int32_t j, const double *v, int64_t n)
{
    net_t *nt = get_net(net);
    if (j < 0 || j >= nt->nlocal) die("local end out of range%s", "");
    const int64_t now = (int64_t)now_fs();
    if (nt->delayed) hist_add(&nt->local[j].h, now, v, (uint32_t)n, now);
    if (!is_fed(net)) return;
    if (nt->watched[j] == 1) {
        sched_q_t *q = ahead ? ahead[j] : NULL;
        while (q && q->t < now) {                       // (none should be left behind)
            ahead[j] = q->next;
            free(q);
            q = ahead[j];
        }
        if (q && q->t == now) {                         // sent when it was scheduled
            const int ok = q->n == (uint32_t)n && !memcmp(q->v, v, n * sizeof(double));
            ahead[j] = q->next;
            free(q);
            if (!ok) {
                fprintf(stderr, "libarena: end %d took effect at %lld fs with a value other than the one "
                        "scheduled for then\n", j, (long long)now);
                exit(2);
            }
            remember(nt, j, v, (uint32_t)n);
            return;
        }
        if (same_as_last(nt, j, v, (uint32_t)n)) return;
        if (nt->promise[j] > now) {
            fprintf(stderr, "libarena: end %d changed at %lld fs, but was promised unchanged until %lld fs\n",
                    j, (long long)now, (long long)nt->promise[j]);
            exit(2);
        }
    }
    remember(nt, j, v, (uint32_t)n);
    broadcast(0, (uint32_t)j, v, n);
}

static hist_t *end_hist(int32_t net, int32_t g)
{
    net_t *nt = get_net(net);
    if (net != fed_net) {
        if (g < 0 || g >= nt->nlocal) die("end out of range%s", "");
        return &nt->local[g].h;
    }
    if (g < 0 || g >= total) die("end out of range%s", "");
    peer_t *p = &peers[g_peer[g]];
    return p->self ? &nt->local[g_end[g]].h : &p->slots[g_end[g] * MAX_WHAT].h;
}

int32_t arena_len_at(int32_t net, int32_t g, int64_t t)
{
    hist_t *h = end_hist(net, g);
    int i = hist_at(h, t);
    return i < 0 ? 0 : (int32_t)h->n[i];
}

void arena_get_at(int32_t net, int32_t g, int64_t t, double *v, int64_t n)
{
    hist_t *h = end_hist(net, g);
    int i = hist_at(h, t);
    if (i < 0) return;
    int64_t m = (int64_t)h->n[i] < n ? (int64_t)h->n[i] : n;
    memcpy(v, h->v[i], m * sizeof(double));
}

int64_t arena_next_after(int32_t net, int32_t g, int64_t t)
{
    hist_t *h = end_hist(net, g);
    if (!h->len || h->t[h->len - 1] <= t) return -1;
    if (h->t[0] > t) return h->trimmed ? (h->len > 1 ? h->t[1] : -1) : h->t[0];
    return h->t[hist_at(h, t) + 1];
}

// The resolver of local end j found the path from end g (global) to take d:
// the slack to g's simulation is the shortest such path into any local end.
void arena_slack(int32_t net, int32_t j, int32_t g, int64_t d)
{
    net_t *n = get_net(net);
    if (net != fed_net || g < 0 || g >= total || j < 0 || j >= n->nlocal) return;
    peer_t *p = &peers[g_peer[g]];
    if (p->self) return;
    if (n->dly_total != total) {                        // the arena changed: start again
        free(n->dly);
        n->dly = malloc(sizeof(int64_t) * (size_t)n->nlocal * total);
        for (int k = 0; k < n->nlocal * total; k++) n->dly[k] = -1;
        n->dly_total = total;
    }
    n->dly[j * total + g] = d;
    int64_t s = -1;
    for (int i = 0; i < n->nlocal && s != 0; i++)
        for (int h = 0; h < total; h++) {
            if (&peers[g_peer[h]] != p) continue;
            const int64_t x = n->dly[i * total + h];
            if (x < 0) { s = 0; break; }                // not known for every path: none
            if (s < 0 || x < s) s = x;
        }
    p->slack = s < 0 ? 0 : s;
}

void arena_place(int32_t net, int32_t j, int32_t what, const double *v, int64_t n)
{
    get_net(net);
    if (what < 1 || what >= MAX_WHAT) die("place field out of range%s", "");
    if (is_fed(net)) broadcast((uint32_t)what, (uint32_t)j, v, n);
}

int32_t arena_total(int32_t net)
{
    net_t *n = get_net(net);
    return net == fed_net ? total : n->nlocal;
}

int32_t arena_global(int32_t net, int32_t j)
{
    get_net(net);
    return net == fed_net ? my_base + j : j;
}

int32_t arena_remotes(int32_t net)
{
    get_net(net);
    return net == fed_net ? nremote : 0;
}

int32_t arena_remote_global(int32_t net, int32_t k)
{
    get_net(net);
    if (net != fed_net || k < 0 || k >= nremote) die("remote endpoint out of range%s", "");
    return remote_global[k];
}

static slot_t *remote_slot(int32_t net, int32_t k, int32_t what)
{
    get_net(net);
    if (net != fed_net || k < 0 || k >= nremote || what < 0 || what >= MAX_WHAT)
        die("remote endpoint out of range%s", "");
    return &peers[remote_peer[k]].slots[remote_end[k] * MAX_WHAT + what];
}

int32_t arena_len(int32_t net, int32_t k, int32_t what)
{
    return (int32_t)remote_slot(net, k, what)->n;
}

void arena_get(int32_t net, int32_t k, int32_t what, double *v, int64_t n)
{
    slot_t *s = remote_slot(net, k, what);
    int64_t m = (int64_t)s->n < n ? (int64_t)s->n : n;
    memcpy(v, s->v, m * sizeof(double));
}

// ---- time ----

static uint64_t now_fs(void)
{
    vhpiTimeT t;
    vhpi_get_time(&t, NULL);
    return ((uint64_t)t.high << 32) | t.low;
}

static int64_t next_fs(void)
{
    vhpiTimeT nt;
    if (vhpi_get_next_time(&nt) == vhpiNoActivity) return -1;
    return (int64_t)(((uint64_t)nt.high << 32) | nt.low);
}

static vhpiHandleT find_kick(const char *path)
{
    // 'path_name is ":top:inst:kick"; VHPI names are "top.inst.kick" (or ":top:inst:kick")
    vhpiHandleT h = vhpi_handle_by_name(path, NULL);
    if (h) return h;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path[0] == ':' ? path + 1 : path);
    for (char *c = buf; *c; c++)
        if (*c == ':') *c = '.';
    return vhpi_handle_by_name(buf, NULL);
}

static void after_cb(const vhpiCbDataT *d) { (void)d; }   // only makes the time step

static void set_slot(slot_t *s, const double *v, uint32_t n)
{
    if (s->cap < n) {
        s->cap = n;
        s->v = realloc(s->v, n * sizeof(double));
    }
    memcpy(s->v, v, n * sizeof(double));
    s->n = n;
}

// A VALUE from p: into p's waveform copy for that end.  A value for now is used
// at once (returns 1 for a driver: the resolvers must run again); one for later
// replaces whatever the copy had from its time on, and waits.
static int stage(peer_t *p, msg_t *m, int64_t now)
{
    int64_t t;
    uint32_t what, end, n;
    memcpy(&t, m->body, 8);
    memcpy(&what, m->body + 8, 4);
    memcpy(&end, m->body + 12, 4);
    memcpy(&n, m->body + 16, 4);
    if (what >= MAX_WHAT || (int)end >= p->nlocal || 20 + 8 * (size_t)n > m->len)
        die("%s sent a value out of range", p->name);
    const double *v = (const double *)(m->body + 20);
    slot_t *s = &p->slots[end * MAX_WHAT + what];
    pend_t **pp = &s->pending;
    while (*pp && (*pp)->t < t) pp = &(*pp)->next;
    while (*pp) {                                       // the copy changes from t on
        pend_t *dead = *pp;
        *pp = dead->next;
        free(dead);
        npending--;
    }
    if (nets[fed_net].delayed && what == 0)             // the resolvers read it at t + each path's delay
        hist_add(&s->h, t, v, n, now);
    if (nets[fed_net].delayed) {
        // The sender may be behind this simulation by up to the slack: its
        // signal reaches here no sooner than t + slack, which is not past.
        if (what == 0 && t + p->slack < now) {
            fprintf(stderr, "libarena: %s emitted at %lld fs, which reaches here at %lld fs at the earliest "
                    "(slack %lld fs), but this simulation is already at %lld fs: did an end jump closer "
                    "faster than light?\n", p->name, (long long)t, (long long)(t + p->slack),
                    (long long)p->slack, (long long)now);
            exit(2);
        }
        if (what == 0) return 1;                        // the resolvers find out when it arrives
        if (t <= now) {                                 // a place: where it is from now on (the
            set_slot(s, v, n);                          //   resolvers work out the new delays)
            return 1;
        }
    } else if (t < now) {
        fprintf(stderr, "libarena: %s sent a value for %lld fs, this simulation is already at %lld fs\n",
                p->name, (long long)t, (long long)now);
        exit(2);
    }
    if (t == now) {
        set_slot(s, v, n);
        return what == 0;
    }
    pend_t *q = malloc(sizeof(pend_t) + n * sizeof(double));
    q->next = NULL;
    q->t = t;
    q->n = n;
    memcpy(q->v, v, n * sizeof(double));
    *pp = q;
    npending++;
    uint64_t dt = (uint64_t)(t - now);
    vhpiTimeT at = { (uint32_t)(dt >> 32), (uint32_t)dt };
    vhpiCbDataT cb = { .reason = vhpiCbAfterDelay, .cb_rtn = after_cb, .time = &at };
    vhpi_register_cb(&cb, 0);
    return 0;
}

// The waveform copies at `now`: values whose time has come become current.
// Returns 1 if a driver changed.  *first := the earliest value still waiting.
static int advance_copies(int64_t now, int64_t *first)
{
    int drv = 0;
    *first = -1;
    if (!npending) return 0;
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self || !p->alive) continue;
        for (int k = 0; k < p->nlocal * MAX_WHAT; k++) {
            slot_t *s = &p->slots[k];
            while (s->pending && s->pending->t <= now) {
                pend_t *q = s->pending;
                s->pending = q->next;
                set_slot(s, q->v, q->n);
                drv |= (k % MAX_WHAT) == 0 || nets[fed_net].delayed;
                free(q);
                npending--;
            }
            if (s->pending && (*first < 0 || s->pending->t < *first)) *first = s->pending->t;
        }
    }
    return drv;
}

static msg_t *try_take(peer_t *p)
{
    pthread_mutex_lock(&mu);
    msg_t *m = p->head;
    if (m) {
        p->head = m->next;
        if (!p->head) p->tail = NULL;
    }
    pthread_mutex_unlock(&mu);
    return m;
}

static void wait_any(void)              // until some peer has sent something
{
    double t0 = wall();
    pthread_mutex_lock(&mu);
    for (;;) {
        int any = 0;
        for (int i = 0; i < npeers && !any; i++)
            if (!peers[i].self && peers[i].head) any = 1;
        if (any) break;
        pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
    t_blocked += wall() - t0;
    n_waits++;
}

static void send_status(int64_t now, int64_t next, int64_t quiet)
{
    static int64_t last_now = -2, last_next = -2, last_quiet = -2;
    static uint64_t *last = NULL;
    static int64_t *last_slack = NULL;
    const size_t sz = 28 + 24 * (size_t)npeers;
    uint8_t *b = malloc(sz);
    memcpy(b, &now, 8);
    memcpy(b + 8, &next, 8);
    memcpy(b + 16, &quiet, 8);
    uint32_t np = (uint32_t)npeers;
    memcpy(b + 24, &np, 4);
    for (int i = 0; i < npeers; i++) {
        memcpy(b + 28 + 24 * i, &peers[i].sent, 8);
        memcpy(b + 36 + 24 * i, &peers[i].recv, 8);
        memcpy(b + 44 + 24 * i, &peers[i].slack, 8);
    }
    if (!last) last = calloc(2 * (size_t)(npeers > 0 ? npeers : 1), sizeof(uint64_t));
    if (!last_slack) last_slack = calloc((size_t)(npeers > 0 ? npeers : 1), sizeof(int64_t));
    int same = now == last_now && next == last_next && quiet == last_quiet;
    for (int i = 0; i < npeers && same; i++)
        same = last[2 * i] == peers[i].sent && last[2 * i + 1] == peers[i].recv && last_slack[i] == peers[i].slack;
    if (!same) {
        for (int i = 0; i < npeers; i++) {
            peer_t *p = &peers[i];
            if (p->self || !p->alive) continue;
            if (send_msg2(p->sock, M_STATUS, b, (uint32_t)sz, NULL, 0)) p->alive = 0;
        }
        last_now = now;
        last_next = next;
        last_quiet = quiet;
        for (int i = 0; i < npeers; i++) {
            last[2 * i] = peers[i].sent;
            last[2 * i + 1] = peers[i].recv;
            last_slack[i] = peers[i].slack;
        }
        n_status++;
    }
    free(b);
}

static void take_status(peer_t *p, msg_t *m)
{
    uint32_t np;
    if (m->len < 28) die("short status from %s", p->name);
    memcpy(&p->st_now, m->body, 8);
    memcpy(&p->st_next, m->body + 8, 8);
    memcpy(&p->st_quiet, m->body + 16, 8);
    memcpy(&np, m->body + 24, 4);
    if ((int)np != npeers || m->len < 28 + 24 * np) die("bad status from %s", p->name);
    for (int i = 0; i < npeers; i++) {
        memcpy(&p->st_sent[i], m->body + 28 + 24 * i, 8);
        memcpy(&p->st_recv[i], m->body + 36 + 24 * i, 8);
        memcpy(&p->st_slack[i], m->body + 44 + 24 * i, 8);
    }
    p->have_status = 1;
}

// Every other simulation has completed its deltas, and every value sent has
// been received (by the counts each one gave, pair by pair): nothing can come
// before the earliest next event of all.  *T := that event (-1: none).
static int quiescent(int64_t mynext, int64_t *T)
{
    *T = mynext;
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self || !p->alive) continue;
        if (!p->have_status) return 0;
        if (p->st_recv[me] != p->sent || p->st_sent[me] != p->recv) return 0;
        for (int j = 0; j < npeers; j++) {
            peer_t *q = &peers[j];
            if (j == i || q->self || !q->alive) continue;
            if (!q->have_status || p->st_sent[j] != q->st_recv[i]) return 0;
        }
        if (p->st_next >= 0 && (*T < 0 || p->st_next < *T)) *T = p->st_next;
    }
    return 1;
}

// The earliest time a signal from any peer could still reach this simulation
// (-1: never), from the peers' last statuses and the slacks.  A peer p emits
// next no sooner than
//   - its next event, if everything sent to it has been received (the counts
//     in the statuses match) -- else no sooner than it takes those values in:
//     at its current time at the earliest, and for this simulation's values no
//     sooner than they reach it (emitted + the slack to p);
//   - and, since a third simulation r may have sent to p after r's last status,
//     no sooner than r's time then + r's slack to p;
// -- but not before the promise it reported (STATUS quiet: what it has
// scheduled ahead) -- and what it emits arrives here the slack to p later:
// everything p could send is known up to then.  *all := 0 while some peer
// has not reported yet.
static int64_t unacked_emission(peer_t *p, uint64_t acked)   // of value number `acked` (0-based)
{
    if (acked > p->ebase) {                             // forget those it has taken in
        uint64_t k = acked - p->ebase;
        if (k > (uint64_t)p->elen) k = p->elen;
        memmove(p->etime, p->etime + k, (p->elen - k) * sizeof(int64_t));
        p->elen -= (int)k;
        p->ebase += k;
    }
    if (!p->elen) return now_fs();
    return p->etime[0];
}

static int64_t emit_bound(int *all)
{
    int64_t B = -1;
    *all = 1;
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self || !p->alive) continue;
        if (!p->have_status) {
            *all = 0;
            return now_fs();
        }
    }
    for (int i = 0; i < npeers; i++) {
        peer_t *p = &peers[i];
        if (p->self || !p->alive) continue;
        int64_t lb = p->st_next;                        // -1: none
#define LOWER(x) do { int64_t x_ = (x); if (lb < 0 || x_ < lb) lb = x_; } while (0)
        if (p->sent != p->st_recv[me]) {                // ours not all taken in yet
            int64_t e = unacked_emission(p, p->st_recv[me]);
            LOWER(e + p->slack > p->st_now ? e + p->slack : p->st_now);
        }
        for (int j = 0; j < npeers; j++) {
            peer_t *r = &peers[j];
            if (j == i || r->self || !r->alive) continue;
            if (r->st_sent[i] != p->st_recv[j]) LOWER(p->st_now);
            LOWER(r->st_now + r->st_slack[i]);
        }
#undef LOWER
        // what p has scheduled ahead: none of its ends changes before then,
        // whatever it does meanwhile
        if (lb >= 0 && p->st_quiet > lb) lb = p->st_quiet;
        if (lb >= 0 && (B < 0 || lb + p->slack < B)) B = lb + p->slack;
    }
    return B;
}

static void kick_resolvers(void)
{
    net_t *n = &nets[fed_net];
    if (!n->kick) {
        n->kick = find_kick(n->kick_name);
        if (!n->kick) die("no kick signal %s", n->kick_name);
    }
    vhpiValueT v = { .format = vhpiIntVal };
    v.value.intg = ++n->kick_count;
    if (vhpi_put_value(n->kick, &v, vhpiDepositPropagate) != 0) die("cannot deposit %s", n->kick_name);
    n_delivers++;
}

// The end of the deltas at `now`.  Take in what has arrived; if a remote driver
// for now came, run the resolvers again (more deltas, then back here).  Else
// this simulation's deltas are complete: say so, and go on to the next event
// when the federation allows it, waiting for messages until it does.
static void lkd_cb(const vhpiCbDataT *d)
{
    (void)d;
    if (finished || fed_net < 0) return;
    const int64_t now = (int64_t)now_fs();
    nvc_vhpi_sched_flush();                             // what this time's deltas scheduled ahead
    for (;;) {
        int drv = 0, changed = 0;
        for (int i = 0; i < npeers; i++) {
            peer_t *p = &peers[i];
            if (p->self || !p->alive) continue;
            msg_t *m;
            while (p->alive && (m = try_take(p))) {
                if (m->type == M_VALUE) {
                    p->recv++;
                    drv |= stage(p, m, now);
                } else if (m->type == M_STATUS) {
                    take_status(p, m);
                } else if (m->type == M_GONE) {
                    p->alive = 0;
                    changed = 1;
                    if (trace) fprintf(stderr, "arena: %s left arena %s\n", p->name, arena_name);
                } else {
                    die("unexpected message from %s", p->name);
                }
                free(m);
            }
        }
        int64_t first;
        drv |= advance_copies(now, &first);
        if (changed) {                                  // someone left: the indices close up
            index_arena();
            drv = 1;
        }
        if (drv) {
            kick_resolvers();
            return;                                     // the deltas run; this comes back at their end
        }
        int64_t mynext = next_fs();
        if (first >= 0 && (mynext < 0 || first < mynext)) mynext = first;
        // Each simulation reports only its own time, next event, counts and
        // slacks.  A signal from a peer is heard here no sooner than the
        // slack after it is emitted, so no driver update needs considering in
        // the cycle at which its sender said where it is: this simulation goes
        // on to its next event when that is before the earliest time anything
        // from any peer could still arrive (emit_bound) -- or, when everyone's
        // deltas are complete with nothing in flight, when it is the earliest
        // next event of all (needed where the slack is 0: zero delay).
        send_status(now, mynext, quiet_until(now));
        int all = 1;
        const int64_t bound = emit_bound(&all);
        if (all && mynext >= 0 && (bound < 0 || mynext < bound)) {
            if (trace) fprintf(stderr, "arena: %lld fs done, next %lld (bound %lld)\n", (long long)now,
                               (long long)mynext, (long long)bound);
            n_advances++;
            return;
        }
        int64_t T;
        if (quiescent(mynext, &T)) {
            if (T < 0) {                                // nobody has anything scheduled
                finished = 1;
                if (trace) fprintf(stderr, "arena: finish at %lld fs\n", (long long)now);
                return;
            }
            if (mynext == T) {                          // this one is among the first: go on
                if (trace) fprintf(stderr, "arena: %lld fs done, next %lld\n", (long long)now, (long long)T);
                n_advances++;
                return;
            }
        }
        wait_any();
    }
}

static void eos_cb(const vhpiCbDataT *d)
{
    (void)d;
    if (fed_net < 0) return;
    uint64_t bin = 0, bout = 0;
    for (int i = 0; i < npeers; i++)
        if (!peers[i].self) {
            shutdown(peers[i].sock, SHUT_WR);
            bin += peers[i].bytes_in;
            bout += peers[i].bytes_out;
        }
    fprintf(stderr, "libarena: %s in arena %s: %llu time steps, %llu deliveries, %llu values sent (%d as scheduled), "
            "%llu statuses, %llu waits (%.3f s), %.1f MB out / %.1f MB in\n",
            fed_name, arena_name, (unsigned long long)n_advances, (unsigned long long)n_delivers,
            (unsigned long long)n_puts, n_scheduled, (unsigned long long)n_status, (unsigned long long)n_waits, t_blocked,
            bout / 1e6, bin / 1e6);
}

static void eoi_cb(const vhpiCbDataT *d)
{
    (void)d;
    vhpiCbDataT cb = { .reason = vhpiCbRepLastKnownDeltaCycle, .cb_rtn = lkd_cb };
    vhpi_register_cb(&cb, 0);
    vhpiCbDataT e = { .reason = vhpiCbEndOfSimulation, .cb_rtn = eos_cb };
    vhpi_register_cb(&e, 0);
}

static void parse_arenas(const char *s)
{
    char *dup = strdup(s), *save = NULL;
    for (char *item = strtok_r(dup, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        while (*item == ' ') item++;
        if (!*item) continue;
        char *eq = strchr(item, '=');
        if (!eq || nmaps >= MAX_ARENAS) die("FED_ARENAS: want band=arena[@host:port],...: %s", item);
        *eq = 0;
        map_t *m = &maps[nmaps++];
        snprintf(m->band, sizeof m->band, "%s", item);
        char *at = strchr(eq + 1, '@');
        if (at) *at = 0;
        snprintf(m->arena, sizeof m->arena, "%s", eq + 1);
        snprintf(m->rv, sizeof m->rv, "%s", at ? at + 1 : m->arena);
    }
    free(dup);
}

static void startup(void)
{
    trace = getenv("FED_TRACE") && *getenv("FED_TRACE") == '1';
    fed_name = getenv("FED_NAME");
    if (fed_name && !*fed_name) fed_name = NULL;
    const char *a = getenv("FED_ARENAS");
    if (a && *a) parse_arenas(a);
    if (nmaps && !fed_name) die("FED_ARENAS is set but FED_NAME is not%s", "");
    vhpiCbDataT cb = { .reason = vhpiCbEndOfInitialization, .cb_rtn = eoi_cb };
    vhpi_register_cb(&cb, 0);
}

void (*vhpi_startup_routines[])(void) = { startup, NULL };
