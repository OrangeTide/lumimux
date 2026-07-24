# Future Work

This file tracks planned and in-progress feature work. Phase 11 grouped four
advanced features; three have shipped and one remains.

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

## Implementation Order

```
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
