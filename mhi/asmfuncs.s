#----------------------------------------------------------------------------------
# Assembler frames for C interrupt servers that need to return with the Z flag set.
#----------------------------------------------------------------------------------
    .globl  _dev_isr
_dev_isr:
    jbsr   _cdev_isr
    move.l #0,d0
    rts

