/* ---------------------------------------------------------------------------------- */
/* Assembler frames for C interrupt servers that need to return with the Z flag set.  */
/* ---------------------------------------------------------------------------------- */
    .globl  _dev_isr
_dev_isr:
    jbsr   _cdev_isr
    moveq.l #0,d0       /* Set Z flag to continue to process other servers */
    rts

    .globl  _dev_sisr
_dev_sisr:
	move.l  a6,-(sp)    /* software interrupts must preserve A6 */
    jbsr   _cdev_sisr
    move.l  (sp)+,a6
    moveq.l #0,d0       /* Set Z flag to continue to process other servers */
    rts

