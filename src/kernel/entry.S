/* entry.S - kernel entry point and interrupt stubs.
 * The loader jumps here (SysV ABI) with rdi = SbosBootInfo*. */

.section .text
.global _start
_start:
    cli
    cld
    leaq  kstack_top(%rip), %rsp
    andq  $-16, %rsp
    xorq  %rbp, %rbp
    call  kmain                     /* rdi already carries the boot info */
1:  hlt
    jmp   1b

/* load our GDT and reload all segment registers; rdi = &gdtr */
.global gdt_flush
gdt_flush:
    lgdt  (%rdi)
    movw  $0x10, %ax                /* data selector */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    movw  %ax, %fs
    movw  %ax, %gs
    pushq $0x08                     /* code selector */
    leaq  1f(%rip), %rax
    pushq %rax
    lretq
1:  ret

/* ---- interrupt stubs ---------------------------------------------------
 * Vectors without a CPU error code push a fake 0 so the frame is uniform:
 *   [r15..rax][vec][err][rip][cs][rflags][rsp][ss]                        */
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp   isr_common_stub
.endm
.macro ISR_ERR n
.global isr\n
isr\n:
    pushq $\n
    jmp   isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

isr_common_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, %rdi
    call  isr_common
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rbp
    popq  %rdi
    popq  %rsi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax
    addq  $16, %rsp                 /* vec + err */
    iretq

.section .bss
.align 16
kstack:
    .skip 65536
kstack_top:

/* nothing here needs an executable stack */
.section .note.GNU-stack,"",@progbits
