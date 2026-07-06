/* entry.S - kernel entry point and interrupt stubs.
 * The loader jumps here (SysV ABI) with rdi = SbosBootInfo*. */

.section .text
.global _start
_start:
    cli
    cld
    /* We still run on the firmware's page tables; strict memory-protection
     * firmware may have mapped parts of our allocation read-only. WP=0 lets
     * ring 0 write through RO mappings until we build our own tables. */
    movq  %cr0, %rax
    andq  $~(1<<16), %rax
    movq  %rax, %cr0
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

/* ---- interrupt stubs ----------------------------------------------------
 * One 16-byte stub per vector, all 256 of them: real firmware leaves local
 * APIC sources (timer, thermal, error) armed at vectors >= 48, and an
 * unhandled vector must be ignored, not become a #NP panic. C locates stub
 * N at vector_stubs + N*16.
 * Vectors without a CPU error code push a fake 0 so the frame is uniform:
 *   [r15..rax][vec][err][rip][cs][rflags][rsp][ss]                        */
.align 16
.global vector_stubs
vector_stubs:
.set i, 0
.rept 256
  .align 16
  .if (i==8)||(i==10)||(i==11)||(i==12)||(i==13)||(i==14)||(i==17)||(i==21)||(i==30)
    pushq $i                        /* CPU already pushed the error code */
  .else
    pushq $0
    pushq $i
  .endif
    jmp   isr_common_stub
  .set i, i+1
.endr

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
    cld                             /* SysV requires DF=0; delivery doesn't clear it */
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

/* ---- ring 3 ------------------------------------------------------------
 * user_enter(entry, user_rsp): save the kernel context setjmp-style, then
 * iretq down to ring 3. Returns only when user_abort(code) longjmps back -
 * from the exit syscall, or from the fault handler killing the process.
 * The exit path abandons whatever stack it was on (syscall or exception
 * frame); nothing up there owns resources, so that's clean by design. */
.global user_enter
user_enter:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, kctx_rsp(%rip)
    pushq $0x1B                     /* user SS  (0x18 | RPL 3) */
    pushq %rsi                      /* user stack */
    pushq $0x202                    /* RFLAGS: IF set */
    pushq $0x23                     /* user CS  (0x20 | RPL 3) */
    pushq %rdi                      /* entry point */
    xorl  %eax, %eax                /* leak nothing into ring 3 */
    xorl  %ebx, %ebx
    xorl  %ecx, %ecx
    xorl  %edx, %edx
    xorl  %esi, %esi
    xorl  %edi, %edi
    xorl  %ebp, %ebp
    xorl  %r8d, %r8d
    xorl  %r9d, %r9d
    xorl  %r10d, %r10d
    xorl  %r11d, %r11d
    xorl  %r12d, %r12d
    xorl  %r13d, %r13d
    xorl  %r14d, %r14d
    xorl  %r15d, %r15d
    iretq

.global user_abort
user_abort:
    movq  kctx_rsp(%rip), %rsp
    movq  %rdi, %rax                /* user_enter's return value */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    sti                             /* exception entry left IF off */
    ret

/* syscall arrives with rcx = user rip, r11 = user rflags, rsp = USER stack,
 * IF masked by FMASK. Switch stacks first, then mirror Linux's contract:
 * everything preserved except rax (result), rcx, r11. */
.global syscall_entry
syscall_entry:
    movq  %rsp, usr_rsp(%rip)
    leaq  sstack_top(%rip), %rsp
    pushq usr_rsp(%rip)
    pushq %rcx
    pushq %r11
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    subq  $8, %rsp                  /* 16-byte alignment for the call */
    sti
    movq  %rdx, %rcx                /* dispatch(nr, a, b, c) */
    movq  %rsi, %rdx
    movq  %rdi, %rsi
    movq  %rax, %rdi
    call  syscall_dispatch
    cli
    addq  $8, %rsp
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rdx
    popq  %rsi
    popq  %rdi
    popq  %r11
    popq  %rcx
    popq  %rsp                      /* back onto the user stack */
    sysretq

.section .bss
.align 16
kstack:
    .skip 65536
kstack_top:
.align 16
.global sstack_top
sstack:                             /* ring0 stack for syscalls + ring3 IRQs */
    .skip 16384
sstack_top:
kctx_rsp:
    .skip 8
usr_rsp:
    .skip 8

/* nothing here needs an executable stack */
.section .note.GNU-stack,"",@progbits
