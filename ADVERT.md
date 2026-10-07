# ⚡ lumimux — The Next-Gen Ultra-Lightweight Terminal Multiplexer

> **GNU Screen ergonomics meet modern terminal tech, packaged into a single static binary of under a megabyte with zero runtime dependencies.**

---

```
    __                      _                     
   / /_  ______ ___  _ _____  ____  __  __  __   
  / / / / / __ `__ \| / __  |/ __ \/ / / / |/_/   
 / / /_/ / / / / / /| / /_/ / / / / /_/ />  <     
/_/\__,_/_/ /_/ /_/_|_\__,_/_/ /_/\__,_/_/|_|     
                                                 
        "The micro-server multiplexer designed for 2026"
```

---

## 🚀 Why lumimux?

While traditional multiplexers rely on massive, monolithic, single-point-of-failure background daemons, **lumimux** completely re-engineers terminal multiplexing from the ground up. By combining a **micro-server architecture** with cutting-edge terminal protocols, lumimux delivers unmatched reliability, rich visual modes, and near-zero latency.

### ✨ Killer Features

* **🔮 Speculative Local Echo (Zero-Latency SSH)**
  Tired of keyboard lag over high-ping SSH connections? lumimux predicts and renders characters locally on the client immediately, validating or rolling them back when the server confirms. Your SSH sessions will feel like they are running locally!
  
* **🛡️ Micro-Server Reliability (One PTY, One Process)**
  No more losing all your windows because a single server process crashed. In lumimux, each window is backed by an independent `lumi-mserver` process. A failure in one window leaves all others perfectly intact.
  
* **🎨 Three Visual Modes (Including "Turbo" Overlapping GUI)**
  * **Screen:** Traditional split-pane tiling (GNU Screen style).
  * **Minimal:** A completely transparent, lightweight passthrough mode.
  * **Turbo:** Overlapping terminal windows with mouse-driven drag, resize, minimize, and maximize—inspired by Borland's classic *Turbo Vision*!
  
* **🖼️ Full SIXEL & Kitty Graphics**
  Render complex images, plots, and terminal graphics inline. lumimux automatically forwards SIXEL and kitty graphics when a window is focused.
  
* **🖱️ First-Class Mouse Support**
  Navigate your workspace with zero friction. Click tabs on the taskbar to switch windows instantly, drag and resize windows in Turbo Mode, and click interactive buttons or approval prompts directly inside CLI tools (like `claude-cli`).
  
* **🔔 Kitty Notification Integration (AI-Ready)**
  Stay informed of background events. lumimux integrates with Kitty desktop notifications to deliver toast alerts instantly to your host environment—perfect for knowing exactly when an AI agent is waiting for your approval or a background task completes.

* **⌨️ kitty Keyboard Protocol Support**
  Say goodbye to escape-sequence hacks. Enjoy flawless multi-key detection (Shift+Enter, Ctrl+Tab, and modified arrows) inside the multiplexer.
  
* **🗺️ Guided Keystroke Popup Menu**
  New to the keybindings? Press `Ctrl-A` and lumimux displays a beautiful, guided action-menu overlay showing all available actions, keys, and session parameters.

---

## 📊 Feature Comparison

| Feature | `lumimux` ⚡ | `tmux` 🐙 | `GNU Screen` 📺 |
| :--- | :---: | :---: | :---: |
| **Binary Size (musl static)** | **~850 KB stripped** | ~1.5 MB+ | ~1.0 MB+ |
| **Process Isolation** | **Per-Window (Micro-server)** | Monolithic (Single daemon) | Monolithic (Single daemon) |
| **Overlapping Window GUI** | **Yes (Turbo Mode)** | No | No |
| **Speculative Local Echo** | **Yes (Mosh-style)** | No | No |
| **First-Class Mouse Support** | **Yes (Click tabs & CLI prompts)** | Basic (Clicks/Scroll only) | No |
| **AI-Agent Toast Notifications** | **Yes (Kitty Notification)** | No | No |
| **Guided Prefix Key Menu** | **Yes (Popup Menu)** | No | No |
| **SIXEL Graphics Support** | **Yes (DCS Pass-Through)** | Experimental/Complex | No |
| **Kitty Keyboard Protocol** | **Yes (Full CSI u)** | Partial | No |

---

## 🛠️ Quick Start

### 1. Build from Source (C99 compiler & GNU Make)
```sh
make RELEASE=1
alias lumi=_out/x86_64-linux-gnu/release/bin/lumi
```

### 2. Launch a Named Session
```sh
lumi new -s development
```

### 3. Attach Over SSH or Encrypted Netchan
```sh
lumi attach user@remote-host:development
lumi attach -n user@remote-host:development    # encrypted roaming
```

### 🔑 Essential Keybindings (Default Prefix: `Ctrl-A`)
* `Ctrl-A c` — Create a new window
* `Ctrl-A d` — Detach session (leaves processes running safely in the background!)
* `Ctrl-A w` — Interactive window picker
* `Ctrl-A t` — Toggle between **Turbo** and **Screen** UI modes
* `Ctrl-A s` — Toggle taskbar visibility
* `Ctrl-A [` — Enter scrollback history (supports mouse wheel)

---

## 🏗️ Architecture

```
                       [ Discovery Session Directory ]
                                      |
                     +----------------+----------------+
                     |                |                |
                mserver 0        mserver 1        mserver 2
               (PTY 0 + VT)     (PTY 1 + VT)     (PTY 2 + VT)
                     ^                ^                ^
                     |                |                |
                     +----------------+----------------+
                                      |
                  Unix Sockets / SSH / Encrypted Netchan
                                      |
                     +-----+-----+-----v-----+-----+
                     |     |     |           |     |
                  client  client client  view-only client
                (attached,  (shared attach, multiple clients,
                 roaming)   keyboard handover)
```

By decoupling the terminal client from the shell processes (`lumi-mserver`),
lumimux guarantees robustness and keeps your running processes fully isolated.
Multiple clients can attach to the same session with optional view-only mode
and keyboard handover.

---

## 📄 License & Integrity

* **License:** Public Domain / MIT-0 (Dual Licensed).
* **Speed:** Written in pure, high-performance C99 without bloated library dependencies.

**Give your terminal the upgrade it deserves. Switch to lumimux.**
