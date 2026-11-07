# Sum the values of a 16-node linked list. Each node is {value, next}; next is an absolute
# address (0x10000000 + 8 * index) and 0 ends the list.
# Every load needs the previous one's result, so there is nothing to overlap: a case where
# reordering cannot help.
.data
nodes: .word 1, 268435464,  2, 268435472,  3, 268435480,  4, 268435488
       .word 5, 268435496,  6, 268435504,  7, 268435512,  8, 268435520
       .word 9, 268435528, 10, 268435536, 11, 268435544, 12, 268435552
       .word 13, 268435560, 14, 268435568, 15, 268435576, 16, 0
result: .word 0

.text
    lui  x10, 0x10000        # &nodes
    addi x5, x10, 0          # current node
    addi x13, x0, 0          # sum
loop:
    lw   x6, 0(x5)           # value
    add  x13, x13, x6
    lw   x5, 4(x5)           # next
    bne  x5, x0, loop
    sw   x13, 128(x10)       # result
