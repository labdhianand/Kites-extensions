# Prints a greeting with the print_string syscall, then exits.
.data
msg: .string "Hello from Kites!\n"

.text
    la   a0, msg
    li   a7, 4          # print_string
    ecall
    li   a7, 10         # exit
    ecall
