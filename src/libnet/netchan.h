/* netchan.h : multiplexed UDP channels for game networking */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NETCHAN_H
#define NETCHAN_H

#include <stdint.h>
#include <stddef.h>
#include "nc_addr.h"

/****************************************************************
 * Constants
 ****************************************************************/

enum {
    NETCHAN_RELIABLE,
    NETCHAN_UNRELIABLE,
    NETCHAN_STREAM,
};

enum {
    NETCHAN_DIR_SEND,
    NETCHAN_DIR_RECV,
};

enum {
    NETCHAN_STATE_NEW,
    NETCHAN_STATE_CONNECTING,
    NETCHAN_STATE_CONNECTED,
    NETCHAN_STATE_CLOSING,
    NETCHAN_STATE_CLOSED,
};

enum {
    NETCHAN_EV_NONE,
    NETCHAN_EV_CONNECTED,
    NETCHAN_EV_DISCONNECTED,
    NETCHAN_EV_REDIRECT,
    NETCHAN_EV_CHAN_OPEN,
    NETCHAN_EV_CHAN_CLOSE,
    NETCHAN_EV_DATA,
};

enum {
    NETCHAN_OK       =  0,
    NETCHAN_ERR      = -1,
    NETCHAN_ERR_NOMEM = -2,
    NETCHAN_ERR_AGAIN = -3,
    NETCHAN_ERR_CLOSED = -4,
    NETCHAN_ERR_FLOW  = -5,
    NETCHAN_ERR_PROTO = -6,
    NETCHAN_ERR_TOOBIG = -7,
};

/****************************************************************
 * Types
 ****************************************************************/

struct netchan_conn;
struct netchan_chan;

struct netchan_cfg {
    unsigned mtu;
    unsigned chan_window;
    unsigned idle_timeout_ms;
    unsigned retransmit_ms;
};

struct netchan_event {
    int type;
    struct netchan_chan *ch;
    struct nc_addr redirect_addr;
    uint32_t redirect_conn_id;
};

struct netchan_conn_stats {
    uint32_t rtt_ms;
    uint32_t rtt_min_ms;
    uint32_t pkts_sent;
    uint32_t pkts_recv;
};

struct netchan_chan_stats {
    uint32_t msgs_sent;
    uint32_t msgs_acked;
    uint32_t retransmissions;
    uint32_t msgs_recv;
};

/****************************************************************
 * Configuration
 ****************************************************************/

void netchan_cfg_default(struct netchan_cfg *cfg);

/****************************************************************
 * Connection lifecycle
 ****************************************************************/

struct netchan_conn *netchan_open(int server);
void netchan_close(struct netchan_conn *c);
void netchan_config(struct netchan_conn *c, const struct netchan_cfg *cfg);
int netchan_state(struct netchan_conn *c);
uint32_t netchan_id(struct netchan_conn *c);

int netchan_connect(struct netchan_conn *c, const struct nc_addr *addr);
int netchan_accept(struct netchan_conn *c);

/****************************************************************
 * Packet I/O -- application owns the UDP socket
 ****************************************************************/

/** Extract connection ID from a raw packet without full parsing. */
uint32_t netchan_peek_id(const void *pkt, size_t len);

/** Feed a received datagram into the connection. `from` is the transport
 *  address it arrived from (NULL if the transport has no source address). */
int netchan_feed(struct netchan_conn *c, const void *pkt, size_t len,
                 const struct nc_addr *from);

/** Get next outgoing packet. Returns byte count, 0 when nothing pending.
 *  `to` receives the peer's transport address. */
size_t netchan_send_next(struct netchan_conn *c, void *buf, size_t buflen,
                         struct nc_addr *to);

/** Service timers. Returns ms until next needed call, or -1 if idle. */
int netchan_service(struct netchan_conn *c, uint32_t now_ms);

/****************************************************************
 * Channel API
 ****************************************************************/

struct netchan_chan *netchan_chan_open(struct netchan_conn *c,
                                     int type, int dir,
                                     const char *content_type);
void netchan_chan_close(struct netchan_chan *ch);
int netchan_chan_id(struct netchan_chan *ch);
int netchan_chan_type(struct netchan_chan *ch);
int netchan_chan_state(struct netchan_chan *ch);

/** Queue a datagram or stream bytes for sending. Returns bytes queued. */
int netchan_chan_write(struct netchan_chan *ch, const void *data, size_t len);

/** Read next datagram or stream bytes. Returns bytes read, 0 if empty. */
int netchan_chan_read(struct netchan_chan *ch, void *buf, size_t buflen);

/****************************************************************
 * Events -- call after netchan_feed to drain the event queue
 ****************************************************************/

int netchan_poll(struct netchan_conn *c, struct netchan_event *ev);

/****************************************************************
 * Statistics -- for congestion detection and diagnostics
 ****************************************************************/

void netchan_conn_stats(struct netchan_conn *c, struct netchan_conn_stats *s);
void netchan_chan_stats(struct netchan_chan *ch, struct netchan_chan_stats *s);

#endif /* NETCHAN_H */
