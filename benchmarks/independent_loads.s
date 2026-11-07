# Eight independent products, written the "natural" way: each value is used right after it is
# loaded. Straight-line code, so all the gain comes from reordering within one block.
# The inputs are 64 bytes apart (different cache lines) from the start of the data section.
.data
inputs: .word 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .word 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
result: .word 0

.text
    lui  x10, 0x10000
    lw   x1, 0(x10)
    mul  x1, x1, x1
    lw   x2, 64(x10)
    mul  x2, x2, x2
    lw   x3, 128(x10)
    mul  x3, x3, x3
    lw   x4, 192(x10)
    mul  x4, x4, x4
    lw   x5, 256(x10)
    mul  x5, x5, x5
    lw   x6, 320(x10)
    mul  x6, x6, x6
    lw   x7, 384(x10)
    mul  x7, x7, x7
    lw   x8, 448(x10)
    mul  x8, x8, x8
    add  x1, x1, x2
    add  x3, x3, x4
    add  x5, x5, x6
    add  x7, x7, x8
    add  x1, x1, x3
    add  x5, x5, x7
    add  x1, x1, x5
    sw   x1, 512(x10)        # result
