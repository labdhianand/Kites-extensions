# Benchmarks for the reordering optimizer

Small RISC-V programs used to measure the code-reordering optimizer. The measured results are in
[docs/BENCHMARKS.md](../docs/BENCHMARKS.md) (and [docs/benchmarks.csv](../docs/benchmarks.csv)).

| Program | What it does | Why it is here |
|---|---|---|
| `array_sum.s` | Sums 32 words | A load feeding an accumulator chain |
| `vector_add.s` | `c[i] = a[i] + b[i]` | Two independent loads per iteration and a store |
| `dot_product.s` | Dot product of two vectors | Loads feeding a multiply and an accumulator |
| `saxpy.s` | `y[i] = 3 * x[i] + y[i]` | Load, multiply, load, store to the same array |
| `memcpy.s` | Copies 32 words | Load immediately followed by its store |
| `matrix_multiply.s` | 4x4 integer matrix product | Nested loops; only the innermost is unrollable |
| `pointer_chase.s` | Sums a linked list | Every load depends on the previous one: nothing to overlap |
| `independent_loads.s` | Eight independent squares | Straight-line code written in "natural" order |

Every program keeps its data in `.data`, which the simulator loads straight into main memory, so
it starts cold in the caches. Each one reaches its data with `lui x10, 0x10000` (the data section
starts at `0x10000000`) instead of `la`: `la` expands to two dependent instructions that cannot be
padded apart on a pipeline without hazard detection.

## Regenerating the results

`kites-bench` is built with the other targets. It runs every program on all four pipeline
variants, under ideal memory and under a cold cache with load latencies, as written, reordered,
and reordered and unrolled. It checks every version against the single-cycle processor and exits
with an error if any result differs.

```sh
cmake --build build --target kites_bench
./build/kites-bench benchmarks --markdown docs/BENCHMARKS.md --csv docs/benchmarks.csv
```

The simulator is deterministic, so the numbers are the same on every run.
