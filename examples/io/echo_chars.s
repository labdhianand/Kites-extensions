# Memory-mapped I/O: waits for a key by polling the keyboard status register, echoes it to the
# display register, and stops at 'q'. Uses no syscalls except exit.
#   0x20000000 KBD_STATUS (bit 0 = key waiting)   0x20000004 KBD_DATA
#   0x20000008 DISP_STATUS                        0x2000000C DISP_DATA
.text
    lui  t0, 0x20000        # t0 = 0x20000000
    li   t3, 113            # 'q'
wait:
    lw   t1, 0(t0)          # KBD_STATUS
    andi t1, t1, 1
    beq  t1, x0, wait       # poll until a key is waiting
    lw   t2, 4(t0)          # KBD_DATA: take the key
    sw   t2, 12(t0)         # DISP_DATA: show it
    bne  t2, t3, wait
    li   a7, 10
    ecall
