# src/libnet provenance

This directory is the reliable-UDP transport for lumi's networked
connections (see [FUTURE.md 11C][1]). The `netchan`/`nc_*` files were
extracted from the netchan-v2 demo and are now verbatim copies of it.
The lumi-original glue that adapts netchan to the `ipc_transport` seam is
listed separately below.

## What was copied

| File | Role | Upstream |
|------|------|----------|
| `netchan.c` / `netchan.h` / `nc_addr.h` | transport-agnostic protocol core | netchan-v2 demo, top level |
| `nc_udp.c` / `nc_udp.h` | UDP backend (the only code that knows `sockaddr`) | netchan-v2 demo, top level |
| `nc_crypto.c` / `nc_crypto.h` | encrypted-UDP decorator (X25519 + XChaCha20-Poly1305) | netchan-v2 demo, top level |
| `third_party/monocypher.c` / `monocypher.h` | crypto primitives used by `nc_crypto` | Monocypher 4.0.2, vendored via the demo |

The core (`netchan` + `nc_udp`) has no third-party dependency.
`nc_crypto` is the only part that reaches outside, and only into
Monocypher.

## lumi-original files (not from the demo)

`ipc_transport_netchan.{c,h}` and `test_ipc_transport_net.c` are lumi
code, not extracted from netchan-v2. They adapt netchan to lumi's
`ipc_transport` seam (see [FUTURE.md 11C step 4][1]): a reliable channel
carries `ipc_msg`-framed TLV, chunked to netchan's per-message limit and
reassembled on receive. They depend on `lu_ipc`, which is why the module
now sets `lu_net_LIBS = lu_ipc`.

## What was left behind

The demo's game, its WebSocket (`nc_ws`) and WebRTC (`nc_rtc`) backends,
its examples, and its own test harness were not copied. The WebSocket
and WebRTC backends are only needed for a future browser client and can
be added the same way when that work starts.

## Local modifications

None. The `netchan`/`nc_*` files match the upstream demo byte for byte.

Earlier lumi carried one local `netchan.c` fix: the reliable channel
never retransmitted a lost message, because `send_next` advanced
`out_head` (the oldest-unacked cursor) when a message was *transmitted*
rather than when it was *acked*, so a dropped reliable message fell
behind `out_head` where nothing could resend it. lumi's stress test
(`test_netchan.c`) surfaced it; the demo has no packet-loss tests. That
fix has since been taken upstream (upstream factored the in-flight window
scan into a `chan_next_sendable` helper used by both the DATA send loop
and the `has_data` early-out), so this vendored copy no longer diverges.

When re-syncing with a newer netchan, replace the `netchan`/`nc_*` files
wholesale and re-run `test_netchan` (loss/reorder stress) to confirm the
reliable channel still delivers byte-exact and in order.

## Licensing

The netchan files carry a `PUBLIC DOMAIN (CC0-1.0)` tag. Monocypher is
dual-licensed (2-clause BSD or CC0); its files retain their upstream
license headers and must not be edited to fit lumi's C style. Update
Monocypher by replacing the files under `third_party/` wholesale.

[1]: ../../doc/FUTURE.md
