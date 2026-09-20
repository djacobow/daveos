.syntax unified
.cpu cortex-m33
.thumb

/* Preserve the pre-exception frame before moving to the reserved stack. */
.section .text.IWDG_IRQHandler,"ax",%progbits
.global IWDG_IRQHandler
.type IWDG_IRQHandler, %function
.thumb_func
IWDG_IRQHandler:
  tst lr, #4
  ite eq
  mrseq r0, msp
  mrsne r0, psp
  mov r1, lr
  movs r2, #4
  ldr r3, =0x20000100
  msr msplim, r3
  ldr r3, =0x20000800
  msr msp, r3
  b DaveOS_FaultCapture
.size IWDG_IRQHandler, .-IWDG_IRQHandler
