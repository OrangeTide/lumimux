# TODO

- [x] when spawning internal commands, update argv[0] so that ps can see the
      difference between lumi and an mserver session.

- [ ] Networked client connections over QUIC reliable stream.
      Swap Unix socket for QUIC transport in libipc -- existing TLV message
      protocol works unchanged. Gets encrypted roaming (survives IP/network
      changes), TLS 1.3, 0-RTT reconnect. Server listens on both Unix socket
      (local) and QUIC (remote). Auth via pre-shared key or SSH key challenge.
