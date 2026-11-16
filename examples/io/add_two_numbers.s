# Reads two integers, prints their sum.
.data
ask:  .string "Enter two numbers: "
out:  .string "Sum = "
nl:   .string "\n"

.text
    la   a0, ask
    li   a7, 4          # print_string
    ecall

    li   a7, 5          # read_int
    ecall
    mv   s0, a0         # first number

    li   a7, 5          # read_int
    ecall
    add  s0, s0, a0     # sum

    la   a0, out
    li   a7, 4
    ecall
    mv   a0, s0
    li   a7, 1          # print_int
    ecall
    la   a0, nl
    li   a7, 4
    ecall

    li   a7, 10         # exit
    ecall
