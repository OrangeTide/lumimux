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
| `doc/lumi.1`, `doc/DEV.md` | Document the subcommand, flags, config, ACL file, and protocol |
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

[1]: https://github.com/OrangeTide/the-mechanical-researcher/tree/main/netchan-v2
[2]: https://monocypher.org
[3]: https://github.com/ngtcp2/ngtcp2
[4]: https://github.com/h2o/picotls
