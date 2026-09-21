.syntax unified
.thumb

/* Called immediately after loading MSP, before any stack-using calls.
 * No data/BSS dependencies; also covers constructors and initialization.
 * Each core/image paints only its own linked stack, never retained RAM. */
.section .text.daveos_stack_paint,"ax",%progbits
.global daveos_stack_paint
.type daveos_stack_paint, %function
.thumb_func
daveos_stack_paint:
  ldr r0, =_sstack
  ldr r1, =_estack
  ldr r2, =0xa5a5a5a5
1:
  cmp r0, r1
  bhs 2f
  str r2, [r0], #4
  b 1b
2:
  bx lr
.size daveos_stack_paint, .-daveos_stack_paint
