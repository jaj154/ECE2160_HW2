	.file	"kernel_mul.c"
	.text
	.globl	kernel_scale
	.type	kernel_scale, @function
kernel_scale:
.LFB0:
	.cfi_startproc
	endbr64
	movq	$64, -8(%rsp)
	testl	%esi, %esi
	je	.L4
	movq	%rdi, %rax
	movl	%esi, %esi
	leaq	(%rdi,%rsi,4), %rdi
	movl	$0, %ecx
.L3:
	movq	-8(%rsp), %rsi
	movl	(%rax), %edx
	imulq	%rsi, %rdx
	addq	%rdx, %rcx
	addq	$4, %rax
	cmpq	%rdi, %rax
	jne	.L3
.L1:
	movq	%rcx, %rax
	ret
.L4:
	movl	$0, %ecx
	jmp	.L1
	.cfi_endproc
.LFE0:
	.size	kernel_scale, .-kernel_scale
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
