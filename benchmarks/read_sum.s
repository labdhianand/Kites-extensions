# Reads eight integers from the user, stores them and prints their sum.
# The loop is held together by ecalls, which nothing may be moved across, so there is very little
# for the optimizer to do here; it is in the set to show that nothing breaks (and nothing is gained
# by pretending). Input: read_sum.in.
.data
vals:   .zero 32

.text
    lui  x20, 0x10000        # &vals
    addi x11, x0, 8          # n
    addi x12, x0, 0          # i
    addi x13, x0, 0          # sum
loop:
    addi x17, x0, 5          # read_int -> a0
    ecall
    slli x14, x12, 2
    add  x15, x20, x14
    sw   x10, 0(x15)         # vals[i]
    add  x13, x13, x10
    addi x12, x12, 1
    blt  x12, x11, loop
    addi x10, x13, 0
    addi x17, x0, 1          # print_int
    ecall
