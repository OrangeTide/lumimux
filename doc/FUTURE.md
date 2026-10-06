# Future Work

This file tracks planned and in-progress feature work. Phase 11 grouped four
advanced features; all four have shipped. Work scoped since then is recorded
in its own section below.

## Phase 11: Advanced Features

| Sub-phase | Feature                    | Status                       |
|-----------|----------------------------|------------------------------|
| 11A       | State-dependent bindings   | Done (commit 763af21)        |
| 11B       | SIXEL/DCS pass-through     | Done (commit 763af21)        |
| 11D       | Speculative local echo     | Done (commit 763af21)        |
| 11C       | Networked connections      | Landed (see below)           |

The plans below are retained for reference. 11A/11B/11D describe what was
built. 11C has now shipped in full: the encrypted netchan net-proxy, the
`attach -n` local and cross-host paths, roaming, and hands-free roaming
(automatic network-change detection in the attach client). The section
below records the details.

---

## Background Color Erase (BCE) (DONE)

**Status:** Fixed. Regression test `test_state_bce` added to
`src/libvt/test_vt.c`.

**Symptom:** `dialog(1)` (for example `dialog --dselect /tmp 20 60`) and
other curses apps that paint solid colored panels rendered with the panel
interiors showing the terminal default background instead of the widget
color. Only the cells that held text carried color; the padding did not.

**Cause:** The VT erase operations cleared cells to hard defaults rather
than to the current SGR background color. Curses libraries set a background
color and then use ECH (`CSI n X`), EL (`CSI K`), ED (`CSI J`), and scroll
fills to paint a region. Without background color erase those cleared cells
reverted to the default background.

**Fix:** Added `vt_cell_erase(cell, bg)` and threaded the current
background (`st->bg`) through every erase and scroll path. `vt_buf_scroll`
and `vt_buf_clear_rows` take a `bg` argument; ECH, EL, ED, ICH, DCH, IL,
DL, SU, SD, LF/IND/RI scrolling, and alt-screen clear all fill with the
current background.

| File | Change |
|------|--------|
| `src/libvt/vt_cell.c/.h` | `vt_cell_erase(cell, bg)` |
| `src/libvt/vt_buf.c/.h` | `vt_buf_scroll`/`vt_buf_clear_rows` take a `bg`, fill with it |
| `src/libvt/vt_ops.c` | erase/insert/delete/scroll ops pass `st->bg` |
| `src/libvt/vt_state.c` | index/reverse-index scroll and alt-screen clear pass `st->bg` |

---

## GPM Mouse on the Linux Console (DONE)

**Status:** Landed as an optional, auto-detected build feature.

**Goal:** Mouse support on the raw Linux text console, where the terminal
reports no xterm-style mouse sequences. There the gpm daemon is the source
of mouse events.

**Design:** A small module `src/cmd/attach/gpm_mouse.c` connects to gpm
(only when `TERM=linux`), registers the gpm socket on the event loop, and
translates `Gpm_Event` records into `struct tkbd_seq` mouse events fed
through the same `dispatch_input` path as terminal mouse sequences. Built
without gpm it compiles to no-op stubs, so the call sites in `attach.c` are
unconditional.

**Build:** libgpm is auto-detected with a compile-and-link probe in
`src/module.mk`, the way an autoconf script would. `make GPM=1` forces it
on, `make GPM=0` off. Requires the libgpm development headers
(`libgpm-dev` on Debian and Ubuntu).

| File | Change |
|------|--------|
| `src/cmd/attach/gpm_mouse.c/.h` | gpm connection, event translation, stubs |
| `src/cmd/attach/attach.c` | init after stdin watcher, shutdown before loop free |
| `src/module.mk` | auto-detect probe, `-DHAVE_GPM` and `-lgpm` when present |

---

## 11A: State-Dependent Key Bindings (DONE)

**Goal:** Bindings that activate conditionally based on window title regex
or toggle state.

### Design

Binding layers replace the single flat `bindings[256]` array. Each layer has:
- A name (e.g. `"default"`, `"vi"`, `"bash"`)
- An optional match predicate: regex on window title, or a named toggle state
- A `bindings[256]` array (sparse -- KEYS_ACTION_NONE means "fall through")

Lookup walks layers top-to-bottom, first non-NONE match wins.

```c
struct keys_layer {
    char            name[32];
    regex_t         *title_re;     /* NULL = always active */
    char            toggle[32];    /* "" = no toggle, else named state */
    int             toggle_active; /* only meaningful if toggle[0] != 0 */
    enum keys_action bindings[256];
};
```

**API additions:**
- `keys_set_title(k, title)` -- attach client calls on focus change / title update
- `keys_toggle(k, name)` -- flip a named toggle (bound to KEYS_ACTION_TOGGLE)
- `keys_feed()` signature unchanged -- reads context internally

**Config format:**
```ini
[bind]
c = new-window          # default layer, always active

[bind "vi"]
match-title = ^vi.*
j = scroll-down         # active only when title matches ^vi

[bind "logging"]
toggle = logging        # active only when "logging" toggle is on
l = toggle              # pressing 'l' flips the toggle off
```

### Files to Change

| File | Change |
|------|--------|
| `src/libkeys/keys.h` | Add `keys_layer`, update `struct keys`, new API |
| `src/libkeys/keys.c` | Layer lookup in `keys_feed()`, layer management, regex |
| `src/cmd/attach/attach.c` | Call `keys_set_title()` on focus change / title update |
| `tests/test_keys.c` | Tests for layered lookup, title matching, toggle, fall-through |

---

## 11B: SIXEL Pass-Through (DONE)

**Goal:** Forward SIXEL (and other DCS) sequences from child PTY through
to the outer terminal without interpreting them.

### Design

Add `dcs` callback to `struct vt_ops`. Accumulate DCS bytes in a dynamic
buffer during `ST_DCS_PASSTHRU`, emit via callback on ST.

```c
void (*dcs)(void *ctx, const char *data, size_t len);
```

Client-side `op_dcs_passthru` writes raw `ESC P ... ESC \` directly to
stdout, bypassing the cell grid. Only active when source pane is fullscreen
(single pane, no splits). Suppressed in split/turbo modes.

DCS buffer is dynamically allocated (realloc doubling), capped at 16 MB.

### Files to Change

| File | Change |
|------|--------|
| `src/libvt/vt_parse.h` | Add `dcs` callback to `struct vt_ops` |
| `src/libvt/vt_parse.c` | DCS buffer accumulation, `emit_dcs()` helper |
| `src/libvt/vt_ops.c` | Add `op_dcs` to default vtable (no-op server-side) |
| `src/cmd/attach/attach.c` | Implement `op_dcs_passthru`, gate on fullscreen |
| `tests/test_vt_parse.c` | DCS accumulation, ST termination, max-size cap |

---

## 11C: Networked Connections (LANDED)

**Goal:** Attach to sessions over the network, with the existing TLV message
protocol unchanged. Gains encrypted roaming (a session survives an IP or
network change), and opens a path to a browser client later.

**Transport choice: [netchan-v2][1], not QUIC.** The earlier plan wrapped
[ngtcp2][3] + [picotls][4] for QUIC. That is dropped in favor of netchan-v2,
a reliable-UDP multiplexed channel protocol (~1500 lines of plain C) that
already shares lumi's toolchain: the same `microser` IDL generator
(`src/libipc/lumi.idl`, `microser.h`, `gen.sh`) and the same modular-make
build. Rationale:

- **Dependency weight.** netchan core is ~1500 lines of C, `nc_udp` ~60, and
  the crypto backend ~200 lines over vendored [Monocypher][2] (single-file,
  CC0). All compiles with `cc`, no external package. QUIC would vendor ngtcp2
  and picotls (tens of thousands of lines of TLS/QUIC state machine, a CVE
  surface to track). netchan matches the "maintain only what we can" rule.
- **The 11C features are already built.** Connection migration/roaming (an
  `nc_addr` compare when a peer's address changes), PSK-mixed encryption, and
  a replay window exist and run clean under ASan/UBSan.
- **Crypto matches the threat model.** A "reattach to my own remote session"
  tool wants WireGuard/Noise + optional PSK (X25519 + XChaCha20-Poly1305),
  which netchan provides, not QUIC's TLS 1.3 + PKI/cert chains.
- **Multiple channels natively** (reliable + unreliable per connection) map
  onto lumi's control-vs-output streams.
- **Multi-backend, including the browser.** UDP, encrypted UDP, WebSocket, and
  WebRTC backends exist, and the core compiles to wasm. This makes a browser
  lumi client tractable; raw QUIC from wasm is not.

Trade-offs accepted: netchan is currently demo/research code (extraction and
sole maintenance are ours), it has far less hostile-internet hardening than
ngtcp2/TLS 1.3, and it offers no 0-RTT or standardized handshake. QUIC stays a
possible *additional* backend behind the same seam if internet-grade hardening
or standard interop later becomes a hard requirement.

### Design

**Transport abstraction layer (prerequisite, needed for any backend):**

lumi's IPC is currently welded to a raw fd (`ipc_msg_send(int fd, ...)` over a
Unix `SOCK_STREAM`). Introduce a vtable so the network-facing endpoints (the
attach client's mserver connections and the proxy's mserver connections) send
through an abstract transport instead. mserver itself stays on raw `ipc_msg`:
it is always the local Unix endpoint, reached remotely through the
`lumi-net-proxy` bridge (see step 1 below for the full boundary).

```c
struct ipc_transport {
    int  (*send)(void *ctx, uint32_t type, const void *payload, uint32_t len);
    int  (*recv)(void *ctx, uint32_t *type, void *buf, size_t bufsz,
                 uint32_t *len);
    int  (*get_fd)(void *ctx);   /* pollable fd for iox; -1 if N/A */
    void (*close)(void *ctx);
    void *ctx;
};
```

Two implementations: the existing Unix-socket path (behavior-preserving), and
a netchan path. The attach client changes from `ipc_msg_send(fd, ...)` to
`transport->send(transport->ctx, ...)`. TLV framing and the microser payloads
are unchanged; the netchan transport carries the same bytes on a reliable
channel.

`get_fd` is on the vtable because the whole system is fd- and event-loop
coupled: every connection is registered with `iox_fd_add(fd, IOX_READ, cb)`
and `recv` runs only after poll signals readability, and mserver's output path
does not call `ipc_msg_send` at all (it frames with `ipc_msg_frame` into its
own `outq` and flushes with non-blocking `send(MSG_DONTWAIT)` under
backpressure). Exposing the pollable fd keeps the event loop and that async
queue working unchanged during the port. A netchan impl returns its UDP socket
fd, or -1 with a different readiness hook wired in a later step. Abstracting
the readiness model itself is out of scope for step 1 (it would not be
behavior-preserving).

**New binary: `lumi-net-proxy`** runs on the remote host (one per session):
- Listens on a netchan/UDP endpoint (host:port), encrypted backend by default
- Authenticates via pre-shared key (PSK) or SSH key challenge
- Bridges TLV messages between the netchan connection and the local mserver
  Unix sockets (it is the netchan peer on one side, an ordinary Unix-socket
  IPC client on the other)
- Registers a `net-addr` file in sessdir so `lumi attach` can discover the
  endpoint

**Channel mapping:** open one reliable channel for control and window
output (terminal streams must be byte-exact and in order); an unreliable
channel may later carry loss-tolerant status. Netchan support is optional
(`#ifdef HAVE_NETCHAN`).

### New Files

| File | Purpose |
|------|---------|
| `src/libipc/ipc_transport.{h,c}` | Transport abstraction + Unix-socket impl |
| `src/libipc/test_ipc_transport.c` | Round-trip test of the Unix-socket impl |
| `src/libnet/` | netchan core + `nc_addr` + `nc_udp` + `nc_crypto`, extracted from the netchan-v2 demo and stripped of its game/gateways; Monocypher vendored |
| `src/cmd/net-proxy/` | New `lumi-net-proxy` binary |

### Implementation Order

1. Cut the `ipc_transport` seam; port existing Unix-socket IPC onto it with no
   behavior change (keeps all current tests green). Concretely:
   - Add `src/libipc/ipc_transport.{h,c}`: the vtable, plus
     `ipc_transport_unix_new(int fd)` / `ipc_transport_free()` whose methods
     wrap `ipc_msg_send`/`ipc_msg_recv`/`ipc_close` and return the stored fd
     from `get_fd`. Add `test_ipc_transport.c` (round-trip over a
     `socketpair`).
   - Attach client: `struct mconn` gains an `ipc_transport *`. The central
     send helper `mconn_ipc_send`, the recv drain in `on_mserver_read`, and the
     attach handshake go through it. The apps (`app_calc`/`app_dict`/
     `app_emoji`) send input through the focused mconn's transport instead of a
     bare `input_fd`.
   - Proxy: a transport per mserver-facing connection (`struct pconn`). The
     client-facing side is stdin/stdout with `proxy_msg` framing (the network
     boundary, replaced by netchan later), not `ipc_msg`, so it is untouched
     here.
   - Left on the raw fd deliberately:
     - `mserver`: always the *local Unix endpoint*. A remote client reaches it
       through the future `lumi-net-proxy`, which speaks netchan on the wire
       and ordinary Unix `ipc_msg` to mserver, so mserver never needs a
       non-Unix transport. It also does not fit the message-level vtable
       cleanly: its bulk output is a byte-level non-blocking `send(client_fd)`
       queue, and `attr_store_txn_rollback_client` takes a raw fd. Wrapping
       only the control recv/`IPC_MSG_OK` path would leave two representations
       of one connection, so mserver keeps raw `ipc_msg`.
     - `lumi kill`, `lumi detach`, `lumi attr` (`ipc_attr`): one-shot,
       local-only admin commands that never traverse the network. They can
       adopt the transport later if remote admin is ever wanted.
2. Extract `src/libnet/` from `~/research/netchan-v2/demo` (netchan core,
   `nc_addr`, `nc_udp`, `nc_crypto`, Monocypher) into a lumi module.mk.
3. Stress-test the reliable channel under simulated loss/reorder to confirm
   byte-exact, in-order delivery for the output stream before relying on it.
4. Build `lumi-net-proxy` and the netchan `ipc_transport` impl; register and
   discover `net-addr` in sessdir. Split into three commits:
   - 4a: the netchan `ipc_transport` impl (`src/libnet/ipc_transport_netchan.{c,h}`).
     It owns a UDP socket, runs netchan's feed/service/send_next/poll cycle,
     and layers `ipc_msg` framing over one reliable channel (chunked to fit
     netchan's per-message limit, reassembled on receive) so callers see the
     same message boundaries as over a stream fd. `send`/`recv` block like
     `ipc_msg`; `get_fd` exposes the UDP socket. A two-process loopback test
     (`test_ipc_transport_net.c`) round-trips TLV up to ~60 KB. Landed.
   - 4b: the non-blocking event-loop drain on the netchan transport
     (`ipc_transport_netchan_pump` / `_try_recv` / `_timeout`). The blocking
     send/recv vtable suffices for handshakes and simple clients, but a
     server multiplexing many fds must not block in recv; these let a
     poll/iox loop service the link. Verified by an event-loop-style variant
     of the loopback test. Landed.
   - 4c: the `lumi net-proxy` subcommand, bridging netchan <-> local mserver
     Unix sockets. It binds a UDP endpoint, publishes it as `net-addr` in the
     session dir (`sessdir_{write,read}_session_file`), accepts one client,
     and multiplexes every window over one reliable channel using the
     window_id envelope helpers `proxy_msg_xsend` / `_xdecode`. The client
     link is serviced with the 4b non-blocking drain in a hand-rolled poll
     loop. Verified end to end by `test_net_proxy`: the real
     `cmd_net_proxy_main` bridges a netchan client to a fake mserver, and
     input round-trips through client -> proxy -> mserver -> proxy -> client.
     Landed.
   - 4d: attach-side discovery of `net-addr` and connect via the netchan
     transport, wiring the drain into attach's iox loop. `lumi attach -n`
     reads the proxy's `net-addr` from the session dir, dials it on
     loopback, and runs the proxy envelope over the netchan
     `ipc_transport`. The link is serviced both on socket readability and
     on a re-armed one-shot iox timer, so retransmits fire while idle.
     Proxy dispatch and PROXY_READY parsing are shared with the ssh-pipe
     path. Verified end to end: `attach -n` renders a bridged session and
     `echo` input round-trips client -> netchan -> net-proxy -> mserver
     and back. Landed. Cross-host endpoint discovery over ssh is step 5.
5. Wire encryption (PSK / SSH-key challenge) and roaming; document in lumi.1.
   Split into sub-steps:
   - 5a: encrypt the netchan link. `ipc_transport_netchan_new_crypto` wraps
     every datagram in the nc_crypto AEAD layer (X25519 ephemeral handshake,
     XChaCha20-Poly1305 per packet, sliding replay window). The crypto
     handshake runs inside `_establish` before netchan's, so the connect SYN
     is already sealed; the initiator repeats its HELLO until ready and the
     responder answers each HELLO until the peer sends sealed DATA. net-proxy
     mints a random 32-byte PSK at startup, publishes it as `net-key` (0600)
     in the session dir alongside `net-addr`, and removes both on exit;
     `attach -n` reads and decodes the key and both ends mutually
     authenticate against it. Verified: encrypted TLV round-trips (blocking
     and event-loop drain, valgrind-clean) in `test_ipc_transport_net`, the
     `test_net_proxy` bridge runs encrypted end to end, and real `attach -n`
     renders a session and round-trips input over the encrypted link. Landed.
   - 5b: cross-host discovery over ssh. `net-proxy -d` prints one line
     "udp <port> <hexkey>" on stdout then daemonizes (fork, setsid, std fds
     to /dev/null) and keeps serving. `attach -n host:session` runs
     `ssh host lumi net-proxy -s session -b 0.0.0.0 -d`, reads that line back
     through the ssh channel (which authenticates the user and encrypts the
     key transfer), resolves `host`, and dials the endpoint directly, so
     window I/O rides the encrypted netchan link rather than ssh. A local
     `-n name` still attaches to a proxy already running on this host over
     loopback. Verified: `net-proxy -d` reports a well-formed line and the
     detached proxy keeps serving, and `attach -n host:session` (driven
     through a stand-in ssh) bootstraps, dials, renders a session, and
     round-trips input over the encrypted link. Landed. Known limits: each
     `-n` attach bootstraps its own proxy (the one-client design), and an
     SSH-key challenge in place of the PSK file is not yet implemented.
   - 5c: roaming. netchan already migrates a connection when a validated
     packet arrives from a new `nc_addr` (netchan.c updates `peer_addr` after
     `validate_migration` confirms the packet references a live channel), and
     the nc_crypto layer is address-independent (directional keys, counter
     nonce, replay window), so a moved client keeps decrypting with no
     re-handshake. The transport exposes `ipc_transport_netchan_rebind`, which
     swaps the underlying UDP socket while keeping the netchan and crypto
     session intact, for a client whose local socket broke on a network
     change. Verified: `test_ipc_transport_net` establishes an encrypted
     session, exchanges a message, rebinds the client to a fresh source port,
     and the next message still round-trips (20/20 deterministic, valgrind
     clean). Landed.
   - 5c follow-up (hands-free roaming): landed. The netchan transport flags
     `roam_pending` when a client send fails with a dead-local-address errno
     (EADDRNOTAVAIL / ENETUNREACH / ENETDOWN / EHOSTUNREACH), and
     `ipc_transport_netchan_roam` opens a fresh socket in the peer's family
     and rebinds to it. Detection needs no netlink: netchan pings every ~5s,
     so an idle client's next send trips the error within seconds of a
     network change. The attach client polls `roam_pending` on its service
     tick (rate-limited to 1/s), self-roams, re-registers the changed fd with
     the iox loop, and sends a `window_id 0` NOP (dropped by the proxy) so the
     new source produces real channel DATA. That last step matters: netchan's
     migration only accepts a new address on a validated DATA/ACK frame, so a
     bare keepalive ping would not migrate an idle session. The self-roam API
     is covered by `test_ipc_transport_net`; the send-error trigger is not
     loopback-observable, so the attach glue is verified by inspection and a
     no-regression run of `attach -n`.
   - 5d: server identity (ssh host-key TOFU). The PSK already gives mutual
     authentication, but it rotates every startup, so a client cannot pin a
     server across restarts. netchan's `nc_crypto` carries an optional
     long-term X25519 identity key: the server presents its public half in
     the HELLO, a second Diffie-Hellman folds it into the key derivation, and
     the first sealed packet that opens is proof of possession (Noise NX).
     lumi wires this as opt-in server auth, layered on the PSK, not replacing
     it:
       - Files live under `~/.config/lumi/` (honoring `XDG_CONFIG_HOME`):
         the server's `host_key` (its X25519 secret, mode 0600, generated on
         first use) and the client's `known_hosts` (host -> pinned public
         key). Both formats come from netchan's vendored `keystore` (only
         `ks_host_key` and `ks_known_host`/`ks_known_host_add` are used;
         `nc_auth` userauth is not adopted).
       - net-proxy gains `-k`: it loads or generates its host key, presents
         it via `ipc_netchan_auth.static_sk`, appends the public key (hex) as
         a third field on the `-d` report line ("udp <port> <psk> <hostkey>"),
         and writes it to a `net-hostkey` session file for the local path.
       - attach gains `-V` (verify host): for a remote target it adds `-k` to
         the ssh net-proxy invocation and reads the third report field; for a
         local target it reads `net-hostkey`. It installs a `verify_peer`
         callback that consults `known_hosts` via `ks_known_host`. MATCH is
         accepted; CHANGED is refused loudly inside the handshake; UNKNOWN is
         accepted tentatively and confirmed by an interactive fingerprint
         prompt *after* `_establish` returns, so a slow human cannot trip the
         5s handshake timeout. Declining tears the session down before any
         data flows. On acceptance the key is recorded with
         `ks_known_host_add`. The prompt runs pre-TUI while the terminal is
         still cooked.
     Without `-k`/`-V` the flow is byte-identical to 5b. Forward secrecy is
     unchanged: the ephemeral-ephemeral secret stays in the transcript, so a
     later host-key theft permits impersonation but does not decrypt recorded
     sessions.
   - 5e: user authentication (nc_auth), gated mechanism. netchan's
     `nc_auth` (vendored) is an ssh-shaped userauth conversation
     (publickey/password) carried as reliable messages over the encrypted
     channel, authenticating the connecting *user* rather than the
     connection. In lumi's current flow the user is already authenticated by
     ssh (cross-host) or filesystem access (local), so this is redundant
     until a direct-connect deployment (dialing a listening net-proxy with no
     ssh) is built. Landed so far, gated and off by default: the vendored
     `nc_auth`; `lumi net-keygen`, which writes an Ed25519 client identity
     (`~/.config/lumi/id_netchan`, Argon2id-sealable) and prints the
     `authorized_keys` line; and an opt-in auth phase in the netchan
     transport. When `ipc_netchan_auth.userauth` is non-NULL,
     `_establish` runs the conversation over the reliable channel after the
     crypto handshake and before any TLV, framing each nc_auth message with a
     2-byte length; a denied login fails the establish. The server supplies
     policy callbacks (backed by `authorized_keys`/`passwd` when wired), the
     client supplies `user` plus credential fetchers that should hand over a
     preloaded key or already-collected password so the establish deadline
     cannot race a human. With `userauth` NULL the transport is byte-identical
     to 5d. Verified: `test_nc_auth` covers the state machines (pubkey,
     password fallback, denial, session-id binding) and
     `test_ipc_transport_net` drives the phase over a real loopback link
     (pubkey login + TLV, password fallback, refusal), valgrind-clean.
   - 5f: direct-connect deployment. `net-proxy -L -p <port>` listens
     persistently and serves clients one at a time (rebinding the fixed port
     between them, reusing the single-client bridge; no two simultaneous
     attaches, the documented one-client limit). It presents a host key and
     requires userauth backed by `~/.config/lumi/authorized_keys` (via
     `ks_authorized_key`) and `passwd` (via `ks_check_password`); the same
     methods are offered for every name so the handshake cannot enumerate
     accounts. There is no PSK: the endpoint is public and every client
     authenticates. `attach lumi://[user@]host:port/session` dials it
     directly with no ssh, pins the host key in `known_hosts` on first contact
     (the `-V` fingerprint prompt), unlocks `~/.config/lumi/id_netchan` (with
     a passphrase prompt up front so the handshake deadline cannot race the
     human), and logs in by public key or password. `lumi net-keygen` writes
     the client key and prints its `authorized_keys` line. Verified end to end
     through the screen harness: host-key TOFU, pubkey login, keystroke
     forwarding, and live render all work over the direct link; the full test
     suite stays green. This is what makes the userauth of 5e non-redundant
     with ssh.
     Automated coverage: `test_net_proxy` drives the real `-L` listener with
     a fake mserver and a forked proxy: an authorized key logs in and
     round-trips, the listener serves a second client (proving the
     rebind-and-loop persistence), and an unenrolled key is refused.
     Building it surfaced and fixed two real listener bugs. First, a client's
     graceful close queued a netchan DISCONNECT but never flushed it before
     dropping the socket, so a persistent server only learned a client left
     via the keepalive timeout and stalled the next client for seconds; the
     transport now transmits the DISCONNECT (new upstream `netchan_disconnect`,
     flushed in `nct_close`). Second, `udp_bind` lacked `SO_REUSEADDR`, so the
     immediate rebind of the fixed port could fail. Known limitation: a client
     that connects immediately after a *rejected* one can fail to establish,
     because the rejected client's stray DISCONNECT reaches the freshly
     rebound socket; a legitimate client's retry recovers. A clean fix wants a
     per-connection socket or a drain-before-serve step, left for the
     concurrent-clients work.

---

## 11D: Speculative Local Echo (DONE)

**Goal:** Predict echoed characters on client side for low-latency typing,
confirm or roll back when server responds.

### Design

New attribute flag: `VT_ATTR_PREDICTED (1u << 9)`.

On printable input (when prediction active):
1. Write char into client `vt_state` at predicted cursor position
2. Set `VT_ATTR_PREDICTED` on that cell
3. Advance predicted cursor, push onto pending ring
4. Mark row dirty, render immediately

On server output:
1. Feed through VT parser as normal (overwrites predicted cells)
2. `VT_ATTR_PREDICTED` cleared by `vt_state_putchar()`
3. Pop confirmed chars from pending ring
4. On mismatch: flush pending ring, disable prediction, `render_full`

**Heuristics:** Predict only for printable chars in ground state, no alt
screen, cursor visible. Disable on mismatch, re-enable after 500ms calm.

Predicted cells rendered with `VT_ATTR_DIM` (configurable).

### Files to Change

| File | Change |
|------|--------|
| `src/libvt/vt_cell.h` | Add `VT_ATTR_PREDICTED` |
| `src/cmd/attach/predict.{h,c}` | Prediction logic |
| `src/cmd/attach/attach.c` | Hook prediction into input/output paths |
| `src/librender/render.c` | Render predicted cells with dim style |
| `tests/test_predict.c` | Prediction unit tests |

---

## Alt-Screen Scrollback Capture (Option C) (DONE)

**Status:** Landed. `vt_buf_push_rows` appends rows into a buffer's
scrollback ring; `vt_state_altscreen_leave` calls it (gated by
`vt_state_set_altscreen_scrollback`, default off) to copy the alt buffer's
content, trailing blank rows trimmed, into primary scrollback before the
alt buffer is freed. The attach client enables it per window from the
`attach.altscreen-scrollback` config key. Verified: unit tests for the
capture and the default-off case; a probe through the real
`vt_parse`/`vt_ops` path (a `?1049h ... ?1049l` sequence yields three
captured scrollback rows enabled, zero disabled); and a config-parse probe
confirming the key reads back. The server VT keeps `scrollback == 0`, so
the push is a no-op there and only the client captures.

**Goal:** Make lumi's `ctrl-A [` scrollback viewer show content from
alt-screen applications (TUI apps, editors, claude CLI) rather than
showing only the primary-screen bash history from before the app started.

**Background:** Lumi's scrollback ring is attached to `VT_TARGET_PRIMARY`
only. Apps that use the alternate screen (`VT_TARGET_ALT`, DECSET 1049)
write output that is never captured in the ring. When the user enters
scrollback while such an app is focused, they see bash history instead
of the app's output.

Option B (forwarding mouse wheel events to children that requested mouse
tracking) was implemented first and handles the common case of scrolling
within a live alt-screen app.

**Design:** When a child calls `DECRST 1049` (leaves the alternate screen),
copy the last visible rows of the ALT buffer into PRIMARY's scrollback ring
as synthetic history. On re-entry (DECSET 1049), optionally mark the
boundary so the viewer can show a separator. This is analogous to tmux's
`alternate-screen off` option.

**Changes required:**

| File | Change |
|------|--------|
| `src/libvt/vt_state.c` | In `vt_state_altscreen_leave()`, copy ALT rows into PRIMARY ring |
| `src/libvt/vt_buf.c` | Add `vt_buf_push_rows()` to append rows into the ring head |
| `src/libvt/vt_buf.h` | Declare `vt_buf_push_rows()` |

**Caution:** Pushing rows on every alt-screen exit may produce confusing
history for apps that frequently toggle alt screen (e.g. vim during
startup). A config option to gate the behavior may be warranted.

---

## Kitty Keyboard Flag Forwarding Leak (DONE)

**Status:** Fixed (commit 614f9b3), regression test added (commit e60d303).

**Symptom:** With many windows open, switching between applications that use
the kitty keyboard protocol (vim, the claude CLI) and plain shells (bash)
would sometimes leave the outer terminal unable to enter a newline. Exiting
the terminal emulator and reattaching cleared it.

**Cause:** The attach client mirrors the focused window's effective kitty
keyboard flags onto the outer terminal. `sync_keyboard_proto()` did so with
a stack push (`CSI > flags u`) and pop (`CSI < u`). Those manipulate the
outer terminal's own flag stack. Switching between windows that requested
different nonzero flag sets pushed extra entries without popping them, so the
outer stack grew. Landing on a plain window emitted a single pop, which left
a stale enhanced-keyboard entry active. With report-all-keys in that entry,
Enter arrives as `CSI 13 u`, so plain programs never see a carriage return.

**Fix:** Forward the value with a set (`CSI = flags u`) and reset with
`CSI = 0 u`. The client tracks one effective value and the outer terminal is
a surface it fully owns, so the flag stack never grows. Two lines in
`src/cmd/attach/attach.c`.

**Verification:** `tests/kbd_leak.sh` drives a window through flags
`0 -> 1 -> 15 -> 0` while a real attach client runs on a pty, captures the
bytes sent outward, and asserts they use set ops and never push/pop ops. The
test fails on the pre-fix client and passes on the current one; it is wired
into CI next to the smoke tests.

---

## Window Number Map Race (DONE)

**Status:** Fixed (commit 7f6a0ce), regression test added in the same commit.

**Symptom:** Creating windows quickly could drop some of them from the
session's window-number map, so numbering jumped. Past window 9 the next
window could appear as 13 rather than 10, and the missing windows were absent
from `WINDOW_NUMS` in the session state file.

**Cause:** `sessdir_state.c` commits every state change with a temp file plus
an atomic `rename()`, which swaps `state` to a fresh inode on each write. The
read-modify-write sections `flock()`'d the `state` descriptor itself, which
gave no mutual exclusion: a process that opened the file before a rename
locked the old inode while one that opened it after locked the new inode, so
the two ran at once. A waiter that had blocked on the old inode then read
stale contents and overwrote the other's addition, dropping a window from the
slot map.

**Fix:** Lock a dedicated `state.lock` file that is created once and never
renamed, so every writer contends on the same stable inode, and read the
state file fresh by path inside the lock so each read sees the committed
contents. The lock is opened `O_RDONLY` so its `close()` raises only
`IN_CLOSE_NOWRITE` and does not retrigger the session-directory watch. All
changes are in `src/libsessdir/sessdir_state.c`.

**Verification:** `test_state_concurrent_add` in
`src/libsessdir/test_sessdir.c` forks 24 processes that each register a
distinct window at once, then asserts every one survives in `WINDOW_NUMS`.
The test fails on the pre-fix code and passes on the current one, and it runs
with the rest of the `test_sessdir` suite.

---

## macOS First-Window Startup Hang (DONE)

**Status:** Fixed (commit c95b658). Needs a confirming run on macOS; the
change builds clean and the full test suite passes on Linux, which exercises
the unchanged poll() path.

**Symptom:** On macOS the first window hangs at startup in both screen and
turbo modes. Nothing paints and the shell appears to ignore input, though
Ctrl-A commands still work. Creating a second window unwedges the first,
after which both behave normally.

**Cause:** The mserver arms no timers, so its event loop blocks in `poll()`
with an indefinite timeout waiting on the shell's PTY master. macOS `poll()`
does not report readiness on a pty device, so the loop never wakes for the
shell's output. Ctrl-A works because it is handled entirely in the attach
client, whose loop arms timers and so wakes regardless. Creating a second
window sends a refresh over the first window's mserver socket; that socket
event wakes `poll()`, and the now level-triggered pty read finally runs, so
the window comes alive. A secondary race compounded it: `pty_open()` passed
`NULL` for the winsize to `forkpty()` and resized after the fork, so the
child could exec before its size arrived.

**Fix:** Use `select()` in place of `poll()` on Apple only (Linux and the
BSDs keep `poll()`, which handles ptys); `select()` reports pty masters
correctly. The shim maps `POLLIN`/`POLLOUT` through `fd_set`s and treats a
descriptor closed behind the loop's back as `POLLNVAL`, preserving the
loop's self-healing. It lives in the shared platform layer, so every
poll-on-pty path is covered, not just the mserver. Separately, `pty_open()`
now passes a real winsize to `forkpty()` so the slave is born at the right
size. Changes in `src/libiox/iox_plat.c`, `src/libpty/pty.c`, `pty.h`, and
`src/libsession/window.c`.

**Verification:** Pending on macOS. On Linux the build is clean under
`-Wall -Wextra -Wconversion`, the Apple branch was syntax-checked standalone,
and `test_iox` plus the full `make run-tests` suite pass.

---

## Layout Persistence Hardening (SCOPED)

**Status:** Not started. The blank-screen and lost-title bugs it came from
are fixed (commits 14f62d4, a47b7b1, 9ea000a); the format weakness that
allowed them is not.

**Goal:** Make a saved layout survive changes to the session's window set,
and make an unusable layout impossible to write rather than merely
tolerated on read.

**Background:** `sessdir_layout_save_screen()` stores each pane as an
index into the window order held in the session directory's `state` file,
and stores focus the same way. That order is a separate file with its own
lifetime, so an index is only meaningful while the two agree. When they
disagreed, a pane was written as index -1 and restored as a pane with no
window and no VT, which composites as a blank area no repaint can fill.
Because the restored empty pane was exported as -1 again on the next
detach, a session that hit this once stayed broken.

The reader and writer are now defensive: an unresolvable pane is rejected
on import (a split collapses onto its surviving side, matching what
closing that pane would do), refused on export, and the focus that
`tile_focus()` accepted is read back rather than trusted from the file.
That contains the damage but leaves the layout describing windows by a
position in a list that something else owns.

**Design:** Store the stable window number (the value the tab bar shows,
already tracked in `WINDOW_NUMS`) instead of the order index, and resolve
it through the same lookup the window-select commands use. A window that
has exited then simply fails to resolve, which the import path already
handles, and reordering windows no longer silently repoints panes at
different windows. Bump the layout file with a version or a distinct key
name so an old file is ignored rather than misread as the new format.

**Changes required:**

| File | Change |
|------|--------|
| `src/libsessdir/sessdir_layout.h` | Version the screen layout; name the field for what it now holds |
| `src/libsessdir/sessdir_layout.c` | Serialize/parse window numbers; ignore an unversioned file |
| `src/cmd/attach/attach.c` | Export and import panes by window number in `screen_export_tree()` / `screen_build_tile_tree()` |
| `src/libsessdir/test_sessdir.c` | Round-trip a layout whose windows changed between save and load |

**Open question:** Turbo layouts (`sessdir_layout_save_turbo()`) index the
same window order and have the same weakness, though an unplaceable turbo
window is skipped rather than turned into an empty pane. Worth converting
in the same pass.

---

## Kitty Graphics Protocol (SCOPED)

**Status:** Partially implemented. Anchoring, client and server cursor
accounting, cell-pixel plumbing, and screen-mode window-switch replay have
shipped. Image-number resolution, animation, and turbo-mode clipping remain
design only. See `doc/kitty-graphics.md` (section "As implemented") for what
the code does today.

Extends the existing SIXEL/DCS pass-through (11B) to carry the kitty
graphics protocol through lumi. Because lumi is a screen-buffer
multiplexer, images cannot be parsed into cells; they are tracked as a
host-drawn overlay with per-window bookkeeping that hides placements on
switch out and replays them on switch in.

Feasibility by mode:

- Minimal / single fullscreen pane: works today via the existing
  pass-through.
- Screen mode: feasible and is the target. One full-screen window at a
  time, so the window rect equals the host rect and no clipping is needed.
- Turbo mode: dead end for overlapping windows. The protocol has no clip
  or occlusion primitive, so replay is meaningful only for the top window.

The design adds a per-window placement table, a record path in
`dcs_passthru()`, switch hooks in `micro_select_window()`, and
image-number resolution from host replies (mirroring the OSC color-query
round trip). Replay defaults to re-transmitting image bytes on each switch
(portable, no host assumptions); a lazy variant that trusts host
persistence is deferred behind a latency measurement.

As shipped, the record path and re-transmit replay use a minimal per-window
store (raw APC bytes plus the anchor cell), not the general placement table;
image-number resolution and the lazy variant are unbuilt. Testing found that
kitty ties an image to its anchor cell, so the ED (`\e[2J`) that
`render_cells_full` emits erases all on-screen images. Replay therefore
re-emits the store after every full render, not once per switch.

Confirmed by testing: the prompt originally painted on top of the image
because lumi never advanced its own cell-grid cursor past it. Cursor
accounting was the first fix, on both the client and server vt_state, with
the cursor left on the image's last row (advance `rows - 1`) to match kitty.

Known limit: classic placements anchor at a cursor cell and drift if the
window scrolls. The robust fix is kitty's unicode-placeholder mode, which
needs a `vt_cell` extension (per-cell placeholder codepoint, combining
diacritics, and foreground-encoded image id) not present today.

---

## Phase 12: Shared Attach (SCOPED)

**Status:** Not started.

**Goal:** Let several clients be attached to one session at the same time.
Two stages: **12A** gives one writer plus any number of view-only clients,
**12B** lifts the single-writer rule so every attached client can type, the
way `screen -x` works.

### Why it does not work today

`lumi-mserver` holds exactly one `client_fd`. `on_new_client()` calls
`disconnect_client()` on the previous client before installing the new one,
so a second `lumi attach` silently steals the session one window at a time.
Everything downstream of that fd is single-client too: one output queue, one
`output_paused` flag, one `send_replay()` target. `lumi-proxy` and
`lumi-net-proxy` likewise serve a single client each.

Two properties of the existing design make sharing cheap to add. The mserver
already maintains its own server-side VT image and can replay it to a fresh
connection at any time, so a late joiner needs no cooperation from the other
clients. And each client keeps its own VT replica, scrollback, and renderer,
so viewers can scroll back or open menus without disturbing anyone.

### Vocabulary

| Term         | Meaning                                                     |
|--------------|-------------------------------------------------------------|
| connection   | one client to one mserver, as today                          |
| client       | one `lumi attach` process: N connections plus a role         |
| role         | `WRITE` or `VIEW`, negotiated per connection at attach time  |
| write token  | session-scoped right to hold `WRITE`, one holder in 12A      |
| coupling     | `MIRROR` (follow the session layout and focus) or `FREE`     |

Role and coupling are orthogonal. A view-only client may follow the writer
(`MIRROR`, the pair-programming case) or browse windows on its own (`FREE`,
the "watch the build log while you work" case). Default for `attach -v` is
`MIRROR`; the writer is always `FREE` because it owns the layout.

---

## 12A: One Writer, Many Viewers

### 1. mserver client fan-out

Replace the `client_fd` / `outq` globals with a small table:

```c
#define MCLIENT_MAX 8

struct mclient {
    int         fd;
    uint8_t     role;       /* MCLIENT_WRITE | MCLIENT_VIEW */
    uint8_t     flags;      /* SIZE_NEGOTIATE, MIRROR, ... */
    uint32_t    client_id;  /* attach-process id, same across windows */
    uint16_t    rows, cols; /* this client's desired size */
    uint8_t     *outq;      /* per-client queue, as today */
    size_t      outq_off, outq_len, outq_cap;
    int         hiwater;
};
```

`client_enqueue()` becomes `mclient_enqueue(mc, ...)` plus a
`broadcast(type, payload, len)` helper for OUTPUT and PTY\_FLAGS. Attach
replays only to the joining connection. Everything else in the file is
already written against "the client fd" and converts mechanically.

### 2. Attach handshake

Microser skips unknown tags, so the handshake extends without a flag day.
New IDL messages, with tags 1 and 2 kept compatible with `IpcSize`:

```
message IpcAttach
    uint16 rows = 1
    uint16 cols = 2
    uint8  flags = 3        # role wanted, size policy, coupling
    uint32 client_id = 4    # same value on every window of one client
    string name = 5         # "user@host", shown in the roster
end

message IpcAttachReply
    uint16 rows = 1
    uint16 cols = 2
    uint8  role = 3         # role actually granted
    uint8  nclients = 4     # how many are attached now
end
```

An old mserver reads `IpcAttach` as `IpcSize` and behaves exactly as it does
now. An old client reads `IpcAttachReply` as `IpcSize` and ignores the rest.

Two new server-to-client messages: `IPC_MSG_ROLE_CHANGE` (role was granted
or revoked while attached) and `IPC_MSG_CLIENT_EVENT` (someone attached,
detached, or took the token). The second exists so no session can be watched
without the other clients being told.

### 3. Input authority

The mserver accepts `INPUT`, `WIN_RESIZE`, `KILL`, `FLOW_CTRL`, and the
attribute write messages only from a connection whose role is `WRITE`.
Anything else is dropped, with a rate-limited `IPC_MSG_ERROR` back so the
client can flash "read-only" rather than appear hung. At most one `WRITE`
connection exists per window in 12A; a second requester is granted `VIEW`
and told so in the reply.

### 4. The write token

Per-window enforcement is not enough on its own, because a client attaches
to every window in the session and two clients could each win a different
window. Session-wide agreement comes from an advisory lock:

- `<session>/control.lock`, held `LOCK_EX | LOCK_NB` for the lifetime of the
  writer process.
- A client acquires it before attaching with `role=WRITE`. If the lock is
  taken, it attaches as `VIEW` instead.
- The kernel releases it when the writer dies, so a crashed or killed writer
  never wedges the session. The next client to ask simply gets it.

The mserver check stays as the safety net for a buggy or hostile client.

### 5. Handing the token over

A viewer that wants to type writes `<session>/control.req` with its client
id, pid, and name. The writer notices through the existing sessdir watch and
prompts. On grant it drops the flock, re-attaches its connections as `VIEW`,
and the requester takes the lock and re-attaches as `WRITE`. Both sides learn
the outcome through `IPC_MSG_ROLE_CHANGE`, so the transfer is visible even to
clients that were not involved.

New `lumi share` subcommand:

| Command                | Effect                                            |
|------------------------|---------------------------------------------------|
| `lumi share -l`        | list attached clients, roles, sizes, attach times  |
| `lumi share -g <id>`   | grant the write token to a client                 |
| `lumi share -t`        | take the token back (or take an unheld one)       |
| `lumi share -k <id>`   | kick a client                                     |
| `lumi share -L`        | lock the session, refuse further attaches         |

Plus `KEYS_ACTION_SHARE_MENU`, an overlay listing clients with grant, revoke,
and kick, so none of this needs a second terminal.

### 6. Size negotiation

The mserver keeps each connection's desired size and applies
`min(rows), min(cols)` over the connections that set `SIZE_NEGOTIATE`. If no
connection sets it, the writer's size wins. Viewers default to observe: they
never send `WIN_RESIZE` and never shrink the writer's window. A viewer whose
terminal is smaller crops around the cursor and marks the pane as clipped; a
larger one letterboxes. `share.resize = negotiate|observe` in `lumi.conf`
picks the default, matching `screen -x` when set to negotiate.

### 7. Slow clients must not stall the session

Today one backlogged client pauses PTY reads for everyone. With fan-out the
rule becomes:

- PTY reads pause only when a `WRITE` or `SIZE_NEGOTIATE` client crosses the
  high-water mark. Those clients are participants, so backpressure to the
  application is correct.
- A `VIEW` client that crosses a hard cap (4 MiB) or makes no progress for a
  few seconds is dropped with `IPC_MSG_ERROR`, not throttled. A viewer on a
  bad link is never allowed to slow down the person working.
- `FLOW_CTRL` is honored only from a writer.

### 8. Roster and notice

Each client registers `<session>/clients/<client_id>/info` holding its name,
pid, role, mode, and size, and removes it at exit. `sessdir_cleanup_stale()`
grows a pass to prune entries for dead pids. `lumi list -c` prints the
roster, the taskbar gains a `share: N` field and an `[RO]` marker when the
local client cannot type, and every client shows a transient notice when
someone joins or leaves.

### 9. Mirror coupling

A `MIRROR` viewer follows focus, window order, and layout from the state and
layout files, which the token holder already writes. Only the token holder
writes them, so there is no contention in 12A. Propagation uses the existing
sessdir watch, with a 250 ms poll fallback for the degraded-watch case that
already prints `[!watch]`. This reuses machinery that exists rather than
adding a session-level daemon, which the micro-server architecture does not
otherwise need.

### 10. Proxy and network clients

Keep the "one client per proxy process" rule and let the accept loop fork a
child per client, each owning its own mserver connections. N remote clients
then become N ordinary clients from the mserver's point of view, and no
demultiplexing logic is added to the proxies.

Every client that is not the session owner's own uid goes through a proxy
acting as a broker, whether it arrives over netchan or over a local socket.
The mserver sockets stay same-uid because `$XDG_RUNTIME_DIR` is 0700, and
there is no setuid multiuser mode of the kind GNU Screen has. A keystore
entry may carry `role=view`, and `lumi-net-proxy` then clamps that client to
`VIEW` no matter what its attach handshake asks for. That makes "here is a
key that lets you watch" expressible without trusting the peer's client.
12C below defines the policy both brokers share.

---

## 12B: Full Sharing (screen -x)

12B drops the single-writer rule. The token survives as an optional input
lock rather than a precondition for typing.

- **Mode setting.** `share.mode = single-writer|multi-writer`, stored in
  `<session>/control` so every client agrees. The mserver reads it at
  startup and on change, and stops rejecting a second `WRITE`.
- **Input atomicity.** `write_to_pty()` already loops over partial writes, so
  two clients can only interleave between messages. That is still wrong for
  an escape sequence split across messages or a bracketed paste. Clients send
  one key event per `INPUT` message, and a multi-message run (paste,
  `send-keys`) is bracketed so the mserver buffers the run per connection and
  flushes it as a unit.
- **Size.** All writers participate in the `min()` negotiation by default,
  which is the `screen -x` behavior.
- **Display group.** `share.display = shared|independent`. Shared means every
  client mirrors one layout and focus and any client may change it.
  Independent means each client keeps its own layout while all may type.
- **Layout write conflicts.** The state and layout files gain a generation
  counter and are written temp-plus-rename. A client that sees a newer
  generation re-imports before writing. Last-writer-wins is acceptable for
  layout; this only has to stop a torn read.
- **Speculative echo (11D).** A prediction is only valid while the local
  client is the sole source of input. In multi-writer mode predictions are
  restricted to the conservative case and discarded as soon as output arrives
  that the prediction does not explain.

---

## 12C: Access Control and Presence

Roles are only meaningful if the server decides them. The client's requested
role is a wish; the granted role is `min(requested, policy)`. This section
defines where the policy comes from, how the identity behind a connection is
established, and how presence is made visible.

### Trust tiers

| Tier | Peer                          | Enforced by                        | Roles available |
|------|-------------------------------|------------------------------------|-----------------|
| 0    | same uid, local socket        | `$XDG_RUNTIME_DIR` 0700 plus peer credential check | any |
| 1    | different local uid           | broker process, uid/gid ACL        | as the ACL says, default deny |
| 2    | remote over netchan           | keystore key, ACL by key name      | as the ACL says, default deny |

Tier 0 needs no new policy. The session directory is 0700, so only the owner
can reach the sockets, and the owner can already run arbitrary code as
themselves. Any role is reasonable there, and the write token from 12A is a
coordination mechanism, not a security boundary.

Tiers 1 and 2 are where authorization is real, and both are default deny.

### Establishing who is on the other end

Never trust the `name` field in `IpcAttach`. It is display text supplied by
the peer. It is used for the roster only, it is sanitized (control characters
and escape sequences stripped) before it is ever rendered into a taskbar or
overlay, and it is never an input to a policy decision. A remote peer that
can inject SGR or cursor sequences into another client's status line through
its own claimed name is a real attack, not a theoretical one.

The identity that counts comes from the transport:

| Platform      | Mechanism                                        |
|---------------|--------------------------------------------------|
| Linux         | `getsockopt(SO_PEERCRED)`, `struct ucred`         |
| macOS, \*BSD  | `getpeereid()`                                    |
| Solaris       | `getpeerucred()`                                  |
| netchan       | the keystore key that completed the handshake     |

New in libipc: `int ipc_peer_cred(int fd, uid_t *uid, gid_t *gid, pid_t *pid)`,
returning ERR where the platform has no such call. Notes that matter:

- The credentials are captured at `connect()` time by the kernel and cannot
  be forged by the peer, including by a later `exec` of a setuid binary.
- The pid is advisory. Pids are reused, so it is displayed in the roster and
  never used for a decision. The uid and gid are the decision inputs.
- Where `ipc_peer_cred()` is unsupported, cross-uid sharing is refused
  outright rather than falling back to trusting the filesystem. Fail closed.

### Foreign uids never touch an mserver

The rule for mservers stays absolute and cheap to verify: **an mserver
accepts a connection only from its own uid.** Anything else is closed before
the ATTACH message is read.

Every foreign-uid client goes through a broker instead: `lumi-proxy` in
listen mode, running as the session owner, holding its own mserver
connections exactly as the network path already does. The broker does the
peer credential check, evaluates the ACL, clamps the role, and relays. This
buys several things at once:

- One place implements policy, shared by the local cross-user path and the
  netchan path, which already clamps roles from the keystore.
- Session control state (`control.lock`, the roster, the layout files) stays
  0700 and owner-owned. A foreign client can never read or write it, so token
  requests and roster queries arrive as protocol messages, not files.
- The mserver's per-connection check reduces to one uid comparison.

### Socket exposure for tier 1

The private per-window sockets stay unreachable: directory 0700. Only the
broker endpoint is exposed, only while sharing is enabled, and it is removed
when sharing is turned off.

| Mode                  | Endpoint permissions                            |
|-----------------------|-------------------------------------------------|
| `lumi share -g devs`  | socket 0660, group `devs`, in a 0710 directory  |
| `lumi share -u alice` | socket 0666 in a sticky 0733 directory, authorization entirely by peer credential |

Group mode is preferred when a suitable group exists, because the kernel
rejects the connection before lumi sees it. The 0666 mode is not a hole
given that the broker authorizes every connection by uid, but it does let
any local user open a connection, so the broker applies a per-uid concurrent
connection cap and a connection rate limit, and drops unauthenticated
connections that do not complete a handshake within a short timeout.

Do not rely on the mode of the socket file itself as the only gate. Some
systems have historically ignored socket permissions on `connect()`. The
directory mode is honored everywhere and the credential check is honored
everywhere; the socket mode is a third layer, not the layer.

Related hardening, worth doing regardless of sharing: the `/tmp/lumi-<uid>`
fallback in `ipc_socket_dir()` and `sessdir_base()` does `mkdir(path, 0700)`
and continues on `EEXIST`. In a world-writable `/tmp` another user can create
that directory first. The fallback must `lstat` the result, and refuse to use
it unless it is a real directory, owned by the current uid, with no group or
other bits set. The socket should also be `fchmod`ed explicitly after bind
rather than inheriting whatever umask the caller had.

### The ACL

A session-level file, `<session>/access`, mode 0600, written only by the
owner, refused if its ownership or mode is wrong. Reloaded on change through
the existing sessdir watch. First match wins, and the implicit last line is
deny.

```
# subject          role    options
owner              write
user:alice         write   ask
group:devs         view
key:bob@laptop     view    ask
*                  deny
```

Subjects are `owner`, `user:<name|uid>`, `group:<name|gid>`, `key:<name>` for
netchan peers, and `*`. The role column is the ceiling for that subject, not
a grant of that exact role: a `write` subject that attaches asking for `view`
gets `view`.

The `ask` option is the VNC-style prompt. A connection matching an `ask` rule
is admitted in a **pending** state: it is registered in the roster, it is
counted in the presence indicator, and **it is sent no replay and no output**
until it is approved. Nothing about the session leaks while the prompt is up.
Approval comes from the write-token holder, or from any owner client if there
is no writer. No one available to approve means deny. A timeout means deny.

An ACL change re-evaluates live connections, not just new ones. A subject
that loses `write` is downgraded through `IPC_MSG_ROLE_CHANGE`; a subject
that loses access entirely is disconnected. Revocation that only applies to
future connections is not revocation.

### Who gets told what

Presence is symmetric. Every attached client learns about every other
attached client. Telling only the writer would leave a viewer unaware that a
third party is also watching, which is precisely the property that makes
silent observation possible.

Prompts are different from notices. A prompt is a decision, so it goes to the
one client that can make it, and the outcome is then broadcast as a notice.

| Event                         | Prompt                    | Notice        |
|-------------------------------|---------------------------|---------------|
| tier 0 client attaches        | none                      | all clients   |
| `ask` subject connects        | token holder, else owner  | all, on outcome |
| non-`ask` foreign client attaches | none                  | all clients   |
| write token requested or moved | token holder             | all clients   |
| client kicked or denied       | none                      | all clients   |

### The indicator

The transient notice is not the mechanism that matters. A persistent
indicator is, in the same spirit as a VNC server's tray icon.

- The taskbar carries a share field whenever more than one client is
  attached: `share:1w+2v` for one writer and two viewers, with a distinct
  color when any attached client is not the session owner's uid. The exact
  format is configurable through `share.indicator`.
- In turbo mode the same badge appears in the focused window's title bar,
  since the taskbar can be covered.
- In minimal mode there is no taskbar, so the indicator goes in the host
  terminal title (`[shared]`), and a join or leave draws a one line reverse
  video banner that clears on the next keypress.
- The indicator is not suppressible while a foreign-uid or view-only client
  is attached. Configuration can change the format, not remove it. This
  follows the precedent already set by the `[!watch]` marker, which is drawn
  independently of `taskbar.format` so a degraded state is always visible.

`lumi share -l` prints the same information in full: client id, name, uid or
key, role, coupling, size, attach time, and pending state.

### Audit

The broker appends one line per decision to `<session>/access.log`, mode
0600: timestamp, subject, uid or key name, requested role, granted role, and
reason on denial. The file is owner-only and never exposed to a foreign
client. This is cheap and it is the only way to answer "who watched this
session while I was away" after the fact.

### Fail-closed rules

1. No peer credential support on the platform means no cross-uid sharing.
2. An unreadable, wrongly-owned, or wrongly-moded ACL means owner-only.
3. No one available to answer an `ask` prompt means deny.
4. A malformed or unparsable ACL line is a denial for that line, not a skip.
5. A pending client receives no session content of any kind.
6. Losing the ability to display the indicator means refusing the client
   rather than sharing without one.

### Files to Change

| File | Change |
|------|--------|
| `src/cmd/mserver/mserver.c` | Client table, per-client outq, roles, size negotiation, slow-viewer drop, same-uid-only accept |
| `src/libipc/ipc.[ch]` | `ipc_peer_cred()`, explicit socket mode after bind, hardened `/tmp` fallback |
| `src/libsessdir/sessdir_access.[ch]` | New: ACL parse, subject match, live reload, audit log |
| `src/libipc/lumi.idl` | `IpcAttach`, `IpcAttachReply`, `IpcClientInfo` |
| `src/libipc/lumi_msg.[ch]` | Regenerated from the IDL |
| `src/libipc/ipc_msg.h` | `IPC_MSG_ROLE_CHANGE`, `IPC_MSG_CLIENT_EVENT`, role flag constants |
| `src/libsessdir/sessdir_control.[ch]` | New: write token, request file, client roster |
| `src/libsessdir/sessdir.c` | Prune dead client entries in `sessdir_cleanup_stale()` |
| `src/cmd/attach/attach.c` | Role handling, read-only input path, mirror coupling, notices, share overlay, name sanitizing |
| `src/cmd/share/share.c` | New subcommand, plus `module.mk` and multicall registration |
| `src/cmd/detach/detach.c` | Detach one client rather than all |
| `src/cmd/list/list.c` | `-c` roster output |
| `src/cmd/proxy/proxy.c` | Fork per accepted client, listen mode as the cross-user broker, peer credential check, ACL, role clamp |
| `src/cmd/net-proxy/net_proxy.c` | Fork per accepted client, ACL by key name, role clamp |
| `src/libnet/keystore.c` | Per-key `role=` annotation |
| `src/libkeys/keys.[ch]` | `KEYS_ACTION_SHARE_MENU` |
| `src/libtaskbar/taskbar.c` | Share indicator, `[RO]` marker, non-suppressible when foreign clients are attached |
| `src/libcfg` consumers | `share.mode`, `share.resize`, `share.display`, `share.indicator` |
| `doc/lumi.1.in`, `doc/DEV.md` | Document the subcommand, flags, config, ACL file, and protocol |
| `tests/` | Multi-client mserver test, token handoff test, roster round-trip, ACL match and reload, pending-client leak test |

### Step-by-Step Implementation Plan

Each step is one commit, builds clean, passes `make run-tests`, and leaves
the tree usable. Steps 1 and 2 are independent of everything else and can
land in any order. Nothing before step 11 exposes anything to another user.

Milestones: **step 6** gives a working view-only second client, **step 10**
gives the finished single-writer feature, **step 12** opens it to other
users, **step 13** is `screen -x`.

---

**Step 1. Peer credentials in libipc.**

Add `ipc_peer_cred(int fd, uid_t *uid, gid_t *gid, pid_t *pid)` to
`src/libipc/ipc.[ch]`: `SO_PEERCRED` on Linux, `getpeereid()` on macOS and
BSD, `getpeerucred()` on Solaris, ERR with `errno = ENOTSUP` elsewhere.
Add `IPC_HAVE_PEER_CRED` so callers can compile out cross-uid paths.

Test in `src/libipc/test_ipc.c`: socketpair and a real listen/connect pair,
assert the reported uid matches `getuid()`, assert ERR handling compiles and
returns cleanly on an unsupported build.

Done when: `test_ipc` covers both socket kinds and the unsupported path.

---

**Step 2. Harden the runtime directory and socket modes.**

`ipc_socket_dir()` and `sessdir_base()` both `mkdir(path, 0700)` and continue
on `EEXIST`, which another local user can pre-create under a world-writable
`/tmp`. Add a shared `lu_runtime_dir_ensure(path)` in libcore: create the
directory, then check it through an `O_DIRECTORY | O_NOFOLLOW` fd so the
check cannot be raced, requiring a real directory owned by `getuid()` with
no group or other bits. Refuse otherwise, with a diagnostic naming the path.
A loose mode on a directory we own is tightened rather than refused, since
that is a umask accident and not an attack. In `ipc_listen()`, bind under a
temporary `umask(0177)` so the socket is 0600 regardless of the caller.

Test: create a directory with wrong ownership bits in a temp tree and assert
the check refuses it; assert the socket's mode after `ipc_listen()`.

Done when: no code path uses a runtime directory it has not validated.

---

**Step 3. mserver client table, still single client.**

Pure refactor of `src/cmd/mserver/mserver.c`. Replace the `client_fd`,
`outq*`, and `output_paused` globals with `struct mclient` and a
`clients[MCLIENT_MAX]` table sized to 1 for now. Convert
`client_enqueue()`, `client_flush()`, `update_client_interest()`, and
`disconnect_client()` to take an `struct mclient *`. Behavior is identical:
a second attach still disconnects the first.

Done when: `tests/smoke.sh` passes and a manual attach, detach, reattach
cycle shows no regression. No protocol change, no new tests.

---

**Step 4. Fan-out.**

Raise `MCLIENT_MAX` to 8. `on_new_client()` appends rather than replacing.
OUTPUT, PTY\_FLAGS, and title updates broadcast; ATTACH\_REPLY and the
replay go only to the joining connection. Reject the connection with
`IPC_MSG_ERROR` when the table is full. Also enforce here what step 1 made
possible: close any connection whose peer uid is not the mserver's own uid,
before reading its ATTACH.

Flow control changes with it. Each client owns its queue. PTY reads pause
only while a client that is a writer or a size participant is over the high
water mark. A client over a 4 MiB hard cap, or making no progress for 10
seconds, is dropped with `IPC_MSG_ERROR`. `FLOW_CTRL` from a non-writer is
ignored, which for now means it is ignored from nobody, since every client
is still a writer.

New test `src/cmd/mserver/test_mserver.c`: fork an mserver against a dummy
child, connect three clients, assert all three get the replay and the same
OUTPUT stream, assert a client that stops reading is dropped without
stalling the other two, assert a foreign-uid connection is refused where the
test can arrange one.

Done when: two `lumi attach` processes on one session both show live output.
Input from both still reaches the PTY, which is the wrong behavior and step
6 fixes it.

---

**Step 5. Attach handshake.**

Add `IpcAttach` and `IpcAttachReply` to `src/libipc/lumi.idl` with tags 1
and 2 identical to `IpcSize`, run `make gen-ipc-msg`, review the generated
diff. Add `IPC_MSG_ROLE_CHANGE` and `IPC_MSG_CLIENT_EVENT` plus the role and
flag constants to `src/libipc/ipc_msg.h`. The client sends `IpcAttach` with
a per-process `client_id`, its sanitized name, and its requested role. The
mserver stores them per connection and echoes the granted role, which is
still always `WRITE`.

Test in `src/libipc/test_ipc.c`: encode `IpcAttach`, decode it as `IpcSize`,
assert rows and cols survive; encode `IpcAttachReply`, decode as `IpcSize`,
same. That is the compatibility claim, so it needs a test that fails if
someone renumbers a tag.

Done when: an old mserver binary and a new client still interoperate, tested
by hand against the previous build.

---

**Step 6. Role enforcement and the read-only client.**

The mserver grants `WRITE` to the first connection that asks and `VIEW` to
every later one, and rejects `INPUT`, `WIN_RESIZE`, `KILL`, `FLOW_CTRL`, and
the attribute write messages from a `VIEW` connection, answering with a
rate-limited `IPC_MSG_ERROR`. In `src/cmd/attach/attach.c`, add `attach -v`
to request `VIEW`, handle `ROLE_CHANGE`, suppress local input paths when
read-only, and flash the server's refusal rather than swallowing it.
Scrollback, copy mode, menus, and window switching stay available to a
viewer, since they are client-local.

Test: extend `test_mserver.c` to assert a `VIEW` connection's INPUT never
reaches the PTY and produces an ERROR.

Done when: `lumi attach -v` in a second terminal shows the session live and
cannot type into it. This is the first genuinely useful milestone.

---

**Step 7. Session control: token and roster.**

New `src/libsessdir/sessdir_control.[ch]`: acquire and release the
`control.lock` flock, write and read `control.req`, register and enumerate
`clients/<client_id>/info`. Extend `sessdir_cleanup_stale()` to prune roster
entries whose pid is gone. The attach client acquires the token before
attaching with `WRITE` and falls back to `VIEW` when it is held, which
replaces step 6's per-window first-come rule as the primary decision. The
mserver check stays as the safety net.

Test in `src/libsessdir/test_sessdir.c`: two processes contend for the
token, assert exactly one wins; kill the winner, assert the next caller can
take it; round-trip a roster entry; assert a stale entry is pruned.

Done when: whichever client started first keeps the keyboard across every
window in the session, and killing it lets the other take over.

---

**Step 8. `lumi share` and the share overlay.**

New `src/cmd/share/share.c` with `-l`, `-g`, `-t`, `-k`, `-L`, registered in
`multicall.c` and a `module.mk`. Add `KEYS_ACTION_SHARE_MENU` to
`src/libkeys/keys.[ch]` and an overlay in the attach client listing clients
with grant, revoke, and kick. Implement the handoff: request file, prompt on
the token holder, flock release, re-attach at the new role on both sides,
`ROLE_CHANGE` broadcast so uninvolved clients see the move.

Test: `keys` action name round-trip; a scripted handoff in
`tests/smoke.sh`.

Done when: the keyboard can be handed to a viewer and taken back, from
either the subcommand or the overlay.

---

**Step 9. Notices and the presence indicator.**

`CLIENT_EVENT` fan-out on join, leave, role change, and kick. In the attach
client, a transient notice for every event, to every client. In
`src/libtaskbar/taskbar.c`, the persistent `share:1w+2v` field, drawn
whenever more than one client is attached and not suppressible by
`taskbar.format`, following the `[!watch]` precedent. Turbo mode draws the
same badge in the focused title bar; minimal mode puts `[shared]` in the
host terminal title and draws a one line banner on join and leave. Sanitize
peer-supplied names here, at the render boundary, not only at parse time.

Test: a name containing `ESC [ 31 m` and a raw newline round-trips through
the roster and renders as printable text. This is a real injection path, so
it gets a test rather than a code comment.

Done when: no client can be attached without every other client showing it.

---

**Step 10. Mirror coupling and size negotiation.**

Two independent changes, one commit each.

Mirror: a `VIEW` client with `MIRROR` coupling follows focus, order, and
layout from the state and layout files through the existing sessdir watch,
with a 250 ms poll fallback when the watch is degraded. Only the token
holder writes those files. `attach -v` defaults to `MIRROR`, `--free` opts
out.

Size: the mserver keeps each connection's desired size and applies the
minimum across connections that set `SIZE_NEGOTIATE`, falling back to the
writer's size when none do. Viewers default to observe and crop around the
cursor, marking the pane as clipped. `share.resize` picks the default.

Test: `test_mserver.c` asserts the effective size is the minimum over
participants and is unaffected by an observer.

Done when: a viewer follows the writer window for window, and a small
viewer terminal does not shrink the writer's shell.

---

**Step 11. Fork per client in the proxies. (DONE)**

`src/cmd/proxy/proxy.c` is already one client per process: it is spawned
over an ssh command's stdin/stdout, so sshd forks a fresh proxy per
connection with no accept loop of its own. The real work was in
`src/cmd/net-proxy/net_proxy.c`'s `-L` direct-connect listener, the only
proxy with a genuine accept loop, which previously rebound its UDP socket
and served one client fully before the next.

The listening socket is now bound once with `SO_REUSEPORT` and stays up for
the process lifetime. On a new client's first datagram, the parent peeks
(`MSG_PEEK`, never consuming it there), forks a child, and the child binds
its own `SO_REUSEPORT` socket to the same `<bindaddr, port>` and `connect()`s
it to that one peer -- the kernel then routes that peer's later datagrams to
the child's more specific socket instead of the listener. Only after the
child exists does the parent drain the peeked datagram, so it is never
re-peeked into a fork storm; the child instead relies on netchan's own
handshake retry to see the client's next attempt, now on its own socket.
Children are capped at 16 concurrent, reaped with `waitpid(WNOHANG)` each
accept-loop tick, and a child's crash or exit never touches the listener
(each has fully independent state after fork). On shutdown the listener
stops accepting and SIGTERMs every still-running child before exiting.

Test: `src/cmd/net-proxy/test_net_proxy.c` gained
`test_listen_concurrent_clients`, which opens two clients against one `-L`
listener before either finishes, interleaves a round trip through each to
prove neither's traffic leaks into the other's bridge, then confirms the
listener still serves a further client once both leave. The fake mserver
test fixture was changed to fork per accepted connection (mirroring real
mserver client fan-out) so it can serve both concurrently attached proxies
instead of stalling the second behind the first.

Done when: two remote clients can attach to one session at once, both
subject to the role rules from step 6.

---

**Step 12. Cross-user access: ACL, broker, prompt, audit.**

Split into sub-steps, one commit each, matching the size of step 10's
12A/12B split.

**12-a. The ACL itself. (DONE)**

New `src/libsessdir/sessdir_access.[ch]`: parse `<session>/access`, refuse
the file unless it is 0600 and owner-owned (falling back to owner-only,
never erroring open), match subjects (`owner`, `user:<name|uid>`,
`group:<name|gid>`, `key:<name>`, `*`) in order with an implicit final
deny, treat a malformed line as a denial for that match rather than a skip
past it, and append decisions to `access.log` (mode 0600). Group matching
uses the peer's gid straight from `ipc_peer_cred()` for the primary-group
case (so it works even for a uid with no local passwd entry) and
`getgrouplist()` for supplementary groups when the uid does resolve to one.
There is no caching, so "reload on change" is just the natural consequence
of reading the file fresh on every `sessdir_access_check()` call -- verified
by a live-reload test that edits the file between two checks with no
explicit reload step in between.

Test: new `src/libsessdir/test_access.c` (42 checks) covers subject
matching, first-match-wins, implicit deny, a malformed line denying (both a
bad role on a matching subject and a wholly unparsable subject, which must
match everyone rather than no one), wrong file mode falling back to
owner-only, live reload, and the audit log's fields and mode. Verified
under `SANITIZE=address` and the musl static build (`getgrouplist()` and
`gmtime_r()` availability was the risk there).

Two bugs caught by this test suite before it ever left this machine: group
matching originally re-derived the peer's primary gid via
`getpwuid(who->uid)`, which fails outright for a uid with no local account
and silently broke the whole "group:" subject for exactly the cross-machine
case it exists for; and an unparsable subject with an otherwise
well-formed role/options (e.g. a line with a typo in the subject column but
`write` spelled correctly) fell through to whatever role parsed instead of
being forced to deny, defeating the fail-closed intent of rule 4.

Not yet done, deferred to the remaining sub-steps below: wiring this into
either broker, `lumi share -u/-g`, pending (`ask`) clients, the presence
indicator, and the keystore `role=` annotation.

**12-b. Wire the ACL into the local broker (`lumi proxy` listen mode). (DONE)**

`src/cmd/proxy/proxy.c` had no listen mode at all -- it only ran over an
ssh command's stdin/stdout for the single-owner-uid path. `lumi proxy -L
[-g group]` adds one: a Unix socket at `/tmp/lumi-broker-<uid>/<session>.sock`
(deliberately outside `$XDG_RUNTIME_DIR/lumi`, which stays 0700 -- a broker
endpoint has to be reachable by a foreign uid's directory traversal from a
world-traversable ancestor, and `/tmp` already is one), with the 12C
permissions table (0660/group-owned in a 0710 directory for `-g`, else
0666 in a sticky 0733 directory). `lu_runtime_dir_ensure_mode()` (new,
generalizes step 2's `lu_runtime_dir_ensure()` to a caller-chosen mode
rather than a hardcoded 0700) creates and re-validates that directory each
run.

Fork per accepted connection, mirroring step 11's `-L` listener but simpler
since a real Unix listen socket has ordinary `accept()` semantics (no
UDP-demux trick needed). Each child calls `ipc_peer_cred()`, then
`sessdir_access_check()` against the session's ACL; an `ask` match is
currently a denial (fail-closed rule 3: nothing implements approval yet,
so there is no one to approve it -- that lands in 12-d). The granted
ceiling becomes the flags on the broker's own `attach_mserver()` call to
each window: `attach_mserver()` now sends a real `IpcAttach` (client id,
name, and flags) instead of the legacy empty ATTACH it used before, so
`IPC_ATTACH_F_VIEW` reaches the mserver exactly as a direct `-v` attach's
would. Every decision is logged via `sessdir_access_audit()`. There is no
per-client role negotiation from the far end yet (the wire protocol has no
message for it): the broker always requests the ACL's full ceiling, so
`min(requested, ceiling)` is really just `ceiling` until a client-side
"what role do I want" message exists -- noted as a gap, not silently
assumed away.

`lumi share -u <user>` / `-G <group>` (not `-g`: that letter was already
taken by "give the keyboard to this client id" from step 8) start the
broker as a background daemon -- fork, `setsid()`, redirect stdio to
`/dev/null`, then call `cmd_proxy_main()` directly in-process rather than
re-exec'ing, since it's the same `lumi` binary already. Each seeds
`<session>/access` with a minimal default (`owner write`, the new
subject at `view`, `* deny`) only if the file does not already exist, so a
hand-curated ACL is never clobbered. The broker publishes its own pidfile
(`<broker dir>/<session>.pid`, written only once the socket is actually
listening) so `lumi share -B` can find and `SIGTERM` it, and removes both
the pidfile and the socket on clean shutdown. Path helpers live in a new
`src/cmd/proxy/proxy_broker.h` so `proxy.c` and `share.c` share one
formula rather than each guessing at the layout independently.

`proxy.c` also gained its own build as a library (`lu_proxy`, mirroring
`lu_netproxy`'s shape) rather than being compiled directly into
`lumi_SRCS` with no test coverage of its own, so `test_proxy.c` could
exist at all; it previously had zero tests. New: a stdio-path regression
check (attach_mserver()'s wire format changed, even though its behavior
for the trusted ssh path did not), and three broker tests -- full access
granted with no ACL file (owner-only fallback), an explicit `* deny`
denying before anything is sent (verifying fail-closed rule 5 with a real
socket rather than a mock), and an ACL ceiling of `view` producing
`IPC_ATTACH_F_VIEW` on the mserver-facing attach. All three use the test's
own real uid through an ACL rule written for it, since a unit test cannot
forge a second kernel-reported uid; this exercises the exact same code
path a foreign uid would take.

One real bug caught before commit: the accept loop initially called
`ipc_accept()` with no preceding `poll()`. `signal()` installs SIGTERM
with `SA_RESTART` on Linux, so a blocked `accept()` with no pending
connection just resumed after the signal instead of ever re-checking the
stop flag -- a broker with nothing currently connecting to it would not
stop on `lumi share -B` until some other connection attempt woke it up.
Fixed by polling with a timeout first, same pattern as step 11's
`-L` listener.

Verified: `make run-tests`, `tests/smoke.sh`, `SANITIZE=address` (clean),
the musl static build, and a live end-to-end run with a real mserver and
`socat` as the client, plus `lumi share -u`/`-G`/`-B` exercised end to end
by hand (ACL seeding, broker start/stop, group-owned vs world socket
permissions).

A second real bug, caught by a post-commit review of this step alongside
12-c: `broker_serve_client()` set `proxy_client_id = (uint32_t)who.uid`
rather than this child's own pid, unlike the ssh path and `net_proxy.c`'s
`-L` listener, both of which use `getpid()`. Two connections from the same
uid would send identical `client_id` in their `IpcAttach`, colliding in the
mserver's per-window roster. Fixed to use `getpid()` like the other two
paths.

Known gap, deferred rather than glossed over: nothing in `lumi attach` can
reach this broker yet. It has no client-side wiring for "connect to a
local cross-user broker by path" the way `-n` reaches a netchan proxy; the
broker is reachable today only by a raw client speaking the existing
`proxy_msg` framing directly (which is exactly what `test_proxy.c` and the
manual `socat` check above do). Adding that is either part of 12-d or a
follow-up of its own.

**12-c. Wire the ACL into `lumi-net-proxy`. (DONE)**

The `-L` listener authenticates a netchan client by name (`ipc_netchan_userauth`)
but had nowhere to persist that name past the handshake: `nct_run_userauth()`
kept its `struct nc_auth` on the stack and threw the authenticated user away
the moment it returned. New `ipc_transport_netchan_auth_user()`
(`src/libnet/ipc_transport_netchan.[ch]`) persists it into `struct nct` on
successful userauth (server side only) and exposes it, so `net_proxy.c` can
learn "who authenticated" once `ipc_transport_netchan_establish()` succeeds.

`serve_one_client()` uses that name to build a `sessdir_access_who` with
`is_key` set (the `authorized_keys` name and the ACL's `key:<name>` subject
are the same string -- there is no separate "key name" concept), checks it
against `sessdir_access_check()`, and clamps `attach_mserver()`'s requested
role to the result exactly as 12-b's broker does. `attach_mserver()` itself
was upgraded from the legacy empty ATTACH to a real `IpcAttach` (client id,
name, flags), the same change 12-b made to `proxy.c`'s copy. A `NULL` from
`ipc_transport_netchan_auth_user()` (the ssh-bootstrap and PSK-only daemon
paths, which never configure `.userauth`) keeps the unrestricted default,
since those are reachable only by the session owner's own uid over a
channel that already needed pre-arranged access -- not a genuine cross-user
path needing a check. An `ask` match is a denial for the same reason as
12-b: 12-d's approval flow doesn't exist yet, so there is no one to ask.

Fixing the two pre-existing `-L` tests to write an ACL granting
`key:testuser write` (`write_key_acl()` in `test_net_proxy.c`) surfaced the
new fail-closed behavior working as intended: both had denied the
previously-unrestricted "testuser" identity as soon as the check went live,
which confirmed the wiring rather than exposing a bug.

Verified: `make run-tests` (all suites, 0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_net_proxy` and `test_proxy`), and the
musl static build.

Not done, deferred rather than folded in here: the per-key `role=`
annotation on `src/libnet/keystore.c`'s `authorized_keys` lines, which
would let a specific distributed key be pre-limited below what the
session's ACL otherwise grants. That is a second, independent clamp
layered on top of what this step wires up, not a prerequisite for it, and
belongs with whatever step first needs to hand out a view-only key (most
likely 12-d or a follow-up once pending clients exist to make the
distinction matter).

**Step 12. Pending clients, the indicator, and the audit trail end to end.**

Split into sub-steps, one commit each, for the same reason 12-a/b/c were:
this bundles several independent pieces (roster visibility, live
re-evaluation, pending admission, the indicator) that each stand on their
own and are each easier to get right and test in isolation.

**12-d-a. Broker clients in the session-wide roster. (DONE)**

`sessdir_client_register()` (`src/libsessdir/sessdir_control.[ch]`) was
only ever called by `attach.c`, by design: "remote clients have no local
session directory to write into, so they are simply absent from it." That
was true before a broker existed to write on their behalf. Both `-L`
brokers (`src/cmd/proxy/proxy.c`'s `broker_serve_client()`,
`src/cmd/net-proxy/net_proxy.c`'s `serve_one_client()`) run as the session
owner already, so each now registers its own relayed client into the
roster the same way `attach.c`'s `client_roster_update()` does -- right
after the ACL decision, using the same `client_id` (this child's own pid)
already sent in the `IpcAttach` -- and unregisters on every exit path
(each of `net_proxy.c`'s three failure returns plus its normal one;
`proxy.c`'s single `proxy_run()` call site).

Added a `who` field to `struct sessdir_client` (the uid or key identity,
distinct from the display `name`; empty for the tier-0 `attach.c` case,
which is always the owner's own uid and has nothing more specific to say)
and a new `WHO` column to `lumi share -l`'s listing. Old roster entries
written before this field existed decode it as empty, and old readers of
the `WHO=` line simply see an unrecognized key, so the roster file format
stayed forward- and backward-compatible with no version bump. `mode` is
left as `"-"` for a broker-registered entry, since the broker relays raw
wire bytes and has no way to know the far client's screen/turbo/minimal
display mode. `share.c`'s size column also now prints `-` instead of
`0x0` for an entry with no known size, which a broker-registered entry
always has (the broker relays multiple windows, not one size).

Test: `test_proxy.c`'s view-ceiling test now also asserts the connected
client appears in `sessdir_client_list()` with `role=view` and a
`uid:`-prefixed `who`, and that the entry is gone again (polled, since the
served connection is a separate forked grandchild from the process the
test's `broker_stop()` tears down) once the connection closes.
`test_net_proxy.c`'s concurrent-clients test asserts both simultaneous
connections appear as two distinct roster entries rather than one
clobbering the other, since they share the same `who` (`testuser`) but
must not share a `client_id`.

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_proxy`/`test_net_proxy`/`test_sessdir`),
and the musl static build.

Done when: a foreign uid or netchan key attached through either broker
shows up in `lumi share -l` while connected, and is gone from it once it
disconnects.

**12-d-b. Live ACL re-evaluation of already-connected clients. (DONE)**

An ACL change re-evaluates live connections, not just new ones. Both
brokers already had a `sessdir_watch_start()`/reconcile pair for their own
connection's mserver window discovery (`proxy.c`'s `on_watch()`,
`net_proxy.c`'s `reconcile_mservers()`); each now also re-runs
`sessdir_access_check()` for its own identity whenever the watch reports a
change, via a new `recheck_acl()` in both files. A downgrade re-requests
the role on every `pconn` with the existing client-to-server
`IPC_MSG_ROLE_REQUEST` (already wired since step 5's attach handshake and
step 8's handoff; no new wire message needed) -- `net_proxy.c`/`proxy.c`'s
own responsibility ends at sending that request, since actual enforcement
of a downgraded role is mserver's `IPC_MSG_ROLE_REQUEST` handler, already
covered by `test_mserver.c`. A full revocation stops the bridge loop,
which falls through to the same teardown path an ordinary client
disconnect already uses. Only acts when the granted role actually
changes from the last decision (tracked per-connection), so an unrelated
write elsewhere in the session directory does not spam the audit log or
re-request a role that never changed.

Two real bugs surfaced writing this, neither in the new code itself:

`sessdir_watch`'s inotify mask was `IN_CREATE | IN_DELETE | IN_MOVED_FROM
| IN_MOVED_TO` -- entry add/remove/rename only. Rewriting `<session>/access`
in place (`fopen(path, "w")`, the normal way any tool would edit an ACL)
touches none of those; the watch never fired for it at all, silently
defeating this entire sub-step until caught by testing. Fixed by adding
`IN_CLOSE_WRITE`. The kqueue side has the same gap and no equivalent fix:
`NOTE_WRITE` on a directory fires for entry changes, not for a write to an
existing child file's data, and catching that would mean watching each
file of interest individually, which this generic directory watcher does
not do. Flagged in a comment as untested and unresolved on BSD/macOS,
matching the precedent already set by step 11's untested routing
assumption on those platforms, rather than assumed fixed by NOTE_WRITE
alone.

`sessdir_client_prune()` (called by every `sessdir_cleanup_stale()`,
which every attach and rescan runs, including from unrelated sibling
processes) treated a roster directory that exists but has no readable
`info` file yet as debris and deleted it immediately.
`sessdir_client_register()` creates the directory before it writes and
renames `info.new` into place, so a narrow window exists where the
directory is visible to a concurrent reader but not yet populated. Before
12-d-a, nothing raced this in practice; two `-L` broker children
registering their own, unrelated clients within milliseconds of each
other (12-d-a's new roster-registration test, and this step's live-ACL
tests, both trigger it directly) can and did lose a client's roster entry
mid-registration. Fixed by giving a young, unreadable directory a 5-second
grace window before treating it as genuine debris.

Test: `test_net_proxy.c` gained `test_listen_acl_live_revoke` (a write
grant flipped to deny mid-connection disconnects the client, verified by
a subsequent round trip getting no reply) and
`test_listen_acl_live_downgrade` (a write grant flipped to view
mid-connection sends `IPC_MSG_ROLE_REQUEST(view)` to the mserver,
verified via a log file the test's fake mserver fixture appends to on
receiving that message). Also gave every `-L` test in the file its own
disjoint 1000-port range instead of each independently computing
`base + getpid() % N` with overlapping ranges -- harmless before this
step added two more tests to the file, but with five `-L` tests now
sharing one process's pid, two could coincide on the same port, and
`SO_REUSEPORT` lets that fail silently (a still-tearing-down listener
from an earlier test stealing a later test's connection) rather than as
a loud `bind()` error.

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean), the musl static build, and 20+ repeated runs
of `test_net_proxy`/`test_proxy` with no flakes (both new races were
intermittent, not deterministic, so a single clean run proves little).

**12-d-c. Pending (`ask`) admission. (DONE)**

An `ask` match is admitted rather than denied: registered in the roster
(12-d-a) as pending, counted in the presence indicator, and sent no
replay and no output until the write-token holder (or an owner client if
there is no writer) approves it. No one available to approve, or a
timeout, is still a denial -- fail-closed rule 3 stays the default, `ask`
just gets a real chance to be answered first instead of always losing.

Added `int pending` to `struct sessdir_client` and two new
`sessdir_ctl_post()` verbs, `SESSDIR_CTL_APPROVE` and `SESSDIR_CTL_REJECT`
(`src/libsessdir/sessdir_control.[ch]`), addressed to the pending client's
roster id. `lumi share` gained `-a id` and `-d id` to post them, mirroring
the existing `-g`/`-r`/`-k` pattern. A new `approver_available()` in both
brokers checks for a write-token holder or any tier-0 (`who` empty)
roster entry before admitting as pending at all; with no one who could
plausibly answer, the connection is denied immediately rather than left
to time out for nothing. `PENDING_TIMEOUT_S` (60s) denies a pending
connection that gets no answer either way.

The two brokers hold a pending connection open differently, matching how
each already structures its main loop. `net_proxy.c`'s `bridge_loop()` is
event-driven but tolerates zero attached mservers on every iteration
already, so a new `pending_tick()` is simply called every iteration
(gated on `pending_state`), and `reconcile_mservers()` gains an early
`if (pending_state) return;` so nothing tries to attach mservers or send
output for a client not yet approved -- `recheck_acl()` still runs
unconditionally, since a pending client's ACL rule being edited to an
outright `deny` while it waits should still cut it off. `proxy.c`'s
`proxy_run()` has a hard `pconn_count == 0` bail-out near its top, so
nothing could be deferred inside it; `broker_serve_client()` instead runs
a wholly separate `broker_wait_for_approval()` with its own dedicated
`iox_loop` (client-disconnect, `SESSDIR_CTL_APPROVE`/`REJECT`, and a
one-shot `iox_timer_add()` timeout, all as watch/fd callbacks) to
completion before `proxy_run()` is ever called. Both brokers recompute
`granted = dec.role` from the original ACL rule on approval rather than
trusting anything from the approval message itself -- nothing renegotiates
a higher role than the rule that admitted the connection in the first
place.

One real bug surfaced by stress-testing, not by a single clean run:
`recheck_acl()` (added in 12-d-b) special-cased `dec.ask` as an automatic
deny, on the reasoning that an `ask` rule was never actually a grant.
That was correct before this step, when `ask` only ever meant "deny, but
log it differently." Once approval could grant a role without editing the
ACL file, the ACL rule producing an approved connection still literally
says `ask` -- so the very next watch wakeup after an approval (any
unrelated write to the session directory triggers one) re-ran the
`dec.ask` special case and immediately revoked the connection it had just
approved, tripping fail-closed rule 3's spirit for exactly the client it
was supposed to protect. `dec.ask` is only meaningful as an initial
admission signal; a connection already admitted must be re-evaluated on
`dec.role` alone, the same as a live grant. Fixed by dropping the
`dec.ask` special case from `recheck_acl()` in both files. A single test
run of the new pending tests did not catch this: it only manifests when a
watch wakeup happens to land between an approval and the client's next
`reconcile_mservers()`/`bridge_loop()` iteration, which the first several
runs did not trigger. 30 repeated runs after the fix, 0 failures (roughly
40-60% flake rate before it, once actually looked for).

A pending roster entry's `role` column reads `"ask"` instead of `"-"` (a
one-line, low-risk change kept out of scope of 12-d-d's fuller column
work) so `lumi share -a`/`-d` has something to target from `-l`'s output
without waiting on that sub-step.

Test: `test_net_proxy.c` gained `test_listen_acl_pending_no_approver`
(no token holder and no owner-tier roster entry denies immediately, no
approval possible) and `test_listen_acl_pending_approve` (a token holder
present, an `ask` match connects, confirms nothing arrives within an
800ms window -- fail-closed rule 5 -- then posts `SESSDIR_CTL_APPROVE`
and confirms the connection completes and the roster entry clears
`pending`). `test_proxy.c` gained the equivalent
`test_broker_pending_no_approver`/`test_broker_pending_approve` pair.

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_proxy`/`test_net_proxy`/`test_sessdir`),
the musl static build, and 20 (`test_proxy`) / 15 (`test_net_proxy`)
repeated runs with no flakes after the `recheck_acl()` fix.

**12-d-d. The indicator. (DONE)**

The presence marker (`share:1w+2v`, `attach.c`'s `share_marker`, prepended
to the taskbar row the same way as the `[!watch]` degraded-watch marker,
independent of `taskbar.format`) already existed going into this
sub-step, from earlier shared-attach work; what it was missing was 12C's
"distinct color when any attached client is not the session owner's uid"
and the last two `lumi share -l` columns.

`share_marker_update()` (`attach.c`) now also computes a new
`share_marker_foreign` alongside the marker text: for a local
(tier-0-owned) session it scans the roster for any entry with a non-empty
`who` (12-d-a's signal for "broker-relayed," i.e. not this uid); for a
client that reached its own session through a broker (`is_remote`), it is
unconditionally set once the marker is shown at all, since a remote
attach is itself a foreign uid by definition and has no roster read
access to check anyone else's. `render_taskbar()` (`attach_ui.c`) wraps
just the marker text in a fixed `txl_setaf` color (yellow) when the flag
is set, leaving the `[!watch]` marker and the rest of the taskbar format
uncolored. The prefix's byte length (which now differs from its display
width once it carries color escapes) is tracked separately from the
plain-text display width used to size the format-driven remainder, so
the color escapes cannot eat into the columns budgeted for
`taskbar.format`'s own expansion. Not configurable, matching every other
part of this indicator: `share.indicator` changes how the marker reads,
not whether it, or now its color, appears.

Added a `coupling` field to `struct sessdir_client`
(`src/libsessdir/sessdir_control.[ch]`), persisted the same way as
`mode`. A tier-0 attach.c client sets it to `mirror` or `free` for a
view-role client (whether `IPC_ATTACH_F_MIRROR` is set, from step 10's
`-v`/`-F`), or `-` for a write-role client, since coupling is a property
of a viewer following someone, not of the one being followed. Both
brokers set it to `-` unconditionally at every registration site, the
same reasoning already applied to `mode`: a broker relays raw wire bytes
and has no way to know whether the far end's own attach.c is mirroring.
`lumi share -l` gained `COUPLING` and `PENDING` columns (the latter
`yes`/`-` from 12-d-c's roster field, printed directly rather than
inferred from the `ask` role string that also appears while pending).

Verified end to end by hand rather than only by unit test, since the
color escape and the render path it touches have no automated coverage
(no existing test exercises `attach_ui.c`'s taskbar rendering at all):
built a real session, a real netchan-enrolled key, and a real
`lumi net-proxy -L` broker under GNU screen with the raw output captured
through `script` (see the verify-TUI recipe). Confirmed the SGR sequence
wraps exactly around `share:1w+1v ` and nothing else, confirmed
`lumi share -l` shows `WHO=key:testuser ROLE=view MODE=- COUPLING=- `
for the broker-relayed viewer and `WHO=- ROLE=write MODE=screen
COUPLING=-` for the local owner, and confirmed switching the ACL rule to
`ask` shows `PENDING=yes` until `lumi share -a` clears it back to `-`
with `ROLE` flipping from `ask` to `view`. This also incidentally
reproduced the `[!watch]` marker's own documented inotify-exhaustion
scenario firsthand: the host had 124 of 128 system-wide
`fs.inotify.max_user_instances` in use from ordinary desktop
applications, which made `test_proxy`'s pending-approval test fail
consistently (`inotify_init1` returning `EMFILE`, confirmed with
`strace`) until the limit was raised. Not a code defect in this step or
in 12-d-c; noted here since it is the same failure mode the `[!watch]`
marker exists to make visible, just caught from the operator's side of
the system instead of the session's.

Test: no new automated test (see above); the existing roster and pending
tests already exercise `coupling`'s write path indirectly through
`struct sessdir_client`'s round-trip, and the manual verification above
covers the parts nothing automated reaches.

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_proxy`/`test_net_proxy`/`test_sessdir`),
the musl static build, 20 (`test_proxy`) / 15 (`test_net_proxy`) repeated
runs with no flakes, and the manual end-to-end TUI verification above.

Done when: another local user can be granted view access, is prompted for,
is visible in the indicator, appears in the audit log, and is cut off the
moment the ACL changes.

---

**Step 13. Multi-writer (12B).**

Split into sub-steps, one commit each, for the same reason 12-d was: this
bundles several independent pieces (mode setting, input atomicity, size,
layout write conflicts, speculative echo) that each stand on their own and
are each easier to get right and test in isolation.

**13-a. `share.mode` and the mserver relaxation. (DONE)**

New `<session>/control` (`src/libsessdir/sessdir_control.[ch]`), a plain
`MODE=single-writer`/`MODE=multi-writer` settings file distinct from
`control.lock` (the keyboard token) and `control.msg` (the request/
approval mailbox) despite the similar name -- this one is neither a lock
nor a mailbox, just session-wide settings every mserver and client needs
to agree on. Missing, unreadable, or unrecognized content reads as
single-writer, the same fail-safe-default precedent `sessdir_access_check()`
already set for a missing ACL. `lumi share -M single-writer|multi-writer`
sets it, mirroring the `-u`/`-G`/`-B` pattern already there for the broker.

`mserver.c`'s `role_for()` (the function step 6 built: "grants WRITE to
the first connection that asks, VIEW to the rest") grows a multi-writer
branch: in multi-writer mode, any connection not explicitly asking for
VIEW is granted WRITE outright, skipping the "someone already holds it"
loop entirely, so nobody already typing is displaced by a new asker.
`IPC_ATTACH_F_TOKEN`'s forced-handoff path (used by `lumi share -g`) is
untouched in this sub-step -- claiming the token still demotes whichever
connections currently hold WRITE, even in multi-writer mode. Whether that
is the right interaction for an *explicit* handoff request or should be
revisited stays open for a later sub-step if testing surfaces a reason to
change it; nothing so far has.

The mode is read fresh (stat + read, no caching) on every `role_for()`
call rather than cached or watched, and this was a deliberate choice, not
an oversight: `mserver.c` had *no* `sessdir_watch_start()` call anywhere
before this sub-step, unlike `attach.c` and the two `-L` brokers which
already watch the session directory each for their own reasons. Since
each window is its own mserver process, adding a live watch here would
mean one more inotify instance per *window*, on top of what this same
session's 12-d-d work already found the local machine sitting close to
capacity on (`fs.inotify.max_user_instances`, ordinary desktop use, no
relation to lumi). A per-attach check needs no watch and satisfies "reads
it at startup and on change" for the case that matters -- a *new*
attach always sees the current mode. The tradeoff, decided rather than
discovered by accident: an already-connected VIEW client is **not**
promoted automatically the moment a live session flips to multi-writer.
It keeps watching until it reattaches or is handed the keyboard with
`lumi share -g`. Revisit only if that turns out to matter in practice.

Confirmed while reading the existing code for this sub-step that two of
FUTURE.md's five 12B bullets needed no new code at all:
`resize_to_fit()` (step 10) already iterates every connection regardless
of role, filtering only on `IPC_ATTACH_F_SIZE_OBSERVE`, so multiple
WRITE-role connections already negotiate size to the minimum correctly.
Only a test was added (`test_multi_writer_size`), not production code.

Test: `test_mserver.c` gained `test_multi_writer_mode` (two connections
both request WRITE under multi-writer mode; both get it; the first is
never sent an `IPC_MSG_ROLE_CHANGE`; both can actually type and see their
own output) and `test_multi_writer_size` (two WRITE-role connections at
different sizes negotiate to the minimum, same as the existing
writer-plus-viewer case). `test_sessdir.c` gained `test_share_mode` (the
get/set round-trip, including the "no file yet" and "unknown session"
defaults).

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_mserver`/`test_sessdir`), the musl
static build, 20 repeated runs of `test_mserver` with no flakes, and a
manual end-to-end check under GNU screen: two local `lumi attach`
clients to one `multi-writer` session, both typing into the same shell,
neither demoted, `lumi share -l` showing both as `write`, and the
presence indicator correctly reading `share:2w+0v`.

**13-b. Input run bracketing. (DONE)**

`write_to_pty()` already loops over partial writes for one message's
buffer, so a single `IPC_MSG_INPUT` is always written atomically -- the
gap was between messages. `attach.c`'s bracketed-paste path sends the
`\033[200~` marker, the paste body, and `\033[201~` as three separate
INPUT messages; in single-writer mode nothing else could land between
them, but in multi-writer mode another writer's own keystroke could be
processed by the mserver's single-threaded event loop in between and get
written to the PTY mid-bracket, splitting the very thing bracketed paste
exists to protect against.

Added two new message types, `IPC_MSG_INPUT_BEGIN`/`IPC_MSG_INPUT_END`
(`src/libipc/ipc_msg.h`, `0x0104`/`0x0105`, alongside `IPC_MSG_INPUT`).
`struct mclient` (`mserver.c`) gained a per-connection accumulator
(`inq`/`inq_len`/`inq_cap`, growable the same way `outq` already is) and
an `inrun` flag. `INPUT_BEGIN` opens a run (resetting any stale one --
never expected from a well-behaved client, but the safe response to a
protocol violation is starting fresh, not concatenating unrelated data);
`INPUT` while a run is open appends to the accumulator instead of writing
immediately; `INPUT_END` flushes the whole accumulated run to the PTY in
one `write_to_pty()` call and closes it. A bare `INPUT` with no open run
is unchanged: written immediately, exactly as before. `mclient_disconnect()`
frees and clears the accumulator (`mclient_reset_inq()`), so a client that
dies mid-paste cannot wedge its own future input, matching the precedent
`mclient_reset_queue()` already set for the output side. A run capped at
4 MiB (`INRUN_HARD_LIMIT`, mirroring `mclient_enqueue()`'s own hard limit
on an output backlog) drops the connection rather than growing without
bound -- a run that large is misbehaving, not just a big paste. Both of
`attach.c`'s `msg_mutates()` (client-side) and `mserver.c`'s own copy
gained the two new types, so a view-only client is refused them the same
way it is already refused plain `INPUT`. `sel_paste()`'s three existing
sends are now wrapped in `INPUT_BEGIN`/`INPUT_END`.

Verified the test actually catches the regression it targets, not just
that it passes: temporarily forced `inrun` to stay `0` (simulating no
bracketing) and confirmed the new test failed with exactly the corruption
it is meant to catch, then reverted and confirmed it passes again.

Test: `test_mserver.c` gained `test_input_run_bracketing` -- one
connection opens a run and sends an incomplete command line, a second
connection (both hold WRITE under multi-writer mode) sends an ordinary
complete command in between, the first connection finishes its line and
closes the run. Asserts the second connection's command executed cleanly
on its own, and the first connection's whole line reached the PTY as one
intact command afterward, rather than the two interleaving into a single
torn line (`echo LUMI-Aecho LUMI-B`, which is what the pre-fix behavior,
confirmed above, actually produces).

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` (clean, `test_mserver`), the musl static build, 20
repeated runs of `test_mserver` with no flakes, and a manual check under
GNU screen confirming ordinary interactive typing from two simultaneous
writers in a multi-writer session is unaffected by the new dispatch code
(the bracketing path itself is exercised end-to-end by the unit test,
which drives the real wire protocol rather than calling internal
functions directly).

**13-c. Size negotiation.** Folded into 13-a's own test coverage above --
the production code was already correct, so there is no separate
sub-step left to do here.

**13-d. Layout generation counter and safe writes. (DONE)**

`sessdir_layout_save_screen()`/`_save_turbo()` used to do a plain
`fopen(path, "w")` -- no temp-plus-rename, no generation counter. This had
never mattered before, since `mirror_publish()` only runs the save for
the sole writer (`client_role == IPC_ROLE_WRITE`); multi-writer plus
`share.display = shared` means more than one process can hit this path.

Added a monotonic generation number to the layout file format
(`GEN=` line, `sessdir_layout.c`), written via temp-plus-rename
(`<session>/layout.new` renamed onto `<session>/layout`, matching
`sessdir_client_register()`'s and `sessdir_ctl_post()`'s existing pattern
in this same library) so a reader never sees a torn write.
`sessdir_layout_generation()` reads it back (0 if there is no file or no
`GEN=` line -- a save always writes 1 or higher, so 0 never collides with
a real generation). A writer (`mirror_publish()` in `attach.c`) reads the
current on-disk generation before saving and re-imports
(`mirror_apply_layout()`) if it has changed since its own last known one,
rather than blindly clobbering a more recent change it never looked at.
Last-writer-wins on content is still fine -- this only has to stop a
writer from silently discarding a change it never read, not resolve a
real conflict, and the writer's own pending change is exported fresh
regardless of the catch-up.

`share.display = shared|independent` (`sessdir_control.c`, stored
alongside `share.mode` in the same `<session>/control` file, a
`DISPLAY=` line) lands here too, exposed as `lumi share -D`. `independent`
(the default) means a write-role client never applies another writer's
layout or focus changes to itself, though it still publishes its own;
`shared` means every write-role client follows the others' layout and
focus the same way a mirror-coupled view-role client already does
(`mirror_following()` in `attach.c` now also returns true for a
WRITE-role client when `mode == multi-writer && display == shared`).
Unlike `share.mode`, this takes effect live for an already-attached
client, not just the next attach -- `mirror_following()`/
`mirror_display_shared()` read the control file fresh on every check,
and there is no promotion/demotion of a client's role involved, only
whether it follows.

Verification surfaced a real bug, unrelated to this sub-step's own
changes: `screen_export_tree()` (`attach.c`, used by `screen_save_layout()`
to build the tree written to the layout file) only allocated its return
value (`sn = xcalloc(...)`) inside the `TILE_LEAF` case. For a split tree
-- i.e. any session with more than one pane, which is exactly what
manually verifying the mirror-following behavior requires -- execution
fell through to the branch case and wrote `sn->type = ...` through an
uninitialized stack pointer, corrupting whatever heap memory that garbage
address happened to land on. Confirmed present on the last commit before
13-a (`git stash` of all 13-d changes, rebuild, same crash), so it
predates multi-writer entirely; it just took a shared multi-writer
session to notice, since that is the first scenario in this session's
testing to attach a second screen-mode client to a session with a split.
Symptom ranged from an immediate segfault (`tile_composite` reading a
freed `struct tile` through the dangling `tilemgr` global, once the
corrupted write happened to land on it) to no visible symptom at all
depending on heap layout -- confirmed with an `ASAN` build, which caught
it as a heap-use-after-free on 100% of repeated attempts once triggered,
where the plain build only crashed about half the time. Fixed by moving
the `xcalloc` above the leaf/branch split so both cases allocate.

Test: `test_sessdir.c` gained `test_share_display` (default independent,
round-trips, and does not clobber a previously-set `share.mode` or vice
versa -- the two settings share one file) and `test_layout_generation`
(generation starts at 0 for a session with no layout yet, first save is
generation 1, second save is 2 not reset, and geometry still round-trips
correctly alongside the new `GEN=` line). The `screen_export_tree()` fix
has no unit test of its own -- there is no test harness for `attach.c`'s
tiling internals in this codebase, so it was caught and verified purely
through manual reproduction under GNU screen plus an ASAN build, per the
verification notes below.

Verified: `make run-tests` (all suites, 0 failures), `tests/smoke.sh`,
`SANITIZE=address` build of `test_sessdir`/`test_mserver` (clean, 20
repeated runs each with no flakes), the musl static build, and a manual
check under GNU screen: two clients attached to one multi-writer session,
`share.display=shared` set, one client (screen mode) splits into two
panes -- the other client (turbo mode) picks up the second window in its
own taskbar; switched to `share.display=independent`, the same client
changed focus between panes -- the other client's focus stayed put this
time. Separately reproduced the `screen_export_tree()` crash under ASAN
(8/8 runs, 100% heap-use-after-free before the fix), confirmed it also
reproduces at the pre-13-a commit with no multi-writer/sharing involved
at all, applied the fix, and confirmed 8/8 clean ASAN runs afterward with
the split rendering correctly in both clients.

**13-e. Restrict speculative echo in multi-writer mode. (DONE)**

The `predict_key()` call in `attach.c` was unconditional whenever this
client sent printable input. `predict_confirm()` already rolls back a
wrong prediction on unexplained output (the existing 11D safety net), but
a prediction that is usually wrong because another writer's input keeps
interleaving is worse than not predicting at all.

Added `share_writer_count`, a cached count of "write"-role entries in the
session roster, kept in step with `share_marker_update()` (which already
scans the roster on every join/leave event and on the existing
250ms/1s polling timers) rather than rescanned per keystroke. Defaults to
1 ("just me") on every path where the roster is not visible from here --
remote sessions (only a connection count is known, not each one's role),
no session directory, or a roster of one -- so prediction stays on unless
it is positively known there is more than one writer. The `predict_key()`
call site now skips prediction outright when `share_writer_count > 1`,
same pattern `approver_available()` in the brokers and
`share_marker_foreign` (12-d-d) already used for a roster-derived gate.
This does not need a separate multi-writer-mode check: single-writer
mode can never produce more than one writer in the roster, so the writer
count alone is sufficient.

No unit test: like 13-d's `screen_export_tree()` fix, there is no test
harness for `attach.c`'s input-handling internals in this codebase (only
`predict.c`'s own library is covered, by `test_predict.c`, which this
change does not touch). Verified instead by hand.

Verified: `make run-tests` (0 failures), `tests/smoke.sh`,
`SANITIZE=address` and musl static builds (clean), and a manual check
under GNU screen: a solo writer's typing still renders normally
(prediction active, `share_writer_count == 1`); two writers under
multi-writer mode typing concurrently produced the same PTY-level
interleaving multi-writer typing is already expected to produce (13-b
only brackets a paste, not ordinary keystrokes) with no crash and no
visible corruption from local prediction now that it is gated off.

Done when: two clients can work in the same session at once without
corrupting each other's escape sequences or layout.

---

**Step 14. Documentation pass. (DONE)**

`doc/lumi.1` was found already up to date: `lumi share` (including `-M`/
`-D`/`-u`/`-G`/`-B`/`-a`/`-d`), `attach -v`/`-F`, the `share.*` config
keys, the `access` file format, and `access.log` were all documented
incrementally as each of Step 13's (and earlier steps') sub-steps landed.
No man page changes were needed for this pass.

`doc/DEV.md` predated all of Phase 12, though, and needed real additions:

- A **Client Roster and Coupling** subsection (under `### Client
  (lumi-attach)`) describing the two places client state lives -- each
  mserver's in-process `struct mclient` list versus the session-wide
  `struct sessdir_client` roster (`sessdir_client_register()` et al.) that
  `lumi share -l` reads -- and every field of the latter.
- A new **Client Roles and Coupling** section: single-writer vs.
  multi-writer semantics and the exact `role_for()` decision order
  (view request, write token, multi-writer mode, first-come-first-served),
  independent vs. shared `share.display`, how a view-role client's
  `IPC_ATTACH_F_MIRROR` coupling works via `mirror_sync()`, and the write
  token's role as an attach-time tiebreaker rather than a precondition for
  typing once multi-writer mode is on.
- An **Attach Handshake and Role Negotiation** update to the existing
  Connection Lifecycle diagram: the ATTACH/ATTACH_REPLY role fields,
  CLIENT_EVENT on join, and ROLE_REQUEST/ROLE_CHANGE for a live role
  change, plus why `IpcAttach`/`IpcAttachReply` share tags 1-2 with the
  older `IpcSize` message.
- A new **Cross-User Broker** section walking `broker_serve_client()`'s
  actual admission sequence (peer credential check, ACL check, `ask`
  pending-admission with `approver_available()`'s fail-closed rule,
  audit logging, roster registration, and the `dup2()`-onto-stdio handoff
  into the ordinary proxy engine), and how it differs from a same-uid
  client's direct connection.
- A new **Connection Types and Trust Tiers** table ranking the five
  distinct paths into a session (local same-uid, cross-user broker,
  SSH-tunneled proxy, netchan direct-connect with key auth, netchan
  direct-connect with password auth) by what authenticates the peer and
  what is encrypted.
- The **IPC Protocol** message-type tables gained every message added
  since they were last written: `ROLE_CHANGE`, `CLIENT_EVENT`,
  `ROLE_REQUEST` (0x00xx), `FLOW_CTRL`, `REFRESH`, `INPUT_BEGIN`/
  `INPUT_END` (0x01xx, 13-b), `PTY_FLAGS` (0x02xx), the attribute-store
  messages (0x03xx, previously undocumented entirely), and the proxy
  control messages (0x04xx). The microser encoding section gained
  `IpcAttach` and `IpcAttachReply`'s field tables.
- The **Architecture** diagram was replaced: it showed a single client
  and, worse, an incorrect claim that "only one client is connected at a
  time" to an mserver -- true before Phase 12, false since. The new
  diagram shows one write-role and one view-role client attached to the
  same three windows at once.

Done when: no flag, config key, or file introduced above is undocumented.
Verified by cross-checking every function name, message name, and struct
field cited against the current source (`mserver.c`'s `role_for()`,
`attach.c`'s `mirror_*` functions, `proxy.c`'s broker path,
`net_proxy.c`'s key/password checks, `ipc_msg.h`, and `lumi.idl`) rather
than trusting a first-pass draft.

### Open Questions

- **Bell and OSC.** A bell should reach every client. Clipboard sync
  (OSC 52) from the application is arguably writer-only, since a viewer
  probably does not want its clipboard replaced.
- **Host title.** Should a mirroring viewer also set its terminal title from
  the session, or keep its own?
- **`lumi kill` from a viewer.** Denied by role enforcement, but the error
  needs a clear message rather than silence.
- **Roster identity.** `user@host` is fine locally. For netchan clients the
  keystore key name is the more honest identifier.
- **Pending clients in the indicator.** Counting a pending client in the
  presence indicator tells the owner someone is knocking, which is useful,
  but it also lets any local user make a mark appear on the owner's status
  line. The rate limit bounds the nuisance; whether pending clients are
  counted separately or not at all is worth deciding with the UI in front of
  us.
- **Cross-user without a shared group.** The 0666 broker endpoint is safe
  under the credential check but still connectable by any local user. An
  alternative is passing the connected fd over an existing channel, which
  avoids a public endpoint entirely but requires a channel to already exist.
- **Owner definition.** The uid that created the session, or the uid the
  mservers run as? They are the same today. Writing it down now avoids a
  subtle divergence later.

---

## Implementation Order

```
12: Shared attach (14 steps, see the step-by-step plan above)
     - steps 1-2:   peer credentials, runtime directory hardening
     - steps 3-6:   mserver fan-out, handshake, view-only clients
     - steps 7-10:  token, share command, presence, mirror, sizing
     - steps 11-12: proxy fork per client, cross-user ACL and broker
     - steps 13-14: multi-writer, documentation
11A: State-dependent bindings  (standalone, ~3-4 days)
11B: SIXEL pass-through        (standalone, ~2-3 days)
11D: Speculative local echo    (standalone, ~4-5 days)
11C: Networked connections     (largest, ~1-2 weeks via netchan-v2)
     - ipc_transport abstraction (prerequisite)
     - libnet: extract netchan core + nc_udp + nc_crypto (Monocypher)
     - lumi-net-proxy
     - client netchan attach
```

## Bundled Utilities: file browser, editor, calculator

Three interactive utilities that run standalone and gain a few extra
bindings when launched inside a lumi session. They are ordinary
subcommands (`src/cmd/<name>/`), run as children in a window with their own
PTY and input loop. They are not overlay "apps" like `app_calc`: those live
inside the attach client's event loop and suit tiny widgets, while these are
full-screen programs that own their input. Running them as subcommands also
gives standalone use for free.

### Session enhancement mechanism

Detection is trivial: `mserver` already exports `LUMI_SESSION` into every
child environment (`mserver.c:1409`), so `getenv("LUMI_SESSION")` answers
"am I inside lumi?" with no IPC. The enhancement *actions* reuse verbs that
already exist: `new-window`, `send-keys`, `send-input`, and `selection.c`
for the shared clipboard. Each tool runs identically everywhere; when in a
session it lights up a small set of extra bindings that call those existing
paths. One shared helper module wraps "in session? fire IPC action" so the
three tools do not each reinvent it. No new protocol, one binary per tool,
a few `if (in_session)` bindings rather than a forked code path.

### Shared pieces

- `libtext`: text buffer with line index and undo (gap buffer or piece
  table). Used by the editor and by `lumi basic`'s line-numbered program
  store. Build it with the editor. Note `libtxl` is the terminal-output
  translation engine, not a text buffer; there is no existing buffer lib.
- `libtui` additions: a scrollable text viewport and a single-line input
  field, added to `libtui` proper so the tools and future overlay apps
  share them.

### 1. File browser (`lumi files`) -- navigator, not manager

Two-pane list plus preview over the existing `libtui` list/box widgets.
Navigate the tree, preview text and directories, open dispatches to the
editor or a pager. In a session, offer "open in new window" and "open in
split" via `new-window`.

Scope discipline: this stays a navigator. Mutating operations (copy, move,
delete, rename, bulk select, archive, chmod) are deferred and, when added,
come one at a time behind an explicit confirm. Built first because it is the
smallest tool and exercises the `libtui` + `tkbd` + `LUMI_SESSION` IPC spine
the other two reuse.

### 2. Editor (`lumi edit`) -- modeless first, keymap table for vi later

A joe/nano-style modeless editor on top of `libtext`. The input layer is a
keymap table (key + context -> command enum) from day one, so a modal vi
keymap can be added later as a separate layer without touching the buffer or
the command set. A full vi personality (modes, operators x motions x counts,
registers, `:ex`) is explicitly out of scope for the first cut, and even
when added it stops at motions plus a handful of operators, not vim
completeness. In a session: "send region to the shell pane" via
`send-input` to another window.

### 3. Calculator (`lumi basic`) -- Tiny BASIC with vector/matrix values

Named `lumi basic` to avoid colliding with the `app_calc` overlay, which
keeps the "calc" name. A Pratt parser drives an immediate/expression mode; a
line-numbered program store (`LET/PRINT/IF/GOTO/GOSUB/FOR/INPUT`) sits on
top. `DEF FN` supported. ASCII-art graphing samples a function or a vector
across terminal columns and plots into a `vt_cell` grid.

Values: scalar, one string type, and first-class vector/matrix *literals*
(`[1,2,3]`, `[[1,2],[3,4]]`) rather than general N-D arrays. Two guardrails
keep it a calculator and not a language runtime:

- Value semantics, no LHS indexing. `v(i)` reads an element; there is no
  `v(i) = x`. Vectors and matrices are built from literals or builtins
  (`zeros(n)`, `range(a,b)`). Immutable values avoid `DIM`, aliasing,
  bounds bookkeeping, and `GOSUB`-scope interactions.
- `+ - *` are elementwise with scalar broadcast. Real linear algebra lives
  in named builtins (`dot()`, `matmul()`), never in an overloaded `*`.

No file I/O, no `EVAL`. It stays a calculator you can script.

### Build order

```
1: lumi files                        (proves the libtui + tkbd + IPC spine)
2: libtext + lumi edit               (modeless, keymap-table input)
3: lumi basic                        (Pratt -> immediate -> programs -> graph)
later: vi keymap layer on the editor
later: mutating ops in the browser, behind confirm
```

### Status

- `lumi files` shipped standalone: navigation, a preview pane (text peek,
  directory listing, binary note) with display-width-correct clipping, and
  open in `$PAGER`/`$EDITOR` by suspending and resuming the TUI. In-session
  open in a window now works too: inside `lumi`, `o` opens the selected file
  in `$PAGER` and `O` in `$EDITOR`, each in a fresh window of the current
  session. This landed once `new-window` and mserver learned to run a command
  line: `new-window` forwards its trailing args, mserver's `+s:` getopt stops
  option scanning at the first non-option and `window_new` execvp's the given
  argv (default shell when none). Bracketed paste is enabled and swallowed:
  the browser has no text field, so a stray paste is discarded rather than
  read as a run of navigation keys. The browser enhancements shipped: `.`
  toggles dot (hidden) files in the listing (reloading in place and keeping
  the selection), Ctrl-U/Ctrl-D scroll the preview pane by half a screen
  (reset when the selection moves, clamped to the built content), and three
  mutating operations landed behind a prompt or confirm: `m` makes a
  directory, `r` renames the selection (a text prompt pre-filled with the
  current name), and `d` deletes it after a yes/no confirm (directories only
  when empty). New names may not contain `/`, so an operation stays in the
  current directory, and a reload follows each, keeping the affected entry
  selected. The prompt and confirm helpers mirror the editor's. Verified live:
  the toggle, half-page preview scroll and clamp, and mkdir/rename/delete each
  reflected on disk.
- `tui_out` (in libtui) holds the shared output buffer, SGR helpers, and the
  display-width field writer used by `lumi files` and `lumi edit`, and by
  `lumi basic` when it lands.
- `libtext` shipped: line buffer with load/save (trailing-newline faithful),
  insert/delete/split/join, a dirty flag, and a valgrind-clean test suite.
- `lumi edit` shipped: open, a modeless keymap-table editor with scrolling
  and tab-expanded rendering, UTF-8-aware editing, save (Ctrl-S, with a
  save-as prompt), quit-if-modified confirm, and undo/redo (Ctrl-Z/Ctrl-Y,
  with typing coalesced into one step). libtext carries the undo stacks.
  Bracketed paste (DECSET 2004) is enabled: pasted text is inserted
  literally, control bytes in the paste never fire commands, and CR, LF, and
  CRLF all collapse to one newline. Selection and an internal clipboard
  shipped too: Shift plus a movement key extends a reverse-video selection,
  Ctrl-C copies (or copies the current line when nothing is selected), Ctrl-X
  cuts, and Ctrl-V pastes, with editing or pasting over a selection replacing
  it. Ctrl-C is now copy rather than a quit alias; Ctrl-Q remains quit.
  Copy and cut also mirror the span to the system clipboard via OSC 52
  (capped at 100 KB, best effort); the reverse direction is covered by
  bracketed paste, so no OSC 52 read is needed. Inside a session the attach
  client forwards a window's OSC 52 set-clipboard request out to the outer
  terminal (in osc_passthru, next to the OSC 9/99/777 notification and OSC
  10/11 color forwarding), so an in-session copy reaches the real system
  clipboard; read requests are dropped. The VT parser's OSC buffer now grows
  on demand (like its DCS buffer) up to a 256 KB cap, so it forwards
  clipboards of roughly 190 KB rather than truncating at the old fixed 4 KB;
  small OSCs such as titles still use a 256-byte buffer. Search and goto-line
  shipped: Ctrl-F prompts for a string and jumps to the next match, searching
  forward from the cursor and wrapping to the top (case-sensitive, byte-based
  strstr per line); the prompt pre-fills the last query, so Enter repeats the
  search. Ctrl-L prompts for a 1-based line number and jumps there, clamping
  past the end to the last line. Both reuse the existing status-line prompt
  (prompt_line now edits its buffer in place so a default can be pre-filled),
  and both run from the main loop like save, where the key stream is available.
  Verified live: find reports the match line, repeat wraps, goto and its
  clamp land on the right line. A joe/nano-style help screen shipped too: F1
  shows a full-screen list of the key bindings (drawn from a table kept next
  to the keymap) and any key returns to editing, and the status bar shows an
  "F1 for help" hint when the editor opens. Verified live: the hint appears,
  F1 lists the bindings, and a keypress returns to the buffer. Not yet done:
  the vi keymap layer.
- `libbasic` and `lumi basic` shipped B1 through B4: a Pratt expression
  engine (variables, builtins, comparisons), an immediate-mode REPL (PRINT,
  LET, bare-expression calculator mode), line-numbered programs with
  LIST/NEW/RUN and control flow (GOTO, GOSUB/RETURN, FOR/NEXT/STEP,
  IF...THEN, INPUT, END), and first-class vector and matrix values
  (elementwise ops with scalar broadcast, 1-based read-only indexing,
  ZEROS/RANGE/DOT/MATMUL/LEN), user functions (DEF FN), and ASCII graphing
  (PLOT of a function over a range, or of a vector). The calculator is
  feature-complete for its scope. It reads cooked line input, so pasting a
  multi-line program already works; leaked bracketed-paste markers (from a
  full-screen app that left DECSET 2004 on) are stripped from each line.
  Program management commands were added as REPL-frontend commands (so
  libbasic stays free of file I/O): `NEW` (already present) clears the
  program, `SAVE`/`LOAD` write and read it back reusing the round-trippable
  `LIST` output and line storage, and `EDIT` dumps the program to a temp
  file, opens it in `lumi edit` full-screen, and re-imports it on exit. The
  `EDIT` route is the "hybrid" chosen over a faithful screen-workspace REPL:
  it gives edit-any-line full-screen editing by reusing the editor, keeps
  editing and execution separate (no live sync between an editable screen and
  the program store, and no PRINT-output-placement problem), and leaves the
  pipe-friendly line REPL intact for non-TTY use. Verified end-to-end: a
  program edited in `lumi edit` (a line added) ran with the new line after
  return.
- `lumi send-input` shipped (option A). It injects raw bytes into a session's
  focused window: bytes come from the arguments (joined with single spaces) or
  from standard input, and go out as one bracketed INPUT_BEGIN/INPUT/INPUT_END
  run, so the mserver flushes them to the PTY as a single write that cannot
  interleave with another writer. The client work is small and reuses the
  existing spine: connect to the focused window's socket like `lumi attr`
  (`sessdir_state_focus` + `sessdir_server_path` + `ipc_connect`), do the
  ATTACH handshake, then send the run. Two details matter. It attaches with
  `IPC_ATTACH_F_SIZE_OBSERVE` so the transient client never resizes the
  window (`resize_to_fit` skips observers). And after sending it does
  `shutdown(SHUT_WR)` and drains the server's replay before closing: closing
  outright makes the server's replay write to us fail, and it then drops the
  client before flushing the input run to the PTY. Option A means it does not
  steal the keyboard: `role_for` grants WRITE only when no other client holds
  it, so an injection while a human is attached in single-writer mode is
  refused with a clear "read-only" message; a detached or multi-writer session
  proceeds. Verified end-to-end (a shell ran the injected command; valgrind
  clean) and the denial path (a human attached blocks it).
- `lumi send-keys` shipped. It translates key names (`Enter`, `Tab`,
  `Escape`, `Space`, `BSpace`, arrows, `Home`/`End`, `PPage`/`NPage`,
  `IC`/`DC`, `F1`-`F12`), modified keys (`C-a` -> a control byte, `M-x` ->
  Esc-prefixed), and literal text into bytes, then sends them through the
  same `lu_send_input()` core with the same `-s`/`-w`/`-o` targeting; `-l`
  forces every argument literal. Unrecognized arguments are sent verbatim,
  as tmux does. Verified byte-for-byte (arrows, `C-a`/`C-x`/`M-a`) and
  functionally (text + `Enter` runs a command); valgrind clean.
- `ipc_client_attach()` (in `ipc_msg.c`) now holds the client-side ATTACH
  handshake -- send ATTACH, read past anything queued ahead of the reply,
  return the granted role -- and both `send-input` and `lumi attr` use it.
  This fixed `lumi attr`, which never attached and so had every request
  refused by the mserver (first message must be ATTACH); it now attaches with
  `IPC_ATTACH_F_SIZE_OBSERVE` before its attribute txn. Its synchronous
  `recv_expect` also learned to skip the screen replay and other messages the
  server queues right after ATTACH, rather than mistaking the first replay
  frame for its reply.
- Window-by-index targeting shipped for `send-input` and `send-keys`: `-i N`
  sends to the window numbered N, the stable per-window number the status/tab
  bar shows and the prefix-plus-digit selects (0-based). The number is not the
  arbitrary `sessdir_list_servers` order; it comes from the session state's
  slot map via a new `lu_window_pid()` (in `send_input.c`, declared in
  `multicall.h`) that reads `sessdir_state_nums()` (slot index = number, 0 =
  empty) and returns the server pid, or 0 when the number is out of range or
  empty. Each command resolves `-i` to that pid before calling the unchanged
  `lu_send_input()`, so `-w`/`-i`/`-o` are alternatives with `-i` overriding.
  Verified live on a detached three-window session: `-i 0/1/2` each landed its
  marker in the matching window, `-i 9` errored, and (with a human attached)
  the resolved window was correctly refused as read-only under the Option-A
  single-writer rule.
- Deferred still: an opt-in `--steal`/token flag to take the keyboard when a
  human holds it (attach with `IPC_ATTACH_F_TOKEN`, which `role_reassign`
  returns on our disconnect).
- Send-to-pane shipped in both tools. The shared piece is `lu_send_input()`
  (in `send_input.c`, declared in `multicall.h`): it resolves a target window
  (a server pid, the focused window, or a non-focused "other" window honoring
  `$LUMI_SEND_TARGET`), then runs the same attach + bracketed-run + drain path
  as the command, returning a result code instead of printing so a TUI caller
  can turn it into a status line. `lumi send-input` gained `-w pid` and `-o`
  over it. In `lumi edit`, Ctrl-G sends the selection (or the current line)
  to another pane with a trailing carriage return so it runs there. In `lumi
  basic`, `SEND <expr>` evaluates the expression (by capturing a `PRINT` into
  an `open_memstream` buffer, so libbasic stays a pure interpreter) and sends
  the value. Both target `-o` (the pane beside the tool) and both are
  in-session only. Verified end-to-end: `-w` delivery, `basic SEND 6*7`
  landing `42` in another window, and edit's Ctrl-G sending the current line;
  valgrind clean.
- `lumi basic` moved off BASIC line numbers to a QBasic-style model (chosen
  as "Model B" over a faithful inline retro workspace, which was rejected as
  large effort for mostly-aesthetic payoff since `EDIT` already gives
  full-screen editing). The program store is now a positional list of source
  lines in file order, not a sparse array keyed by line number. Three parts:
  (1) The REPL is nearly unchanged. A line that starts with a number now
  edits *that file line* (1-based) rather than defining a sparse line number:
  `1 PRINT X` sets the first line, a bare number deletes that line, and typing
  past the end appends. Immediate statements still run as before.
  (2) Jumps are label-only. `GOTO`/`GOSUB` take a label, never a line number,
  to avoid confusing the jump target with a file line. A label is `name:` at
  the start of a line, alone or in front of a statement (`loop: PRINT X`),
  matched case-insensitively. Numeric `GOTO 100` is gone. `IF ... THEN` still
  runs any statement (including `GOTO`/`GOSUB label`), and additionally treats
  a lone token that names a defined label as a jump (`IF x THEN loop`), the
  replacement for the old `IF x THEN <line>` shorthand. This full-statement
  plus label-shorthand form was kept deliberately over a stricter label-only
  `THEN`; it can be tightened later if it proves troublesome.
  (3) `LIST` shows a line-number gutter for the human (so you know which `N`
  to edit), but `SAVE` and `EDIT` write raw source through a new
  `basic_program_dump()`, and `LOAD` reads it back through
  `basic_program_append()`, so the on-disk and editor forms carry no gutter
  and round-trip exactly (blank lines and indentation preserved). libbasic
  stays a pure interpreter: the two new functions are the only additions to
  its file-facing surface, and file I/O still lives in the frontend. Old saved
  programs that used sparse line numbers as jump targets do not carry over;
  this is the intended break, since line numbers are gone. Shipped and
  verified end-to-end: label countdowns and the `IF ... THEN <label>`
  shorthand, nested `GOSUB` unwinding inside a negative-STEP `FOR` loop,
  file-line replace and delete, and a SAVE (raw source) / LOAD / RUN
  round-trip. The libbasic test suite and the frontend SAVE/LOAD path are
  valgrind clean. Also driven interactively in a live session: `lumi basic`
  run in a window of a detached `lumi new -s t` session, the whole stack
  rendered through GNU screen's PTY, where entering a labelled program by
  file-line number, `LIST` showing the gutter with the label preserved, and
  `RUN` looping through `IF N > 0 THEN count` all worked as at the pipe.
- `lumi basic` `RUN` now validates the program before running it. A pre-run
  pass reports a duplicate label or a jump (`GOTO`/`GOSUB`, or the
  `IF ... THEN <label>` shorthand) to an undefined label, naming the file line
  and the label as written, and aborts before any statement executes. This
  closes the sharp edge the non-strict `IF ... THEN` left: a mistyped jump
  target is now a clear up-front error rather than a mid-run failure or, worse,
  a stray statement (a typo that happened to match a keyword such as `QUIT`).
  The check only flags unambiguous jumps: explicit `GOTO`/`GOSUB` targets, and
  a lone token after `THEN` that is not a statement keyword; `THEN END`,
  `THEN PRINT ...`, and the like are left alone. Verified: undefined `GOTO`
  aborts before line 1 runs, duplicate labels and mistyped `THEN` targets are
  rejected, and a valid program with `THEN END` still runs; valgrind clean.
- `lumi basic` `EDIT` gained a filename and save-and-run. `EDIT <file>` uses
  that file as a persistent backing store instead of a throwaway temp: an
  existing non-empty file is edited as it is (reopening a saved program), and a
  missing or empty one is first seeded with the current in-memory program (so
  `EDIT new.bas` starts a fresh file from what you have). `EDIT` with no name
  keeps the temp-file behavior; that temp file is now named with a `.bas`
  suffix (via `mkstemps`) so `lumi edit` picks the BASIC highlighter for it,
  the same coloring a named `.bas` file gets. Either way, on return the program
  is reloaded
  and then run, so the edit/run cycle is one step (the QBasic F5 gesture); an
  empty program is a quiet no-op and a run error prints as `?message`. The
  filename parsing that SAVE/LOAD/EDIT share moved into one `extract_filename`
  helper. All frontend commands, so libbasic stays a pure interpreter.
  Verified: SAVE (quoted) / LOAD round-trip and the bare-word guards still
  hold, the no-TTY `EDIT` reports cleanly, and the SAVE/LOAD/EDIT path is
  valgrind clean.

---

## vi Keybindings in `lumi edit` (IN PROGRESS)

**Goal:** grow the modeless `lumi edit` into a vi/nvi/elvis/vim-style modal
editor, in increments. The buffer engine (`libtext`) and rendering are
shared; the vi personality is layered on top and toggled at runtime.

**Increment 1 (DONE).** A vi personality living in a delimited section of
`src/cmd/edit/edit.c`.

- `F2` toggles between the modeless editor and vi NORMAL mode; `Esc` leaves
  INSERT for NORMAL. The status bar shows the mode and any pending
  count/operator; the terminal cursor is a block in NORMAL and a bar in
  INSERT.
- Counts and the operator + motion grammar. Motions `h j k l`, `0 ^ $`,
  `w b e` and `W B E`, `gg`, `G`, plus the arrow/Home/End/PgUp/PgDn keys and
  `Ctrl-D/U`, `Ctrl-F/B` scrolling. Insert entries `i a A I o O`. Edits `x`,
  `p`/`P` (charwise and linewise register), operators `d c y` over any motion
  and doubled `dd cc yy` (with `cw`/`cW` acting like `ce`/`cE`, and `dw`/`yw`
  stopping at end of line rather than joining).
- `u` undo and `Ctrl-R` redo. An ex line `:` runs `w [file]`, `q`, `q!`,
  `wq`/`x`, and `:N`; `/` searches and `n` repeats.

**Undo grouping (DONE, `libtext`).** Added `text_undo_group_begin` /
`text_undo_group_end` so a command built from several primitives (a linewise
delete, a paste, a change) undoes and redoes as one step. Each recorded
primitive carries a group id; undo/redo replay the whole run. This also fixed
the modeless editor's multi-line cut and paste, which previously needed
several undo presses. Tests `test_undo_group_multi` and
`test_undo_group_nested` in `src/libtext/test_text.c`.

**Files changed:** `src/cmd/edit/edit.c` (the vi section, mode wiring,
status/cursor), `src/libtext/text.c` and `text.h` (undo groups),
`src/libtext/test_text.c`, `doc/lumi.1.in`.

**Increment 2 (DONE).** Line-local character search motions.

- `f`/`F` move to the next/previous occurrence of a typed character on the
  line; `t`/`T` stop just before/after it. `;` repeats the last such search
  in the same direction and `,` in the opposite one. All take a count and
  compose with operators (`dt)`, `df,`, `2dt,`). A pending search takes the
  next key as its literal target (`vi_charsearch`), and the last search is
  remembered (`vi_last_fT`, `vi_last_fT_ch`) for `;`/`,`.

**Increment 3 (DONE).** Paragraph and sentence motions.

- `{`/`}` move by paragraphs (empty-line separated). `(`/`)` move by
  sentences, where a sentence ends at `.`, `!` or `?` followed by optional
  closers (`)]"'`) and then whitespace or end of line, and a blank line is
  also a boundary. All take a count and compose with operators (`d}`, `2d)`,
  `c(`). The backward sentence walks forward from at most a paragraph earlier
  and keeps the last start before the cursor, which handles crossing a blank
  line. These are charwise-exclusive motions; the vim rule that promotes an
  exclusive motion ending at column 0 to linewise is not yet implemented, so
  `d}` can leave one blank line where vim leaves the paragraph gap.

**Increment 4 (DONE).** The `%` match-pair motion.

- From a bracket (`()`, `[]`, `{}`) `%` jumps to its match, counting nesting
  and scanning across lines. When the cursor is not on a bracket it uses the
  first one at or after the cursor on the line. It is an inclusive motion, so
  `d%` covers through the match, and `d%` from before a bracket deletes from
  the cursor through the match. The count prefix is ignored (vi's `N%`
  go-to-percentage variant is deferred). Helpers `vi_bracket_info` and
  `vi_match_pair` in `edit.c`.

**Increment 5 (DONE).** The `H`, `M`, `L` window motions.

- `H`, `M`, `L` move to the first, middle, and last line of the visible
  window, landing on the first non-blank. A count makes `H` and `L` count in
  from the top or bottom (`3H`, `2L`). They are linewise and compose with
  operators (`dH`, `dL`). The window is read from `e->top` and the text
  height (`rows - 1`), which `render` keeps current.

**Increment 6 (DONE).** The `|` go-to-column motion.

- `|` moves to a display column on the current line: column 1 without a
  count, or the count-th column (`5|`). It maps a display column back to a
  byte offset with `vi_col_to_byte`, the inverse of `disp_cols`, so tab stops
  and wide characters are handled. It is a charwise exclusive motion and
  composes with operators (`d5|`).

**Increment 7 (DONE).** More quit commands.

- Normal mode `ZZ` writes the buffer if it changed and quits; `ZQ` quits
  without writing (a `Z`-prefix pending state like `g`). The ex line gained
  the quit-all family `:qa`/`:qall`/`:quita`/`:quitall` and write-all
  `:wqa`/`:wqall`/`:xa`/`:xall` (a trailing `!` forces past unsaved changes,
  matching the single-buffer `:q`/`:wq` behavior), plus `:cq`/`:cquit` which
  leaves with a nonzero exit status (a new `REQ_QUIT_ERR` sets the process
  return code). A small `ex_match` helper keeps the name lists tidy.

**Increment 8 (DONE).** Syntax highlighting, engine + C and shell.

- New `src/libsyntax`: a joe/JSF-style state-machine highlighter distilled
  from the compact-pascal playground. Each language is a C table of states
  (`struct syn_state`) with rules (charclass match, next state, recolor,
  identifier buffer) and a keyword list; the structs are public so a runtime
  file loader can build the same tables in a later increment. `syn_line`
  highlights one line, carrying tokenizer state across lines for multi-line
  comments and strings. Languages: C (`syn_c.c`) and shell (`syn_sh.c`).
  Tests in `test_syntax.c`.
- `lumi edit` integration: language chosen by file extension; a per-line
  start-state cache (`line_state`, `hl_valid`) that edits invalidate from the
  changed line down and the renderer fills forward to the visible window;
  `draw_line` emits an indexed-ANSI foreground per style run, composed with
  the selection's reverse video. The vi command `:syntax off|on|<name>`
  toggles or forces it. Valgrind clean.
- Palette is a fixed indexed-ANSI set in `edit.c` (comment grey, keyword
  yellow, type cyan, constant magenta, string green, preproc red); making it
  themeable via `tui_theme` is a later option.

**Increment 9 (DONE).** Lua and Python `libsyntax` tables.

- `syn_lua.c`: keywords/builtins, numbers, single-line strings, `--` line
  comments, and the `--[[ ]]` long comment and `[[ ]]` long string (level-0;
  the `[==[` equals-level variants are not modeled). `syn_py.c`: keywords,
  builtins and types, numbers, `#` comments, single-line strings, and
  triple-quoted `"""`/`'''` strings that span lines. Both carry state across
  lines for their multi-line constructs. Tests added to `test_syntax.c`.

**Increment 10 (DONE).** Rust and Go `libsyntax` tables.

- `syn_rust.c`: keywords, primitive and common std types, numbers, strings
  (span lines), and `//` / `/* */` comments. Char literals are left uncolored
  because `'x'` and the `'a` lifetime cannot be distinguished by a simple DFA;
  block comments are treated as non-nesting. `syn_go.c`: keywords, predeclared
  types, builtins, numbers, `"..."` strings, backtick raw strings (span
  lines), rune literals, and C-style comments. Tests added.

**Increment 11 (DONE).** BASIC and Forth `libsyntax` tables.

- `syn_bas.c`: a generic classic/QBASIC dialect, case-insensitive. Keywords,
  a few types and builtins, numbers, `"..."` strings (do not span lines), and
  two comment forms: the `'` apostrophe comment to end of line, and `REM`
  painted in the comment color (its trailing text stays code, since `REM` is
  matched as an ordinary word). `syn_fth.c`: Forth is whitespace-delimited, so
  a word runs until the next space and is then looked up. Handles `\` line
  comments, `( ... )` comments (may span lines), numbers, and the core control
  and stack words. String words (`."` `s"` `c"` `abort"`) are left as plain
  words. Tests added.

**Increment 12 (DONE).** JavaScript and HTML/XML `libsyntax` tables.

- `syn_js.c`: a C-like table with double- and single-quoted strings, backtick
  template literals (span lines, no `${...}` sub-highlighting), line and block
  comments, numbers, and keywords, built-in constructors, and value literals.
  Regex literals are not recognized, since telling `/re/` from division needs
  expression context. `syn_html.c`: structural, with no keyword table. Element
  names are painted as keywords, attribute names as types, quoted attribute
  values as strings (a value keeps its color through any `>` it contains),
  `<!-- -->` as comments, doctypes and `<?...?>` as preproc, and `&entity;`
  references as constants. `<script>` and `<style>` bodies are left as plain
  text. Tests added.

  While writing the JS operator class, found that the shared operator class
  string in the earlier tables begins `+-*`, which the class parser reads as
  the (empty) range `+` to `*`, so `+` and `*` are not colored as operators.
  The new JS table puts `-` last to avoid this; the older tables still carry
  the latent quirk. See the deferred list.

**Increment 13 (DONE).** NASM and GAS (AT&T) assembly `libsyntax` tables.

- `syn_nasm.c`: case-insensitive. Handles `;` line comments, raw `'...'` and
  `"..."` strings, `` `...` `` strings with escapes, NASM number forms, and
  `%` preprocessor words (`%define`, `%macro`, `%1`). A keyword table paints
  common mnemonics and directives as keywords and the general-purpose and SSE
  registers as types. `syn_gas.c`: handles `#` and `//` line comments, block
  comments, escaped strings, and the AT&T sigils, painting `.directives` as
  preproc, `%registers` as types, and `$immediates` as constants. Its keyword
  table lists common mnemonics with the frequently used size-suffixed forms
  (`movl`, `movq`, and so on). Labels are left as plain text in both, since a
  name is a label only by its trailing colon and the tables do not look ahead.
  Tests added.

**Increment 14 (DONE).** Pascal `libsyntax` table.

- `syn_pas.c`: case-insensitive (Turbo/Delphi/Free Pascal). Handles all three
  comment forms, with the block kinds spanning lines: `{ ... }`, `(* ... *)`,
  and `//`. A brace that opens with `$` is a `{$...}` compiler directive,
  painted as preproc rather than comment. Strings are `'single quoted'` (a
  doubled `''` reads as two adjacent strings, keeping the run colored).
  Numbers cover decimal, `$hex`, and `%binary`. A keyword table paints
  reserved words, built-in types, and common standard procedures. This clears
  the original target language list (c, lua, python, shell, nasm, gas, pascal,
  rust, go, javascript, html). Tests added.

**Increment 15 (DONE).** Assembly label recognition (NASM and GAS).

- A `name:` label is now recolored as a function. This needed a small engine
  primitive: a new `recolor_buf` rule flag that repaints the whole current
  identifier buffer to the target state's style, since a label's length is not
  fixed and the existing `recolor` count is. When the identifier state sees the
  trailing `:`, it recolors the buffered name and enters a short LABEL state
  that consumes the colon. In GAS the `.directive` state carries the same rule,
  so `.L1:` local labels are caught and distinguished from directives by the
  trailing colon. The flag is a new trailing field on `struct syn_rule`, so the
  older tables (whose rule initializers omit it) are unaffected. Tests added.

**Increment 16 (DONE).** JavaScript `/regex/` literal recognition.

- A slash is a regex where an operand is expected and a division where one just
  ended, so the JS idle state was split into two contexts: `JS_RE` (regex may
  start: input start, and just after an operator, an opening bracket, or a
  separator) and `JS_DIV` (a slash is division: just after an identifier,
  number, string, or a closing bracket). Each token routes to the right
  context on completion, and a slash resolves against whichever is current;
  comments still work in both. A regex body handles `\` escapes and trailing
  flags, and is colored as a string. Known limits (documented in the table): a
  regex after a keyword that expects one (`return /re/`) reads as division,
  since context is chosen by the previous token's shape and not its meaning; a
  postfix `++`/`--` before a division reads as a regex; and a `/` inside a
  regex `[character class]` is treated as the delimiter. Tests added.

**Increment 17 (DONE).** JavaScript `${...}` template interpolation.

- Inside a template literal, a `${ ... }` interpolation is now highlighted as
  an expression: identifiers and keywords, numbers, quoted strings, and
  operators, ending at the matching `}`. A dedicated set of interpolation
  states drives this. A `}` inside a nested string is consumed by that string,
  so `${ obj["}"] }` closes at the right brace. What is not tracked is brace
  depth: an interpolation that itself contains a `{ }` object literal or a
  nested template ends at the first `}`, because the engine has no counter or
  stack. An escaped `\${...}` is left as plain string, and a `/` inside an
  interpolation is always an operator. Tests added.

**Increment 18 (DONE).** Operator character-class cleanup.

- The operator classes in the c, rust, go, and bas tables began `+-*`, which
  the class parser reads as the empty range `+` to `*`, so `+` and `*` were
  classified as plain text rather than operators. Reordered each so the `-` is
  last (a literal). This is engine-level correctness only: the editor palette
  maps `SYN_OPERATOR` to the default foreground, the same as `SYN_TEXT`, so
  there is no visible change. The sh, py, and lua tables were not affected
  (they have no `-` in a range-forming position). A C regression test locks
  `+`, `*`, and `-` as operators.

**Increment 19 (DONE).** Rust, Forth, and BASIC table refinements.

- Rust: char literals now colorize (`'x'`, `'\n'`, `' '` become constants)
  while lifetimes (`'a`, `'static`) stay plain, using the exact test "a letter
  after the quote followed by a non-quote is a lifetime." Raw strings `r"..."`
  and `r#"..."#` are recognized (an inner `"` is kept). Block comments nest one
  level. The counting cases stay bounded: `r##"..."##` and byte-raw `br"..."`
  are not specially matched, and a third comment-nesting level closes one
  delimiter early.
- Forth: string words (`."`, `s"`, `c"`, `abort"`) now paint the introducer and
  the `"..."` body as a string. Approximation: any `"` within a word triggers
  it, which is where a `"` normally appears in Forth.
- BASIC: `REM` runs to end of line, matched as a whole word by a prefix chain
  (so `REMARK`/`REM1` stay identifiers). A trailing type sigil is folded into
  the identifier, so `count%` reads as one token and the string builtins are
  named with their sigils (`LEFT$`, `CHR$`, ...), coloring fully as functions.
- No engine change: the char and Forth-string paints use the existing
  `recolor_buf` rule flag, and REM uses `def_recolor`.

**Increment 20 (DONE).** Mode-aware menu shortcuts and help screen.

- The DOS-chrome pull-down menus and the `F1` help screen showed only the
  modeless `Ctrl+` chords, which do not reach the editor while the vi
  personality is active. Each menu item gained a second `vaccel` column
  (`item_accel` picks it when not modeless), so a menu drawn in vi mode shows
  the vi keys instead (`:w` for Save, `:q` for Exit, `u`/`Ctrl-R` for
  undo/redo, `dd`/`yy`/`p` for cut/copy/paste, `/`/`n`/`G` for the Search
  items). An empty `vaccel` reuses the modeless accel where the two agree
  (`F1`, `F2`). The help screen gained a parallel `help_entries_vi` table and
  `render_help` chooses it by mode. Verified live under `screen`: menus and
  help switch with `F2`.
- **Files changed:** `src/cmd/edit/edit.c`, `doc/lumi.1.in`.

**Deferred to later increments (roadmap).**

- Assembly: broaden the GAS mnemonic list beyond the common suffixed forms.
  GAS numeric local labels (`1:` with `1f`/`1b` references) are not recognized,
  since the digit begins a number rather than an identifier.
- Rust: raw-string hash levels of 2 or more, and block-comment nesting beyond
  one level (both need a counter the DFA does not have). Forth: restrict the
  string span to the four canonical words rather than any in-word `"`.
- Embedded JS/CSS highlighting inside HTML `<script>`/`<style>`.
- A runtime loader that builds `struct syntax` from files (the "loader later"
  half of the format decision).
- Function-call and matched-bracket highlighting; theme-driven palette.
- Backward search `?` and reverse repeat `N`; search offsets; `*`/`#`.
- Text objects (`iw`, `aw`, `i(`, `i"`, ...).
- The exclusive-to-linewise motion promotion (affects `d}`, `d{`).
- The `N%` go-to-percentage variant of `%`.
- The `.` repeat register, named registers (`"a`), and marks (`m`, backtick).
- Visual mode: `v` charwise and `V` linewise are done (see roadmap V1).
  Blockwise `Ctrl-V` (rectangular selection and block operators) remains.
- `r`, `R` (replace), `~`, `J` (join), `>>`/`<<` (shift), `s`/`S`, `D`/`C`.
- A richer ex line: ranges, `:s///`, `:g`, `:%`, `:e`, `:r`, settings.
- Line-preserving column memory for `j`/`k`, and `count` with `G`/`gg` edge
  cases.
- Extract the vi section to its own file (DONE): it now lives in
  `src/cmd/edit/vi.c` behind the shared `editor.h`. See roadmap item F2.

## DOS EDIT-style Chrome in `lumi edit` (DONE)

**Goal:** dress the modeless/vi editor in DOS EDIT / QBasic-style chrome, a
menu bar, a framed window, scrollbars, and modal dialogs, all built on the
`libdraw` surface. The work landed as a sequence of increments, each on top
of the last.

**Increment 1 (DONE).** The frame. The text area is wrapped in chrome: a menu
bar on the top row, a window border whose top edge centers the file name, a
vertical scrollbar down the right edge and a horizontal one along the bottom
border, and a status line showing key hints and the cursor `Line:Col`. The
text area is inset by all of this, so the drawing geometry moved behind
`text_height` and `text_width`, threaded through `render`, the scroll and
paging math, and the vi window-line motions; `draw_line` gained a base column
so it paints into the framed region. Colors come from an editor-local palette
with a DOS preset (blue text area, gray bars) on by default and a monochrome
fallback.

**Increment 2 (DONE).** Working drop-down menus. `F10` or `Alt+letter` opens a
pull-down for File, Edit, Search, View, Options, or Help; the arrow keys move
between menus and items, `Enter` runs the highlighted one, and `Esc` backs
out. (The per-item letter shortcut started as the label's leading character
and became an underlined mnemonic in increment 9.) The menus are a static
table (`MENUS`, per-menu item arrays)
so the bar titles, their columns, and the drop-down contents stay in one
place. Each item maps to work the editor already does (save, undo, cut, copy,
paste, find, go to line, help) plus new File operations (New, Open, Save As)
and toggles (View's Color Scheme and Syntax Highlight). `render` split into
`render_body` plus a present so the menu loop composites the bar and drop-down
over the editor and flushes once.

**Increment 3 (DONE).** Mouse. Mouse reporting is on by default: click the
menu bar to open a pull-down, click an item to run it, click a line to place
the cursor, drag to extend a selection, and use the wheel to scroll. The bar
and drop-down reuse the same hit-testing so a click resolves to a menu or an
item, and a click outside an open menu closes it. `Options > Mouse` toggles
reporting off, handing the mouse back to the terminal so its own selection
works again.

**Increment 4 (DONE).** Draggable scrollbars. Press or drag the vertical
scrollbar to set the top line and the bottom scrollbar to set the horizontal
offset. A press records which region was grabbed (text, vertical bar, or
horizontal bar) and later motion events follow that drag until release, so
the mouse can leave the bar column mid-drag without losing it. An arrow cell
nudges by one line or column; grabbing the track maps position proportionally,
and the cursor is pulled back into the new view so it stays visible.

**Increment 5 (DONE).** Toggle indicators. A bullet in a drop-down item's left
margin marks an enabled toggle (Color Scheme, Syntax Highlight, Vi Keys,
Mouse), so its state reads at a glance. The mark sits in the existing left
padding column, so item widths and label alignment are unchanged
(`menu_checked` drives it).

**Increment 6 (DONE).** The About dialog. `Help > About` opens a centered
modal box with the program name and version rather than a one-line status
message. It reuses the drop-down gray palette and box glyphs, draws a
reverse-video OK button, and repaints on resize. Any key or a fresh left
click dismisses it; the release trailing the click that opened it is ignored
so the dialog does not close the instant it appears.

**Increment 7 (DONE).** The unsaved-changes dialog. New, Open, and Exit now
put up a centered Yes/No/Cancel modal instead of a status-line y/n prompt,
navigated by the arrow keys or Tab and chosen with Enter, the Y/N keys, or a
click, so a modified buffer is never replaced or dropped by accident. The
dialog shares a box helper factored out of the About box, and the three call
sites (New/Open via `confirm_discard`, and both quit paths) route through one
`confirm_save` helper, retiring the old `confirm_yn`.

**Increment 8 (DONE).** Mode-aware shortcuts. The menu accelerators and the
`F1` help screen follow the active personality; see increment 20 of the vi
keybindings section above for the details.

**Increment 9 (DONE).** Underlined mnemonic keys. Menu titles, drop-down
items, and the Yes/No/Cancel dialog buttons carry a DOS-style `&` marker
before their shortcut letter, drawn underlined. Selection now matches that
mnemonic rather than the first character of the label, so ambiguous items
are reachable: `Save As...` (`a`) no longer collides with `Save` (`s`), and
the dialog's `Cancel` gained a `c` key it never had. A single
`draw_menu_label` renders the `&`-marked strings and `menu_disp_w` /
`menu_mnemonic` read them; `menu_title_by_mnemonic` and
`menu_item_by_mnemonic` unify the three match sites (Alt+letter,
`menu_trigger`, and the in-menu letter). This closes the discoverability
gap left in the deferred notes below: the underlined letter is now the
visible cue for the direct chord.

**Increment 10 (DONE).** Internal refactoring pass, no behavior change. As
the file grew past 5,500 lines the modal and geometry code was factored into
named helpers so concerns read separately:

- `center_box` and `dialog_palette` replace the centered-overlay geometry
  and palette fetch that the dialogs had copied inline.
- `CHROME_BOTTOM` / `CHROME_RIGHT` name the bottom/right border geometry
  that was scattered as `rows-2` / `rows-4` / `cols-1` / `cols-2`.
- `current_selection_text` replaces the `sel_bounds` + `region_text` pair
  that Copy, Cut, and the send-to-pane command each spelled out.
- `handle_mouse` moves the ~100-line inline mouse block (wheel, scrollbar
  drag, menu-bar click, click/drag selection) out of the main event loop;
  the anonymous drag-state enum became `enum drag_mode`.
- `draw_scroll_view` shares the full-screen header/body/footer skeleton
  between the `F1` help screen and the build-output viewer, each supplying a
  `get_line` provider.
- `modal_run` owns the centered-dialog loop (frame, present, EOF, resize);
  the save-confirm and About dialogs supply a draw callback, a key callback,
  and a small context struct. This one is a few lines longer than the inline
  loops it replaced, kept because a third dialog now costs only its two
  callbacks.

**Files changed:** `src/cmd/edit/edit.c` throughout, and the `lumi edit`
section of `doc/lumi.1.in`.

**Deferred / notes.**

- The palette is an editor-local preset, not `tui_theme`-driven; wiring it to
  the theme system is a later option (shared with the syntax-palette note in
  the vi section).
- No keyboard accelerator opens the menus other than `F10`/`Alt+letter`; the
  drop-downs are the discoverable path and the direct chords still work.

## Build Commands in `lumi edit` (DONE)

**Goal:** SciTE-style per-file-type build commands, giving the editor IDE-like
compile/make/run hotkeys that integrate with a `lumi` session and `lumi basic`.
SciTE keys compile/build/go to file patterns in its properties files; we map
that onto `lumi.conf` and the tiled session.

**Design decisions (settled with the user).**

- Config lives in `lumi.conf` as `[build "<ext>"]` sections with a `[build]`
  default, plus built-in defaults when the config is silent. Verbs are
  `compile`, `make`, `run`; keys `dir` (working directory) and `save` also
  apply. Variables `$(file)`, `$(filedir)`, `$(filebase)`, `$(filename)`,
  `$(fileext)`.
- Execution splits by verb, mirroring SciTE (Compile/Build use the output
  pane with error filtering; Go launches the program). `compile` and `make`
  are the captured verbs that get error parsing; `run` is interactive.
- Keys: `F5` run, `F6` compile, `F7` make (mirrored in a `Build` menu so they
  work by mouse regardless of terminal F-key encoding). Next/prev error will
  be `F4`/`Shift+F4` (or `]e`/`[e` in vi) in the error-parsing increment.

**Increment 1 (DONE).** Config, verbs, and the simple execution path.

- `edit` reads `[build]` from `lumi.conf` via `libcfg` (`load_build_config`),
  with a built-in `build_defaults` table (universal `make`; C/C++ compile;
  shell and BASIC run). `build_cmd` resolves a verb: `lumi.conf` wins over the
  table, and `[build "<ext>"]` wins over `[build]`. `build_expand` substitutes
  the five variables; `build_save_wanted` and a `dir` lookup honor those keys.
- `run_build` saves the buffer (unless disabled), then either sends
  `(cd '<dir>' && <cmd>)` to the adjacent pane via `lu_send_input` (in a
  session, the Ctrl-G target) or suspends the display, runs the command in the
  file's directory, and waits for a key (no session). All three verbs use this
  path in this increment; `compile`/`make` move to captured output in
  increment 2.
- A `Build` menu (Run/Compile/Make, F5/F6/F7) sits between Search and View;
  the menu columns were renumbered. The keys are handled in the main loop so
  they work in both the modeless and vi personalities. Help screens (both
  personalities) and usage text list them.
- Verified live under `screen`: the Build menu renders with the right keys;
  the no-session inline path runs `cc -Wall -c foo.c` (producing `foo.o`) and
  `make` in the file's directory (producing the Makefile's output); a
  `lumi.conf` `[build "c"]` override replaces the default and expands
  `$(filebase)`/`$(fileext)` correctly.
- The pane-send path was verified end to end in a two-window session: F7 in
  the editor ran `make` in the file's directory in the adjacent pane. Two
  findings came out of it, both fixed. First, `build_expand` had to resolve
  the file to an absolute path: the command runs in the target pane's own cwd,
  so a relative `$(filedir)` of `.` landed in the wrong directory. Second, the
  send used to need the write role, so in the default single-writer mode with
  an attached client it was refused as read-only. That is now fixed for every
  `lu_send_input` caller (see below), so the build loop works in a plain
  single-writer session.
- **Files changed:** `src/cmd/edit/edit.c`, `doc/lumi.1.in`.

**Inject capability (DONE, `mserver`/`libipc`/`send-input`).** `send-input`
attached as an ordinary client, so single-writer mode gave it a view role and
gated out its input, blocking the pane-send used by `lumi edit` (build and
Ctrl-G) and `lumi basic` (SEND). Added `IPC_ATTACH_F_INJECT`: the client keeps
a view role (never takes the keyboard or reshapes the window) but the server
accepts its input run, restricted to the input-run messages, not
kill/resize/attribute changes. It is trusted like `IPC_ATTACH_F_TOKEN` (owner
uid, owner-only sockets). `lu_send_input` attaches with it and no longer needs
the write role. Test `test_inject_bypasses_keyboard` in `test_mserver.c`;
verified live that F7 in a single-writer two-window session runs `make` in the
neighboring pane.

**Increment 2 (DONE).** Captured output and diagnostics.

- `compile` and `make` now run captured, not sent to a pane: `capture_command`
  forks the command with stdout+stderr to a pipe (stdin from `/dev/null`, cwd
  the file's directory), reads it into a buffer (4 MB cap), and returns the
  exit status. `run` stays interactive (pane-send/inline).
- `show_build_output` displays the captured text in a scrollable full-screen
  viewer (arrows/PgUp/PgDn/Home/End, any other key returns).
  `parse_diagnostics`/`parse_one_diag` collect `path:line[:col]: message`
  diagnostics (gcc/clang and `Makefile:line:` style) into `struct build_err`.
- On return the cursor jumps to the first diagnostic in this file
  (`build_goto`), matched by base name; `F4`/`Shift+F4` cycle next/previous,
  and `Build > Next Error`/`Prev Error` do the same. The column maps through
  `vi_col_to_byte`. Cross-file diagnostics are listed in the viewer but not
  jumpable yet (single-buffer editor).
- Verified live under `screen`: a C file with an error compiled to a viewer
  showing both gcc diagnostics, then the cursor landed on `foo.c:3:13` and F4
  advanced to `foo.c:3:9`; a clean file reported `compile: ok` and produced
  `foo.o`; `Build > Prev Error` cycled correctly.
- **Files changed:** `src/cmd/edit/edit.c`, `doc/lumi.1.in`.

**Deferred from increment 2 (now DONE).** The build used to block the editor
while it ran; B1 made the output stream into the viewer live (see the vi
roadmap's Build integration section), and B2 opens cross-file diagnostics.

**Increment 3 (DONE).** `lumi basic` run loop.

- The original sketch (F5 sends `RUN` to a live REPL beside the editor) did not
  fit `basic`'s `EDIT`, which runs the editor in-process and full-screen, so no
  REPL runs alongside it. It also surfaced that the increment-1 `.bas` run
  default `lumi basic $(file)` was broken: `lumi basic` ignored file arguments.
- Chosen approach (with the user): spawn-and-run in the pane. `lumi basic` now
  takes a `[file]` argument: it loads the file as the program, runs it (reusing
  `load_program_file` + the `RUN` half of `EDIT`), then drops to the REPL. The
  `.bas` run default now works: `F5` on a `.bas` file sends
  `lumi basic <file>` to the adjacent pane, which runs the program and leaves a
  live REPL to inspect it. Stateless, and it reuses the send path and the
  single-writer inject fix from before.
- Verified: `lumi basic prog.bas` piped ran the program then read the REPL;
  live in a two-window single-writer session, F5 in the editor ran the program
  in the neighboring pane, printing its output and leaving the `>` prompt.
- **Files changed:** `src/cmd/basic/basic_cmd.c`, `doc/lumi.1.in`.

**Deferred.** A live-REPL loop that reuses one interpreter across F5 presses
(keeping variable state, the QBasic gesture literally) would need `EDIT` to
open the editor in a side pane and keep the REPL alive. The spawn-and-run loop
covers the practical case without that rearchitecture.

## `lumi edit` Remaining-Work Roadmap (SCOPED)

**Status:** In progress. F1 (the test harness) has landed; the rest is open.
This consolidates the open work for the editor, which the sections above
record in scattered deferred notes, into one ordered plan. The individual
sections stay the source of detail; this is the map and the priority. Two of
the items (the test harness and the `vi.c` split) are new here, not tracked
elsewhere.

**Why now:** `src/cmd/edit/edit.c` is one file of ~5,675 lines, the
second-largest command after `attach.c`, and the vi personality (the largest
remaining feature area) is still growing inside it. Before that growth
continues, the editor needs a test harness and a module split, or each new vi
increment adds untested code to a monolith.

### Engineering foundation (do first)

**F1 (DONE).** A `test_edit` suite on the mock draw driver. The editor had no
automated tests; `libtext` was covered but `edit.c` (the vi state machine,
menus, dialogs, mouse hit-testing, build-output parsing) was verified only
live under `screen`. `test_edit.c` includes `edit.c` directly (it is a
static-heavy single unit with no public header until F2) and drives it
against a `libdraw` mock driver, asserting on the captured cell grid without
a real terminal; `lu_send_input` is stubbed. A new `src/cmd/edit/module.mk`
builds it against the editor's libraries and wires it into `make run-tests`.
The first suite (9 tests, valgrind-clean) covers rendering, inserting, cursor
movement, line and selection copy with the OSC 52 mirror, the mnemonic
lookups, the underlined drop-down mnemonic, a vi motion, and `center_box`.
Extend it as the work below lands, especially alongside F2 and the vi
increments.

**F2 (DONE).** Split the editor into an editor core plus `vi.c`. `edit.c` had
reached ~5,675 lines with the vi personality inline. The vi modal layer moved
to `src/cmd/edit/vi.c` behind a shared `editor.h`, which carries `struct
editor` and its enums (`edit_mode`, `req`) plus the two crossing interfaces:
`edit.c` exports the buffer, cursor, selection, and prompt helpers vi.c calls
(they stop being static), and `vi.c` exports the six entry points edit.c
calls (`vi_dispatch`, `vi_colon`, `vi_search`, `vi_clamp`,
`vi_reset_pending`, `vi_col_to_byte`). The code moved verbatim, no behavior
change; `edit.c` drops to ~3,796 lines. `test_edit` includes both files so
the whitebox suite still reaches every internal. Full suite green; F2, a
motion, `dd`, and `:w`/`:q` verified live.

**F3 (DONE, core).** Multi-file / multi-buffer editing. The editor keeps a
list of open files (`struct ebuf` array on the editor); the flat
`struct editor` stays the live copy of the active buffer, and `buf_save`/
`buf_load` mirror only per-file state (text, cursor, scroll, selection,
syntax, diagnostics, marks) between the flat editor and a parked slot. Global
vi state (registers, the dot register, the clipboard, the last search) stays
flat and is shared across buffers, matching vi. This was the snapshot model,
chosen over a nested `struct buffer` split to avoid rewriting the ~1365
per-field accesses in `edit.c`/`vi.c`. Commands: `:e path` (open/switch),
`:e` (reload), `:enew`, `:ls`, `:bn`/`:bp`, `:b N`, `:bd`/`:bd!`; the top
border shows a `[cur/total]` indicator. Twelve `test_edit` cases cover the
buffer API and the ex commands. B2 (cross-file diagnostic jump) can now build
on `buf_open`.

**F4 (DONE).** Finished the file-editing surface left open by F3.

- Buffer navigation is no longer ex-only. The `File` menu gained `Next
  Buffer`, `Prev Buffer`, and `Buffer List` entries (routed through
  `run_menu_act` to `buf_cycle`/`buf_list`), and `F8`/`Shift+F8` cycle to
  the next/previous open file from every personality, mirroring the
  `F4`/`Shift+F4` build-error keys.
- The ex `:r [N]path` (and `:read`) reads a file's contents in below the
  current line, or below line `N` with a range, leaving the cursor on the
  first inserted line (`ex_read_file`). It loads into a scratch `struct
  text`, joins the lines, and inserts them as one undo step via
  `insert_bytes`. A missing file reports `E484` and edits nothing.

Three `test_edit` cases cover `:r` (insert point and a missing file) and
the File-menu buffer actions; the mnemonic-lookup test was widened for the
new File entries.

### vi personality (the main feature roadmap)

Pulled from the "vi Keybindings" deferred list, ordered by value:

**V1 (DONE, charwise and linewise).** Visual mode. `v` and `V` anchor a
selection at the cursor and extend it by any motion, reusing the existing
selection engine; the operators `d`/`x`, `y`, and `c`/`s` reuse
`vi_apply_operator` (a `vi_mot` built from the anchor, charwise inclusive of
both end cells or linewise), so they match the operator+motion forms. `p`
replaces the selection with the register (one undo step). `o`
swaps ends; `v`/`V` again or Esc leaves. `vi_dispatch` routes to a new
`vi_visual_key` that delegates non-visual keys to `vi_normal_key`, so motions
move the cursor and the selection follows. Rendering highlights inclusively
in visual mode; the status bar shows `-- VISUAL --` / `-- VISUAL LINE --`.
Six tests in `test_edit`. **Deferred:** blockwise visual (`Ctrl-V`) -- a
rectangular selection and block operators do not reuse the linear selection
engine, so it is a separate piece of work.

**V2 (DONE).** Text objects. `iw`/`aw` and `iW`/`aW` (word objects,
line-local), the bracket pairs `i(`/`a(`, `i{`/`a{`, `i[`/`a[`, `i<`/`a<`
(with the `b`/`B` aliases and either bracket accepted, matching across lines),
and the quote pairs `i"`/`a"`, `i'`/`a'`, `` i` ``/`` a` `` (line-local). They
compose with the `d`/`c`/`y` operators (`diw`, `ci(`, `ya"`) and with visual
mode (`viw`, `va(`). Implemented in `vi.c` as `vi_text_object` (dispatching to
`vi_word_object`, `vi_bracket_object`, `vi_quote_object`) plus
`vi_apply_textobject_op`; an `i`/`a` following an operator or in visual mode
arms `vi_textobj` and the next key names the object. Covered by seven
`test_edit` tests (diw, daw, di(, da(, ci(, di", viw).

**V3 (DONE).** The `.` repeat, named registers, and marks.

- Marks: `m<letter>` sets one of 26 marks a-z at the cursor; backtick
  jumps to the exact spot and `'` to the first non-blank of the mark's
  line. Both double as operator motions (`d`a`, `y'b`). Marks hold
  absolute positions and do not shift as the buffer is edited.
  (`vi_do_mark`, `vi_markcmd`.)
- Named registers: `"<letter>` selects register a-z (A-Z appends) for the
  next delete, yank, or put; the unnamed register still mirrors every
  delete and yank. Stored in `struct vi_reg vi_regs[26]`, routed through
  `vi_reg_store`/`vi_reg_get`, with `insert_clip` split so `vi_put` can
  paste arbitrary register bytes via `insert_bytes`.
- `.` repeat: `vi_dispatch` records the keys of each command and, when a
  command returns to rest having changed the buffer, commits them as the
  `.` register (`vi_keylog`, `vi_dot_commit`, `vi_dot_replay`). Undo and
  redo are excluded. Change detection uses a new `text_revision` counter
  in libtext.

Fifteen test_edit cases cover the three features.

**V4 (DONE).** More operators and edits.

- `D`/`C` delete or change to the end of the line, `s`/`S` substitute a
  character or a whole line, all composing on the existing operators via
  `vi_edit_span` (which still opens insert on an empty span).
- `~` toggles case and advances (`vi_toggle_case`); `J` joins the next
  line with a single space (`vi_join_lines`). Both take a count.
- `r<char>` replaces the character(s) under the cursor, `r<CR>` breaks
  the line (`vi_do_replace`); `R` enters Replace mode, an insert session
  with an overtype flag so typing overwrites (`vi_overtype`).
- `>`/`<` are operators that shift lines one indent (a tab) over a motion,
  a text object, a mark, or doubled (`>>`/`<<`), with a count
  (`vi_shift_lines`). Visual mode shifts the selection with `>`/`<`.

All are recorded by `.`. The shift operators are handled inside
`vi_apply_operator` and `vi_apply_textobject_op` so every operator path
(motion, object, mark) shifts rather than deletes. Visual `~`/`J`/`r` on
a selection are still not done and are swallowed so they cannot fire the
normal-mode command mid-selection. Eighteen test_edit cases cover V4.

**V5 (DONE, except offsets).** Search completion.

- `?` searches backward and `/` forward; both share the direction state
  `vi_search_dir`. `do_find` was generalized to `do_find_dir(e, q, dir)`,
  with a backward scan that takes the rightmost match on each line above
  the cursor and wraps around the buffer (`last_match` helper).
- `n` repeats the last search in its own direction, `N` in the opposite.
- `*`/`#` search forward/backward for the word under the cursor
  (`vi_search_word`), matched as a plain substring since the search has
  no regex or word boundaries.

Five test_edit cases cover backward search, its wrap, `*`, `#`, and `N`.
Search offsets (`/pat/e`, `/pat/+2`) are not done; they need the search
to carry a post-match adjustment, which the plain-substring engine does
not model yet.

**V6 (DONE, except `:e`/`:r` and `:set`).** A richer ex line.

- Line ranges: `ex_addr` parses one address (`.`, `$`, a number, a mark
  `'x`, `+/-N` offsets) and `ex_parse_range` assembles a clamped, ordered
  `[lo,hi]`, with `%` for the whole file and `;` re-anchoring `.`. A bare
  range jumps to its last line (`:N`, `:$`, `:.+3`).
- Range commands: `:d` (delete), `:y` (yank), `:>`/`:<` (shift).
- `:[range]s/pat/rep/[g]` substitutes (`ex_substitute`/`ex_subst_line`),
  the first match per line or, with `g`, all; `%` covers the file, an
  empty pattern reuses the last search, an empty replacement deletes.
- `:[range]g/pat/cmd` and `:v` / `:g!` run `d` or `s` on the matching (or
  non-matching) lines (`ex_global`); matches are collected before the
  command runs so line numbers stay valid.
- `vi_colon` was split into `vi_ex_exec` (runs a command line) and the
  prompting wrapper so ex commands are unit-testable.

Patterns are plain byte substrings; there is no regex engine. `:e` now
works (F3 shipped) and `:r` (read a file into the buffer) landed in F4.
`:set` is not done: the editor has few
toggles to expose (`:syntax` already covers highlighting), so a settings
line was deferred until there is more to set. Fixing `:%d` uncovered and
corrected a `delete_region` bug that dropped a multi-line span's middle
lines. Fifteen test_edit cases cover V6.

**V7 (DONE).** Motion corrections.

- Exclusive-to-linewise promotion: in `vi_apply_operator`, an exclusive
  charwise motion ending in column 0 of a lower line pulls its end back
  to the close of the previous line, and becomes linewise when the start
  is at or before its first non-blank, so `d}`/`d{` delete whole lines.
  The linewise operator body was extracted into `vi_op_lines` and shared.
- `N%` jumps to N percent of the file (bare `%` still matches brackets).
- Column memory: a run of `j`/`k` aims for the display column of the
  run's first line (`vi_want_col`), so passing through short lines does
  not lose the column; `$` sets a sticky end-of-line column. The run is
  tracked with `vi_vert_run`/`vi_vert_prev`, cleared whenever any other
  command intervenes, so no per-command resets are needed.
- `G`/`gg` counts (`NG`, `Ngg`, `dG`, `d3G`) already worked; tests now
  cover them.

Eleven test_edit cases cover V7.

### Build integration

**B1 (DONE).** Live-filling build output. A compile or make no longer blocks
the editor until the child exits. `build_stream` forks the command with a
non-blocking pipe and, in a `draw_next_event` timeout-poll loop (40 ms),
drains output as it arrives, indexes it by line, and repaints the viewer.
The view follows the tail until the user scrolls up (arrows/PgUp/Home pause
the follow, Down/PgDn/End rejoin it), and any non-scroll key (q, Esc, Enter)
cancels the build with SIGTERM and returns to editing. When the child exits
on its own, the diagnostics are parsed and the viewer becomes the existing
scrollable browse (`build_browse`, split out of the old `show_build_output`)
before the first-diagnostic jump. The output is held in `struct build_lines`,
now an incremental index: the raw bytes grow in one buffer with newlines
intact (so `parse_diagnostics` still reads it) and a `start` array holds each
line's byte offset, so a realloc while streaming cannot dangle a pointer. The
old blocking `capture_command` is gone. One `test_edit` case covers the
incremental indexer (splitting a line across appends, CRLF, no trailing empty
line). Verified live: a Makefile that echoes a line per second fills the
viewer one line at a time, q cancels mid-build, and a real compile error
still streams then jumps to the line.

**B2 (DONE).** Jump to a cross-file diagnostic. The diagnostics list is now
one global quickfix list shared by every buffer, not per-buffer state, so it
survives buffer switches (it was moved out of the `struct ebuf` snapshot and
the `buf_save`/`buf_load` mirror, and is freed once at teardown). `build_goto`
cycles the whole list rather than only same-file entries; a diagnostic in
another file is opened with `buf_open` (`build_resolve_path` joins a relative
compiler path to the build's recorded working directory) before the cursor
lands on the line, and an already-open file is switched to rather than
duplicated. Fixing this exposed a parser bug: gcc's "In file included from
foo.c:1:" context lines were being recorded as diagnostics with a bogus
path; the old same-file filter hid them, so `parse_one_diag` now rejects a
path that contains a space. `build_goto` also checks the file exists before
opening so a misresolved path does not spawn a blank buffer. Three `test_edit`
cases cover the shared list, the context-line skip, and the cross-file jump.
Verified live: compiling a file whose error is in an included header jumps
into the header, F4/Shift+F4 walk the list across files, and re-entering an
open file switches to it.

### Syntax highlighting refinements

From the vi-section deferred list, all lower priority (documented DFA limits,
not bugs):

- Function-call and matched-bracket highlighting.
- A theme-driven palette (shared with the chrome-palette note below), and a
  runtime loader that builds `struct syntax` from files.
- Embedded JS/CSS inside HTML `<script>`/`<style>`.
- Per-language gaps: GAS numeric local labels (`1:`/`1f`/`1b`) and a broader
  mnemonic list; Rust raw-string hash levels of 2+ and block-comment nesting
  past one level; Forth string span; Lua `[==[` equals-level long
  strings/comments; JS regex-vs-division context and template-interpolation
  brace depth (both need a counter/stack the DFA lacks).

### Chrome

**C1 (DONE, chrome palette).** Theme-driven chrome palette. The editor chrome
can now be derived from a `tui_theme` rather than only the hardcoded preset.
`chrome_from_theme` maps a theme onto the editor's `chrome_pal`: the editing
area takes `content_fg`/`content_bg`, the frame takes `border_fg`/`border_bg`,
the title takes `title_fg`, and the menu/status bars invert the content colors
(a theme has no bar color). An `[edit]` section with `theme = <name>` in
lumi.conf selects any named theme (turbo, thin, crimson, ...); with no key the
built-in DOS preset is kept, so the default look is unchanged. One `test_edit`
case checks the mapping; verified live by capturing the SGR stream (default
renders blue `44m`, `theme = crimson` renders red `41m`).

**C1b (DONE, syntax palette).** The syntax palette is now configurable too.
`tui_theme` has no syntax-color fields (highlighting is editor-specific, not a
widget concern), so rather than extend the shared theme this adds an editor-
local `[edit.syntax]` section. `load_syntax_colors` walks the style-name table
(`syn_style_names`), and for each key present in lumi.conf applies the value
through `syn_apply`, which parses it with `tui_theme_parse_color` (`default`, a
`0`-`255` index, or `#rrggbb`) into `syn_color[]`. Unset keys keep the built-in
color; a bad value is ignored. One `test_edit` case covers the name mapping and
the three color forms; verified live by SGR capture (`keyword = 200` renders
`38;5;200` on the highlighted keyword, absent by default).

### Hex editor mode (SCOPED)

A hex view and, later, a hex editor for the current buffer, living inside
`lumi edit`. The same buffer is viewable as hex or as text: it is one
document with two renderings, not two documents.

**One store, byte-faithful.** `struct text` is already an exact byte
container, so it stays the single source of truth for both views. Each line
holds an explicit byte length, not a C string: `text_load` stores the raw
slice between newlines (NULs and all) and `text_save` writes `line.len` bytes,
so a load then save is byte-exact and embedded NULs survive. The byte stream a
hex view walks is just the line buffers joined by an implied `\n`, with a
trailing `\n` only when `final_newline` is set. The one rough edge is that
`text_line` returns a NUL-terminated pointer, so the text view (and syntax and
search) stop at an embedded NUL; that is a display artifact of the text
rendering, not data loss, and the hex view shows every byte. So hex mode does
not need a separate byte buffer or a document-kind split, and toggling never
re-reads the file or drops unsaved edits.

**Shape (decided).** Hex is an alternate view flag on the buffer, not a
separate command, so it reuses the chrome, the multi-buffer list (F3),
scrolling, the status bar, save/quit, and even the cursor: the text cursor
`(cy, cx)` is the position in both views, and a byte offset is just its
distance from the start of the buffer. A `hex_view` bit (per buffer, saved and
restored by `buf_save`/`buf_load`) selects the rendering; the editor's draw and
key dispatch branch on it, the way the build-output viewer sits beside the text
view. Toggling text<->hex keeps `(cy, cx)`, so the caret stays on the same byte.

Two small mapping helpers carry the design: `hex_offset_of(t, cy, cx)` (sum of
earlier line lengths plus one per implied newline, plus `cx`) and its inverse
`hex_pos_at(t, offset, &cy, &cx)`. Both are pure functions over `struct text`
and are the natural unit-test target.

**H1 (read-only viewer, DONE).** Toggle the current buffer into a scrolling
hex dump and back.

- A new `src/cmd/edit/hex.c` holds the pure byte-model helpers over
  `struct text`: `hex_total`, `hex_offset_of`/`hex_pos_at` (position <->
  byte offset), `hex_gather` (copy a run of the reconstructed stream, implied
  newlines included), and `hex_format_row` plus `hex_hexcol`/`hex_asciicol`
  for the dump layout. `text_final_newline` was added to libtext so the byte
  stream is exact. The screen rendering (`hex_render`) and key handling
  (`hex_key`) live in `edit.c` where the chrome, menu bar, and draw helpers
  already are; only the testable byte logic moved to `hex.c`.
- `render_body` branches to `hex_render` when `e->hex_view` is set: an offset
  column, 16 hex bytes, and an ASCII gutter (nonprintables shown as `.`), the
  cursor byte drawn in reverse, following the tail via `hex_top`.
- Navigation reuses the text cursor `(cy, cx)`: arrows and PgUp/PgDn move the
  cursor byte, `g` prompts for a hex offset, and `q`/`Esc` return to text.
  Toggling keeps `(cy, cx)`, so the caret stays on the same byte.
- Entry: the `View > Hex Dump` menu item (a per-buffer `hex_view` flag mirrored
  by `buf_save`/`buf_load`); the status bar shows the offset and total size.
  A byte/ASCII search is deferred to land alongside H2 editing.
- One `test_edit` case covers the offset mapping, `hex_gather` across a line
  boundary, and the row formatter; verified live on a file with a control byte
  (the dump matched `od`, and the cursor position survived the toggle).

**H2 (overwrite editing, DONE).** The hex view now edits. `Tab` toggles the
active sub-column (`hex_ascii`); in the hex column two hex digits accumulate a
byte (`hex_pending` holds the typed high nibble and the cursor cell shows it),
and in the ascii column any printable character writes one. `hex_overwrite`
maps the cursor offset to `(cy, cx)` and rewrites the byte as one undo group
through `text_delete` + `text_insert`, so undo and dirty tracking come for
free, and `hex_advance` steps to the next byte. Writing over, or with, an
implied newline is refused (line-structure edits are H3). Saving is `File >
Save` (which `text_save` already writes verbatim); the status bar shows a `*`
while dirty and which column is active. The transient edit state resets on
entry and on a buffer switch. A new `test_edit` case covers the byte overwrite,
the newline refusals, and undo; verified live: typing `58` then Tab and `Y`
turned `48 69 ..` into `58 59 ..`, and File > Save wrote those exact bytes to
disk.

**H3 (structural editing, DONE).** The hex view now inserts and deletes bytes,
not just overwrites. `Ins` toggles a `hex_insert` flag (the status line shows
`OVR` or `INS`), which resets on entry and on a buffer switch. In insert mode a
completed byte calls `hex_insert_byte`: a `0a` byte splits the line with
`text_split`, any other byte is added with `text_insert`, and the cursor lands
on the byte after it. `Del` (`hex_delete_at`) removes the byte under the cursor,
`Backspace` (`hex_delete_prev`) the byte before it; deleting an implied interior
newline joins the lines with `text_join`. The one trailing newline of the file
is the `final_newline` flag, not a byte, so the delete keys leave it alone. Each
edit is one undo group, so undo and dirty tracking still come for free. A
`test_edit` case covers insert, the newline split, the join-on-delete, the
protected trailing newline, backspace, and undo; verified live with a screen
session (Alt-v h opened the dump, Ins flipped OVR to INS, and typing `4142` in
insert mode prepended `41 42` to the buffer with the total and dirty flag
updating).

**H3b (search, DONE).** The hex view searches its byte stream. In the hex
column `/` prompts for text (matched as its literal bytes) and `\` for a hex
pattern (byte pairs such as `0a 0d`, spaces ignored); `n` and `N` repeat the
last pattern forward and backward. Two pure helpers in `hex.c` carry it,
`hex_parse_bytes` (parse the hex pairs) and `hex_find` (a wrapping byte search
over the reconstructed stream, so a pattern may cross an implied newline and
the match under the cursor is skipped). The last pattern lives in
`hex_pat`/`hex_pat_len`/`hex_pat_dir` on `struct editor` (global, not
per-buffer). The command letters act only in the hex sub-column, where they are
not byte data. `hex_render` now shows a transient message (the match offset, or
"pattern not found") on its status line. A `test_edit` case covers the parser
and forward, backward, wrapping, cross-newline, and not-found searches; verified
live (`/world` and `n`/`N` walked the two matches, `\0a` found a newline, and a
missing pattern reported not found). This also fixed a latent bug: `hex_goto`
and the search prompt passed an uninitialized buffer to `prompt_line`, which
starts from `strlen(buf)`, so both now clear `buf[0]` first.

**H3c (configurable width, DONE).** The hex view's `w` key cycles the dump
width between 8, 16, and 32 bytes per row (`hex_cols` on `struct editor`, a
global view preference defaulting to 16). The layout helpers in `hex.c` are now
parametric: `hex_hexcol` keeps a wider gap after every group of eight (so it
works past 16), and `hex_ascii_start`, `hex_asciicol`, and `hex_row_width` take
the column count. `hex_render` and the cursor-movement math divide by `hex_cols`
rather than the old fixed 16, and the per-row scratch buffers grew to hold a
32-byte row. Wider rows simply clip on a narrow terminal (the draw layer bounds
each row to the screen width). A `test_edit` case checks the 8- and 16-byte
layouts and the group-gap columns; verified live (w cycled 16 -> 32 -> 8, each
with correct gutters).

**H3d (data inspector, DONE).** The hex view's `i` key toggles a footer line
(above the status bar, so it costs one content row) that decodes the bytes at
the cursor: the first byte as unsigned and signed 8-bit and as a character,
then the 16- and 32-bit values in little-endian and big-endian. A pure
`hex_inspect_line` helper in `hex.c` formats the text from the up-to-four bytes
gathered at the cursor, showing "-" for fields without enough bytes, so it is
unit-tested directly. Verified live: the line tracked the cursor and its u16/u32
little- and big-endian decodes matched the bytes on screen.

**H4 (byte copy/paste, DONE).** The hex view has a byte-native clipboard. `v`
starts a selection at the cursor (a second `v`, or Esc, clears it) and moving
the cursor extends it; `y` copies the selected bytes, or the single byte under
the cursor, to the editor clipboard (which `clip_set` also mirrors to the system
clipboard via OSC 52); and `p` inserts the clipboard bytes at the cursor as one
undo group, splitting the line on any newline byte via the shared
`insert_bytes`. The selection is a byte range anchored at `hex_anchor` with
`hex_sel` set, highlighted in reverse video in both the hex and ascii columns,
with its extent shown on the status line. A `test_edit` case covers the range
yank, the single-byte yank, paste, and a newline paste; verified live (selecting
three bytes, yanking, and pasting them before the trailing newline).

**H5 (save/quit through the shared framework, DONE).** The hex view saves and
quits through the editor's one request framework rather than a parallel path.
Previously the main loop short-circuited hex keys (`hex_key(); continue;`), so
the hex view could not reach the save/quit handling and had no way to write the
buffer except the File menu. `hex_key` now returns an `enum req` like
`vi_dispatch` does, Ctrl-S returns `REQ_SAVE` and Ctrl-Q returns `REQ_QUIT`, and
a new `run_req` helper carries out `REQ_FIND`/`REQ_GOTO`/`REQ_HELP`/`REQ_SAVE`/
`REQ_QUIT` for both the text (modeless) and hex views. Save funnels through the
single `save_editor`, and quit through the single `confirm_save` dialog, so the
two views behave identically around the one backing buffer. The text view's
dispatch tail was refactored onto `run_req` too, removing the duplicated switch.
Verified live: from the hex view Ctrl-S wrote an edited byte to disk and cleared
the dirty flag, and Ctrl-Q on a modified buffer raised the same save-changes
dialog as the text view.

With H1 through H5 shipped, the hex view covers a read-only dump, overwrite and
structural editing, text and hex-byte search, a configurable row width, a data
inspector, byte copy/paste, and save/quit, all sharing the text view's
framework around the backing buffer. The hex view is now a peer of the modeless
and vi personalities: its own key interpretation over the common editor core.

**Files to change.** `src/cmd/edit/editor.h` (a `hex_view` bit on
`struct editor`/`struct ebuf`, mirrored by `buf_save`/`buf_load`), a new
`src/cmd/edit/hex.c` for the offset<->position helpers, the dump renderer, and
the hex key handler (kept out of `edit.c`/`vi.c` behind `editor.h`, matching
the `vi.c` split), `src/cmd/edit/module.mk` to compile it, `test_edit.c` for
the offset mapping and the dump formatter, and `doc/lumi.1.in` for the mode and
its keys.

### Suggested order

F1 (tests) -> F2 (`vi.c` split) -> V1 (visual mode) -> V2/V3 (text objects,
registers) alongside B1 (async build) -> F3 (multi-buffer) -> B2 and the ex
`:e`/`:r` work -> the remaining vi, syntax, and chrome refinements, then the
hex editor mode, as they are wanted.

## Rendering / Drawing Abstraction

A backend-neutral drawing layer (`libdraw`) is proposed so the apps and editor
draw into a cell grid through a driver seam instead of emitting raw terminal
escapes, which would let a graphical backend replace the terminal one day. The
design (surface API, `struct draw_driver` vtable, terminal driver reusing
`librender`/`libtxl`/`libtio`, input via `tkbd_seq`, and an adoption roadmap) is
in `doc/DRAW.md`. Status: `libdraw` and `libdraw_term` have landed with a
mock-driven `test_draw` suite, and `edit`, `files`, and `splash` are ported onto
the surface. The driver also owns SIGWINCH and SIGTSTP and delivers resize and
suspend as `draw_wait` events, so the apps carry no signal code. Remaining:
re-express the pad-stack `tui_backend` on `draw_driver` and migrate attach's
overlay apps, then a graphical driver.

---

[1]: https://github.com/OrangeTide/the-mechanical-researcher/tree/main/netchan-v2
[2]: https://monocypher.org
[3]: https://github.com/ngtcp2/ngtcp2
[4]: https://github.com/h2o/picotls
