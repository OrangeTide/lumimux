# libipc

Inter-process communication over Unix domain sockets. `ipc_listen` and
`ipc_connect` create server and client endpoints, and `ipc_msg_send` /
`ipc_msg_recv` provide length-prefixed message framing on top of the raw
socket.
