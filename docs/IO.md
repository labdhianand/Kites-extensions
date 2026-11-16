# Program input and output

Programs talk to the user in two ways, on all five processor models (`rvss` and the four
5-stage pipelines): **`ecall` syscalls**, and a **memory-mapped keyboard and display** for
programs that poll. Examples are in `examples/io/`.

Everything goes through one `IoChannel` (`src/processor/io_channel.h`). The GUI's Console tab,
the CLI's stdin/stdout and the tests are different users of the same channel.

## Syscalls

Put the number in `a7`, arguments in `a0`-`a2` / `fa0`, then `ecall`. Results come back in `a0`
(integers, characters) or `fa0` (floats).

| a7 | Name | Arguments | Result |
|----|------|-----------|--------|
| 1 | print_int | `a0` = value (signed) | |
| 2 | print_float | `fa0` | |
| 3 | print_double | `fa0` | |
| 4 | print_string | `a0` = address of NUL-terminated string | |
| 5 | read_int | | `a0` |
| 6 | read_float | | `fa0` |
| 7 | read_double | | `fa0` |
| 8 | read_string | `a0` = buffer, `a1` = size | line stored NUL-terminated (at most size-1 chars); `a0` = chars read |
| 10 | exit | | |
| 11 | print_char | `a0` = character | |
| 12 | read_char | | `a0` (-1 at end of input) |
| 63 | read | `a0` = 0, `a1` = buffer, `a2` = count | `a0` = bytes read |
| 64 | write | `a0` = 1 or 2, `a1` = buffer, `a2` = count | `a0` = bytes written |
| 93 | exit | `a0` = exit code | |

Reads take white-space separated words from the input, so `3 4` typed on one line feeds two
`read_int`s. A word that is not a number reads as 0. An unknown number does nothing.

**Change from before:** `print_float`/`print_double` now take their value from `fa0`, not from the
bits of `a0`. `exit` no longer ends the whole process; it ends the program and leaves registers
and memory to be inspected.

### How the pipelines run an ecall

An ecall does its work when it **retires** (write-back), so `a7`/`a0` are final whatever the
hazard handling, and the NH_* pipelines need no extra padding around it. The instructions fetched
behind it have already read their registers, so they are flushed and fetched again, like the
wrong path after a taken branch (a real core takes a trap for the same reason). An ecall therefore
costs about four cycles in the pipelines.

## Memory-mapped console

For programs that wait for a key by polling. Accesses go straight to the device and never touch
the caches.

| Address | Register | Access | Meaning |
|---------|----------|--------|---------|
| `0x20000000` | KBD_STATUS | read | bit 0 = a character is waiting |
| `0x20000004` | KBD_DATA | read | next character (0 if none); reading consumes it |
| `0x20000008` | DISP_STATUS | read | bit 0 = ready (always 1) |
| `0x2000000C` | DISP_DATA | write | low byte is printed |

`lui t0, 0x20000` reaches the base. See `examples/io/echo_chars.s`.

## Undo and redo

Undo takes back what the undone step printed and gives back the input it consumed; redo prints
and consumes again without running the program a second time. Undoing an `exit` lets the program
continue.

## GUI

The **Console** tab shows the output and has a line for typing input. When a program running
asks for input the window switches to it. Stepping with the Step button does not wait: a step
that needs input does nothing until the input is typed. Stop clears the console.

## CLI

Output goes to stdout and input comes from stdin (see [CLI.md](CLI.md)). At the end of stdin
reads return 0 / -1 / an empty string, but a program polling KBD_STATUS would wait for ever.

## Examples

`examples/io/`: `hello.s`, `add_two_numbers.s`, `stats.s` (float input) and `echo_chars.s`
(polling). The NH_* pipelines leave data hazards to the programmer, so on `rv5-nh-nf` / `rv5-nh-f`
these need `nop`s between dependent instructions like any other program.
