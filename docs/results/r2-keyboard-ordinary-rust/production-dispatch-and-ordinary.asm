ffffffff80123f70 <keyboard_poll>:
ffffffff80123f70:	8b 47 14             	mov    0x14(%rdi),%eax
ffffffff80123f73:	85 c0                	test   %eax,%eax
ffffffff80123f75:	75 09                	jne    ffffffff80123f80 <keyboard_poll+0x10>
ffffffff80123f77:	e9 24 cd 01 00       	jmp    ffffffff80140ca0 <vos_rust_keyboard_ordinary_poll>
ffffffff80123f7c:	0f 1f 40 00          	nopl   0x0(%rax)
ffffffff80123f80:	e9 db cf 01 00       	jmp    ffffffff80140f60 <vos_rust_keyboard_routed_poll>
ffffffff80123f85:	66 66 2e 0f 1f 84 00 	data16 cs nopw 0x0(%rax,%rax,1)
ffffffff80123f8c:	00 00 00 00 

ffffffff80140ca0 <vos_rust_keyboard_ordinary_poll>:
ffffffff80140ca0:	55                   	push   %rbp
ffffffff80140ca1:	41 57                	push   %r15
ffffffff80140ca3:	41 56                	push   %r14
ffffffff80140ca5:	53                   	push   %rbx
ffffffff80140ca6:	50                   	push   %rax
ffffffff80140ca7:	48 89 fb             	mov    %rdi,%rbx
ffffffff80140caa:	48 8d 47 ff          	lea    -0x1(%rdi),%rax
ffffffff80140cae:	48 83 f8 a7          	cmp    $0xffffffffffffffa7,%rax
ffffffff80140cb2:	0f 93 c0             	setae  %al
ffffffff80140cb5:	f6 c3 07             	test   $0x7,%bl
ffffffff80140cb8:	0f 95 c1             	setne  %cl
ffffffff80140cbb:	08 c1                	or     %al,%cl
ffffffff80140cbd:	75 7a                	jne    ffffffff80140d39 <vos_rust_keyboard_ordinary_poll+0x99>
ffffffff80140cbf:	83 7b 10 00          	cmpl   $0x0,0x10(%rbx)
ffffffff80140cc3:	74 74                	je     ffffffff80140d39 <vos_rust_keyboard_ordinary_poll+0x99>
ffffffff80140cc5:	4c 8d 73 08          	lea    0x8(%rbx),%r14
ffffffff80140cc9:	41 bf 20 00 00 00    	mov    $0x20,%r15d
ffffffff80140ccf:	eb 23                	jmp    ffffffff80140cf4 <vos_rust_keyboard_ordinary_poll+0x54>
ffffffff80140cd1:	66 66 66 66 66 66 2e 	data16 data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
ffffffff80140cd8:	0f 1f 84 00 00 00 00 
ffffffff80140cdf:	00 
ffffffff80140ce0:	41 c7 46 03 00 00 00 	movl   $0x0,0x3(%r14)
ffffffff80140ce7:	00 
ffffffff80140ce8:	41 c7 06 00 00 00 00 	movl   $0x0,(%r14)
ffffffff80140cef:	41 ff cf             	dec    %r15d
ffffffff80140cf2:	74 45                	je     ffffffff80140d39 <vos_rust_keyboard_ordinary_poll+0x99>
ffffffff80140cf4:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140cf7:	bf 64 00 00 00       	mov    $0x64,%edi
ffffffff80140cfc:	ff 10                	call   *(%rax)
ffffffff80140cfe:	a8 01                	test   $0x1,%al
ffffffff80140d00:	74 37                	je     ffffffff80140d39 <vos_rust_keyboard_ordinary_poll+0x99>
ffffffff80140d02:	89 c5                	mov    %eax,%ebp
ffffffff80140d04:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140d07:	bf 60 00 00 00       	mov    $0x60,%edi
ffffffff80140d0c:	ff 10                	call   *(%rax)
ffffffff80140d0e:	40 f6 c5 20          	test   $0x20,%bpl
ffffffff80140d12:	75 db                	jne    ffffffff80140cef <vos_rust_keyboard_ordinary_poll+0x4f>
ffffffff80140d14:	40 80 fd 40          	cmp    $0x40,%bpl
ffffffff80140d18:	0f 92 c1             	setb   %cl
ffffffff80140d1b:	8d 50 ff             	lea    -0x1(%rax),%edx
ffffffff80140d1e:	80 fa fe             	cmp    $0xfe,%dl
ffffffff80140d21:	0f 92 c2             	setb   %dl
ffffffff80140d24:	84 d1                	test   %dl,%cl
ffffffff80140d26:	74 b8                	je     ffffffff80140ce0 <vos_rust_keyboard_ordinary_poll+0x40>
ffffffff80140d28:	0f b6 f0             	movzbl %al,%esi
ffffffff80140d2b:	4c 89 f7             	mov    %r14,%rdi
ffffffff80140d2e:	e8 bd f3 ff ff       	call   ffffffff801400f0 <keyboard_decode>
ffffffff80140d33:	84 c0                	test   %al,%al
ffffffff80140d35:	74 b8                	je     ffffffff80140cef <vos_rust_keyboard_ordinary_poll+0x4f>
ffffffff80140d37:	eb 02                	jmp    ffffffff80140d3b <vos_rust_keyboard_ordinary_poll+0x9b>
ffffffff80140d39:	31 c0                	xor    %eax,%eax
ffffffff80140d3b:	48 83 c4 08          	add    $0x8,%rsp
ffffffff80140d3f:	5b                   	pop    %rbx
ffffffff80140d40:	41 5e                	pop    %r14
ffffffff80140d42:	41 5f                	pop    %r15
ffffffff80140d44:	5d                   	pop    %rbp
ffffffff80140d45:	c3                   	ret
ffffffff80140d46:	cc                   	int3
ffffffff80140d47:	cc                   	int3
ffffffff80140d48:	cc                   	int3
ffffffff80140d49:	cc                   	int3
ffffffff80140d4a:	cc                   	int3
ffffffff80140d4b:	cc                   	int3
ffffffff80140d4c:	cc                   	int3
ffffffff80140d4d:	cc                   	int3
ffffffff80140d4e:	cc                   	int3
ffffffff80140d4f:	cc                   	int3
