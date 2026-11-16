# Reads a count n, then n floats, and prints their sum. Shows float I/O.
.text
    li   a7, 5          # read_int
    ecall
    mv   t0, a0         # n
    fmv.w.x ft0, x0     # sum = 0.0
loop:
    beq  t0, x0, done
    li   a7, 6          # read_float -> fa0
    ecall
    fadd.s ft0, ft0, fa0
    addi t0, t0, -1
    j    loop
done:
    fsgnj.s fa0, ft0, ft0
    li   a7, 2          # print_float
    ecall
    li   a0, 10         # newline
    li   a7, 11         # print_char
    ecall
    li   a7, 10
    ecall
