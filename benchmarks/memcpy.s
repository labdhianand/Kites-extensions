# Copy 32 words from src to dst.
# src at offset 0 and dst at 128 from the start of the data section (0x10000000).
.data
src: .word 7, 14, 21, 28, 35, 42, 49, 56, 63, 70, 77, 84, 91, 98, 105, 112
     .word 119, 126, 133, 140, 147, 154, 161, 168, 175, 182, 189, 196, 203, 210, 217, 224
dst: .zero 128

.text
    lui  x10, 0x10000        # &src
    addi x11, x0, 32         # n
    addi x12, x0, 0          # i
loop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x16, 0(x15)         # src[i]
    sw   x16, 128(x15)       # dst[i]
    addi x12, x12, 1
    blt  x12, x11, loop
