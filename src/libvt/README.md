# libvt

Virtual terminal emulation. Provides a grid buffer with scrollback
(`vt_buf`), a cell structure carrying codepoint, foreground/background color,
and attributes such as bold, underline, italic, blink, reverse, and undercurl
(`vt_cell`), and a state-machine parser for VT escape sequences (`vt_parse`).

## Tests

`test_vt` holds the targeted behavior tests. `test_vt_torture` drives the
parser and terminal state with a seeded stream of random and crafted input,
then checks structural invariants after every operation. Both run under
`make run-tests`.

The torture test is meant to run under the sanitizers and the coverage build,
where a crash, a memory error, a signed-overflow report, or a tripped
invariant marks a defect:

```sh
make SANITIZE=address,undefined test_vt_torture
_out/<triplet>/san-address+undefined/bin/test_vt_torture [seed] [iterations]

make VARIANT=coverage CFLAGS='-O0 -g --coverage' LDFLAGS=--coverage \
    test_vt_torture
```

The run is reproducible: every byte comes from the seed, and a failing run
prints the seed and iteration so it can be replayed.
