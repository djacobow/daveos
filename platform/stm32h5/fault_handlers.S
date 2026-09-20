.syntax unified
.cpu cortex-m33
.thumb

/* Use a reserved emergency stack even if the interrupted MSP hit MSPLIM. */
.macro FAULT_HANDLER name, kind
.section .text.\name,"ax",%progbits
.global \name
.type \name, %function
.thumb_func
\name:
  tst lr, #4
  ite eq
  mrseq r0, msp
  mrsne r0, psp
  mov r1, lr
  movs r2, #\kind
  ldr r3, =0x20000100
  msr msplim, r3
  ldr r3, =0x20000800
  msr msp, r3
  b DaveOS_FaultCapture
.size \name, .-\name
.endm

FAULT_HANDLER HardFault_Handler, 0
FAULT_HANDLER MemManage_Handler, 1
FAULT_HANDLER BusFault_Handler, 2
FAULT_HANDLER UsageFault_Handler, 3
