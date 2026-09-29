	.file	"asm_kernel.c"
	.text
	.p2align 4
	.globl	kernel_scale_mul
	.type	kernel_scale_mul, @function
kernel_scale_mul:
.LFB0:
	.cfi_startproc
	endbr64
	movl	$64, %ecx
	testl	%esi, %esi
	je	.L4
	movl	%esi, %esi
	xorl	%edx, %edx
	leaq	(%rdi,%rsi,4), %rsi
	.p2align 4,,10
	.p2align 3
.L3:
	movl	(%rdi), %eax
	addq	$4, %rdi
	imulq	%rcx, %rax
	addq	%rax, %rdx
	cmpq	%rdi, %rsi
	jne	.L3
	movq	%rdx, %rax
	ret
	.p2align 4,,10
	.p2align 3
.L4:
	xorl	%edx, %edx
	movq	%rdx, %rax
	ret
	.cfi_endproc
.LFE0:
	.size	kernel_scale_mul, .-kernel_scale_mul
	.p2align 4
	.globl	kernel_scale_shift
	.type	kernel_scale_shift, @function
kernel_scale_shift:
.LFB1:
	.cfi_startproc
	endbr64
	testl	%esi, %esi
	je	.L10
	movl	%esi, %esi
	xorl	%edx, %edx
	leaq	(%rdi,%rsi,4), %rcx
	.p2align 4,,10
	.p2align 3
.L9:
	movl	(%rdi), %eax
	addq	$4, %rdi
	salq	$6, %rax
	addq	%rax, %rdx
	cmpq	%rdi, %rcx
	jne	.L9
	movq	%rdx, %rax
	ret
	.p2align 4,,10
	.p2align 3
.L10:
	xorl	%edx, %edx
	movq	%rdx, %rax
	ret
	.cfi_endproc
.LFE1:
	.size	kernel_scale_shift, .-kernel_scale_shift
	.ident	"GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	1f - 0f
	.long	4f - 1f
	.long	5
0:
	.string	"GNU"
1:
	.align 8
	.long	0xc0000002
	.long	3f - 2f
2:
	.long	0x3
3:
	.align 8
4:
