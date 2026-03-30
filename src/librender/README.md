# librender

Differential screen renderer. `render_full` writes the entire grid to the
terminal, while `render_diff` outputs only the cells that changed since the
last render, minimizing I/O on each update.
