# Memory-mapped keyboard and display: polls for a key, echoes it, counts the keys, and stops at
# 'q'. A polling loop that waits on a device, as in the project plan. Reordering must not swap the
# status read, the data read and the echo: the device registers have effects of their own.
# Input: poll_echo.in.
.text
    lui  x5, 0x20000         # the console: 0x20000000
    addi x6, x0, 113         # 'q'
    addi x7, x0, 0           # keys seen
wait:
    lw   x28, 0(x5)          # KBD_STATUS
    andi x28, x28, 1
    beq  x28, x0, wait       # poll until a key is waiting
    lw   x29, 4(x5)          # KBD_DATA: take the key
    sw   x29, 12(x5)         # DISP_DATA: echo it
    addi x7, x7, 1
    bne  x29, x6, wait
    addi x10, x7, 0
    addi x17, x0, 1          # print_int: how many keys
    ecall
