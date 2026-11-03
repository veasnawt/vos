vos_rust_keyboard_routed_poll
ffffffff80140f50:	48 8d 47 ff          	lea    -0x1(%rdi),%rax
ffffffff80140f54:	48 83 f8 a7          	cmp    $0xffffffffffffffa7,%rax
ffffffff80140f58:	0f 93 c0             	setae  %al
ffffffff80140f5b:	40 f6 c7 07          	test   $0x7,%dil
ffffffff80140f5f:	0f 95 c1             	setne  %cl
ffffffff80140f62:	08 c1                	or     %al,%cl
ffffffff80140f64:	75 38                	jne    ffffffff80140f9e <vos_rust_keyboard_routed_poll+0x4e>
ffffffff80140f66:	83 7f 10 00          	cmpl   $0x0,0x10(%rdi)
ffffffff80140f6a:	74 32                	je     ffffffff80140f9e <vos_rust_keyboard_routed_poll+0x4e>
ffffffff80140f6c:	53                   	push   %rbx
ffffffff80140f6d:	48 89 fb             	mov    %rdi,%rbx
ffffffff80140f70:	e8 bb fe ff ff       	call   ffffffff80140e30 <vos_rust_keyboard_routed_drain>
ffffffff80140f75:	8b 4b 30             	mov    0x30(%rbx),%ecx
ffffffff80140f78:	85 c9                	test   %ecx,%ecx
ffffffff80140f7a:	74 25                	je     ffffffff80140fa1 <vos_rust_keyboard_routed_poll+0x51>
ffffffff80140f7c:	8b 53 2c             	mov    0x2c(%rbx),%edx
ffffffff80140f7f:	48 83 fa 1f          	cmp    $0x1f,%rdx
ffffffff80140f83:	77 1c                	ja     ffffffff80140fa1 <vos_rust_keyboard_routed_poll+0x51>
ffffffff80140f85:	0f b6 44 13 34       	movzbl 0x34(%rbx,%rdx,1),%eax
ffffffff80140f8a:	c6 44 13 34 00       	movb   $0x0,0x34(%rbx,%rdx,1)
ffffffff80140f8f:	ff c2                	inc    %edx
ffffffff80140f91:	83 e2 1f             	and    $0x1f,%edx
ffffffff80140f94:	89 53 2c             	mov    %edx,0x2c(%rbx)
ffffffff80140f97:	ff c9                	dec    %ecx
ffffffff80140f99:	89 4b 30             	mov    %ecx,0x30(%rbx)
ffffffff80140f9c:	5b                   	pop    %rbx
ffffffff80140f9d:	c3                   	ret
ffffffff80140f9e:	31 c0                	xor    %eax,%eax
ffffffff80140fa0:	c3                   	ret
ffffffff80140fa1:	31 c0                	xor    %eax,%eax
ffffffff80140fa3:	5b                   	pop    %rbx
ffffffff80140fa4:	c3                   	ret
ffffffff80140fa5:	cc                   	int3
ffffffff80140fa6:	cc                   	int3
ffffffff80140fa7:	cc                   	int3
ffffffff80140fa8:	cc                   	int3
ffffffff80140fa9:	cc                   	int3
ffffffff80140faa:	cc                   	int3
ffffffff80140fab:	cc                   	int3
ffffffff80140fac:	cc                   	int3
ffffffff80140fad:	cc                   	int3
ffffffff80140fae:	cc                   	int3
ffffffff80140faf:	cc                   	int3

keyboard_poll
ffffffff80123f70:	8b 57 14             	mov    0x14(%rdi),%edx
ffffffff80123f73:	85 d2                	test   %edx,%edx
ffffffff80123f75:	0f 85 95 00 00 00    	jne    ffffffff80124010 <keyboard_poll+0xa0>
ffffffff80123f7b:	8b 47 10             	mov    0x10(%rdi),%eax
ffffffff80123f7e:	85 c0                	test   %eax,%eax
ffffffff80123f80:	0f 84 82 00 00 00    	je     ffffffff80124008 <keyboard_poll+0x98>
ffffffff80123f86:	41 55                	push   %r13
ffffffff80123f88:	41 54                	push   %r12
ffffffff80123f8a:	41 bc 20 00 00 00    	mov    $0x20,%r12d
ffffffff80123f90:	55                   	push   %rbp
ffffffff80123f91:	48 89 fd             	mov    %rdi,%rbp
ffffffff80123f94:	53                   	push   %rbx
ffffffff80123f95:	48 83 ec 08          	sub    $0x8,%rsp
ffffffff80123f99:	eb 1b                	jmp    ffffffff80123fb6 <keyboard_poll+0x46>
ffffffff80123f9b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
ffffffff80123fa0:	0f b6 f0             	movzbl %al,%esi
ffffffff80123fa3:	48 8d 7d 08          	lea    0x8(%rbp),%rdi
ffffffff80123fa7:	e8 e4 c1 01 00       	call   ffffffff80140190 <keyboard_decode>
ffffffff80123fac:	84 c0                	test   %al,%al
ffffffff80123fae:	75 4a                	jne    ffffffff80123ffa <keyboard_poll+0x8a>
ffffffff80123fb0:	41 83 ec 01          	sub    $0x1,%r12d
ffffffff80123fb4:	74 42                	je     ffffffff80123ff8 <keyboard_poll+0x88>
ffffffff80123fb6:	48 8b 45 00          	mov    0x0(%rbp),%rax
ffffffff80123fba:	bf 64 00 00 00       	mov    $0x64,%edi
ffffffff80123fbf:	ff 10                	call   *(%rax)
ffffffff80123fc1:	89 c3                	mov    %eax,%ebx
ffffffff80123fc3:	a8 01                	test   $0x1,%al
ffffffff80123fc5:	74 31                	je     ffffffff80123ff8 <keyboard_poll+0x88>
ffffffff80123fc7:	48 8b 45 00          	mov    0x0(%rbp),%rax
ffffffff80123fcb:	bf 60 00 00 00       	mov    $0x60,%edi
ffffffff80123fd0:	ff 10                	call   *(%rax)
ffffffff80123fd2:	f6 c3 20             	test   $0x20,%bl
ffffffff80123fd5:	75 d9                	jne    ffffffff80123fb0 <keyboard_poll+0x40>
ffffffff80123fd7:	80 fb 3f             	cmp    $0x3f,%bl
ffffffff80123fda:	77 08                	ja     ffffffff80123fe4 <keyboard_poll+0x74>
ffffffff80123fdc:	8d 50 ff             	lea    -0x1(%rax),%edx
ffffffff80123fdf:	80 fa fd             	cmp    $0xfd,%dl
ffffffff80123fe2:	76 bc                	jbe    ffffffff80123fa0 <keyboard_poll+0x30>
ffffffff80123fe4:	c7 45 08 00 00 00 00 	movl   $0x0,0x8(%rbp)
ffffffff80123feb:	c7 45 0b 00 00 00 00 	movl   $0x0,0xb(%rbp)
ffffffff80123ff2:	41 83 ec 01          	sub    $0x1,%r12d
ffffffff80123ff6:	75 be                	jne    ffffffff80123fb6 <keyboard_poll+0x46>
ffffffff80123ff8:	31 c0                	xor    %eax,%eax
ffffffff80123ffa:	48 83 c4 08          	add    $0x8,%rsp
ffffffff80123ffe:	5b                   	pop    %rbx
ffffffff80123fff:	5d                   	pop    %rbp
ffffffff80124000:	41 5c                	pop    %r12
ffffffff80124002:	41 5d                	pop    %r13
ffffffff80124004:	c3                   	ret
ffffffff80124005:	0f 1f 00             	nopl   (%rax)
ffffffff80124008:	31 c0                	xor    %eax,%eax
ffffffff8012400a:	c3                   	ret
ffffffff8012400b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
ffffffff80124010:	e9 3b cf 01 00       	jmp    ffffffff80140f50 <vos_rust_keyboard_routed_poll>
ffffffff80124015:	66 66 2e 0f 1f 84 00 	data16 cs nopw 0x0(%rax,%rax,1)
ffffffff8012401c:	00 00 00 00 

vos_rust_keyboard_routed_drain
ffffffff80140e30:	48 85 ff             	test   %rdi,%rdi
ffffffff80140e33:	74 24                	je     ffffffff80140e59 <vos_rust_keyboard_routed_drain+0x29>
ffffffff80140e35:	55                   	push   %rbp
ffffffff80140e36:	41 57                	push   %r15
ffffffff80140e38:	41 56                	push   %r14
ffffffff80140e3a:	41 54                	push   %r12
ffffffff80140e3c:	53                   	push   %rbx
ffffffff80140e3d:	48 89 fb             	mov    %rdi,%rbx
ffffffff80140e40:	f6 c3 07             	test   $0x7,%bl
ffffffff80140e43:	0f 95 c0             	setne  %al
ffffffff80140e46:	48 83 ff a8          	cmp    $0xffffffffffffffa8,%rdi
ffffffff80140e4a:	0f 93 c1             	setae  %cl
ffffffff80140e4d:	08 c1                	or     %al,%cl
ffffffff80140e4f:	74 09                	je     ffffffff80140e5a <vos_rust_keyboard_routed_drain+0x2a>
ffffffff80140e51:	5b                   	pop    %rbx
ffffffff80140e52:	41 5c                	pop    %r12
ffffffff80140e54:	41 5e                	pop    %r14
ffffffff80140e56:	41 5f                	pop    %r15
ffffffff80140e58:	5d                   	pop    %rbp
ffffffff80140e59:	c3                   	ret
ffffffff80140e5a:	4c 8d 7b 1c          	lea    0x1c(%rbx),%r15
ffffffff80140e5e:	4c 8d 73 08          	lea    0x8(%rbx),%r14
ffffffff80140e62:	41 bc 20 00 00 00    	mov    $0x20,%r12d
ffffffff80140e68:	eb 1a                	jmp    ffffffff80140e84 <vos_rust_keyboard_routed_drain+0x54>
ffffffff80140e6a:	66 0f 1f 44 00 00    	nopw   0x0(%rax,%rax,1)
ffffffff80140e70:	41 c7 46 03 00 00 00 	movl   $0x0,0x3(%r14)
ffffffff80140e77:	00 
ffffffff80140e78:	41 c7 06 00 00 00 00 	movl   $0x0,(%r14)
ffffffff80140e7f:	41 ff cc             	dec    %r12d
ffffffff80140e82:	74 cd                	je     ffffffff80140e51 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e84:	83 7b 18 00          	cmpl   $0x0,0x18(%rbx)
ffffffff80140e88:	75 c7                	jne    ffffffff80140e51 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e8a:	83 7b 30 20          	cmpl   $0x20,0x30(%rbx)
ffffffff80140e8e:	74 c1                	je     ffffffff80140e51 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e90:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140e93:	bf 64 00 00 00       	mov    $0x64,%edi
ffffffff80140e98:	ff 10                	call   *(%rax)
ffffffff80140e9a:	a8 01                	test   $0x1,%al
ffffffff80140e9c:	74 b3                	je     ffffffff80140e51 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e9e:	89 c5                	mov    %eax,%ebp
ffffffff80140ea0:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140ea3:	bf 60 00 00 00       	mov    $0x60,%edi
ffffffff80140ea8:	ff 10                	call   *(%rax)
ffffffff80140eaa:	40 f6 c5 20          	test   $0x20,%bpl
ffffffff80140eae:	75 40                	jne    ffffffff80140ef0 <vos_rust_keyboard_routed_drain+0xc0>
ffffffff80140eb0:	40 80 fd 40          	cmp    $0x40,%bpl
ffffffff80140eb4:	0f 92 c1             	setb   %cl
ffffffff80140eb7:	8d 50 ff             	lea    -0x1(%rax),%edx
ffffffff80140eba:	80 fa fe             	cmp    $0xfe,%dl
ffffffff80140ebd:	0f 92 c2             	setb   %dl
ffffffff80140ec0:	84 d1                	test   %dl,%cl
ffffffff80140ec2:	74 ac                	je     ffffffff80140e70 <vos_rust_keyboard_routed_drain+0x40>
ffffffff80140ec4:	0f b6 f0             	movzbl %al,%esi
ffffffff80140ec7:	4c 89 f7             	mov    %r14,%rdi
ffffffff80140eca:	e8 c1 f2 ff ff       	call   ffffffff80140190 <keyboard_decode>
ffffffff80140ecf:	84 c0                	test   %al,%al
ffffffff80140ed1:	74 ac                	je     ffffffff80140e7f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140ed3:	8b 4b 30             	mov    0x30(%rbx),%ecx
ffffffff80140ed6:	8b 53 2c             	mov    0x2c(%rbx),%edx
ffffffff80140ed9:	01 ca                	add    %ecx,%edx
ffffffff80140edb:	83 e2 1f             	and    $0x1f,%edx
ffffffff80140ede:	88 44 13 34          	mov    %al,0x34(%rbx,%rdx,1)
ffffffff80140ee2:	ff c1                	inc    %ecx
ffffffff80140ee4:	89 4b 30             	mov    %ecx,0x30(%rbx)
ffffffff80140ee7:	eb 96                	jmp    ffffffff80140e7f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140ee9:	0f 1f 80 00 00 00 00 	nopl   0x0(%rax)
ffffffff80140ef0:	40 80 fd 40          	cmp    $0x40,%bpl
ffffffff80140ef4:	73 2a                	jae    ffffffff80140f20 <vos_rust_keyboard_routed_drain+0xf0>
ffffffff80140ef6:	0f b6 f0             	movzbl %al,%esi
ffffffff80140ef9:	48 89 df             	mov    %rbx,%rdi
ffffffff80140efc:	e8 bf 77 ff ff       	call   ffffffff801386c0 <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step>
ffffffff80140f01:	85 c0                	test   %eax,%eax
ffffffff80140f03:	78 27                	js     ffffffff80140f2c <vos_rust_keyboard_routed_drain+0xfc>
ffffffff80140f05:	0f 84 74 ff ff ff    	je     ffffffff80140e7f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140f0b:	c7 43 18 01 00 00 00 	movl   $0x1,0x18(%rbx)
ffffffff80140f12:	e9 68 ff ff ff       	jmp    ffffffff80140e7f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140f17:	66 0f 1f 84 00 00 00 	nopw   0x0(%rax,%rax,1)
ffffffff80140f1e:	00 00 
ffffffff80140f20:	41 c6 47 04 00       	movb   $0x0,0x4(%r15)
ffffffff80140f25:	41 c7 07 00 00 00 00 	movl   $0x0,(%r15)
ffffffff80140f2c:	8b 43 28             	mov    0x28(%rbx),%eax
ffffffff80140f2f:	31 c9                	xor    %ecx,%ecx
ffffffff80140f31:	83 f8 ff             	cmp    $0xffffffff,%eax
ffffffff80140f34:	0f 95 c1             	setne  %cl
ffffffff80140f37:	01 c1                	add    %eax,%ecx
ffffffff80140f39:	89 4b 28             	mov    %ecx,0x28(%rbx)
ffffffff80140f3c:	e9 3e ff ff ff       	jmp    ffffffff80140e7f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140f41:	cc                   	int3
ffffffff80140f42:	cc                   	int3
ffffffff80140f43:	cc                   	int3
ffffffff80140f44:	cc                   	int3
ffffffff80140f45:	cc                   	int3
ffffffff80140f46:	cc                   	int3
ffffffff80140f47:	cc                   	int3
ffffffff80140f48:	cc                   	int3
ffffffff80140f49:	cc                   	int3
ffffffff80140f4a:	cc                   	int3
ffffffff80140f4b:	cc                   	int3
ffffffff80140f4c:	cc                   	int3
ffffffff80140f4d:	cc                   	int3
ffffffff80140f4e:	cc                   	int3
ffffffff80140f4f:	cc                   	int3

vos_rust_keyboard_pointer_poll
ffffffff80140d70:	48 85 ff             	test   %rdi,%rdi
ffffffff80140d73:	0f 94 c0             	sete   %al
ffffffff80140d76:	48 85 f6             	test   %rsi,%rsi
ffffffff80140d79:	0f 94 c1             	sete   %cl
ffffffff80140d7c:	08 c1                	or     %al,%cl
ffffffff80140d7e:	40 f6 c7 07          	test   $0x7,%dil
ffffffff80140d82:	0f 95 c2             	setne  %dl
ffffffff80140d85:	31 c0                	xor    %eax,%eax
ffffffff80140d87:	08 ca                	or     %cl,%dl
ffffffff80140d89:	0f 85 9c 00 00 00    	jne    ffffffff80140e2b <vos_rust_keyboard_pointer_poll+0xbb>
ffffffff80140d8f:	48 83 ff a8          	cmp    $0xffffffffffffffa8,%rdi
ffffffff80140d93:	0f 93 c1             	setae  %cl
ffffffff80140d96:	40 08 f1             	or     %sil,%cl
ffffffff80140d99:	48 83 fe fa          	cmp    $0xfffffffffffffffa,%rsi
ffffffff80140d9d:	0f 93 c2             	setae  %dl
ffffffff80140da0:	08 ca                	or     %cl,%dl
ffffffff80140da2:	f6 c2 01             	test   $0x1,%dl
ffffffff80140da5:	0f 85 80 00 00 00    	jne    ffffffff80140e2b <vos_rust_keyboard_pointer_poll+0xbb>
ffffffff80140dab:	83 7f 10 00          	cmpl   $0x0,0x10(%rdi)
ffffffff80140daf:	74 7a                	je     ffffffff80140e2b <vos_rust_keyboard_pointer_poll+0xbb>
ffffffff80140db1:	83 7f 14 00          	cmpl   $0x0,0x14(%rdi)
ffffffff80140db5:	74 74                	je     ffffffff80140e2b <vos_rust_keyboard_pointer_poll+0xbb>
ffffffff80140db7:	41 56                	push   %r14
ffffffff80140db9:	53                   	push   %rbx
ffffffff80140dba:	50                   	push   %rax
ffffffff80140dbb:	49 89 f6             	mov    %rsi,%r14
ffffffff80140dbe:	48 89 fb             	mov    %rdi,%rbx
ffffffff80140dc1:	e8 6a 00 00 00       	call   ffffffff80140e30 <vos_rust_keyboard_routed_drain>
ffffffff80140dc6:	83 7b 18 00          	cmpl   $0x0,0x18(%rbx)
ffffffff80140dca:	74 1d                	je     ffffffff80140de9 <vos_rust_keyboard_pointer_poll+0x79>
ffffffff80140dcc:	8b 43 22             	mov    0x22(%rbx),%eax
ffffffff80140dcf:	0f b7 4b 26          	movzwl 0x26(%rbx),%ecx
ffffffff80140dd3:	c7 43 18 00 00 00 00 	movl   $0x0,0x18(%rbx)
ffffffff80140dda:	66 41 89 4e 04       	mov    %cx,0x4(%r14)
ffffffff80140ddf:	41 89 06             	mov    %eax,(%r14)
ffffffff80140de2:	b8 01 00 00 00       	mov    $0x1,%eax
ffffffff80140de7:	eb 3b                	jmp    ffffffff80140e24 <vos_rust_keyboard_pointer_poll+0xb4>
ffffffff80140de9:	0f b6 4b 1f          	movzbl 0x1f(%rbx),%ecx
ffffffff80140ded:	80 f9 02             	cmp    $0x2,%cl
ffffffff80140df0:	b8 00 00 00 00       	mov    $0x0,%eax
ffffffff80140df5:	77 17                	ja     ffffffff80140e0e <vos_rust_keyboard_pointer_poll+0x9e>
ffffffff80140df7:	84 c9                	test   %cl,%cl
ffffffff80140df9:	74 29                	je     ffffffff80140e24 <vos_rust_keyboard_pointer_poll+0xb4>
ffffffff80140dfb:	0f b6 4b 20          	movzbl 0x20(%rbx),%ecx
ffffffff80140dff:	80 f9 0f             	cmp    $0xf,%cl
ffffffff80140e02:	77 0a                	ja     ffffffff80140e0e <vos_rust_keyboard_pointer_poll+0x9e>
ffffffff80140e04:	fe c1                	inc    %cl
ffffffff80140e06:	88 4b 20             	mov    %cl,0x20(%rbx)
ffffffff80140e09:	80 f9 10             	cmp    $0x10,%cl
ffffffff80140e0c:	75 16                	jne    ffffffff80140e24 <vos_rust_keyboard_pointer_poll+0xb4>
ffffffff80140e0e:	66 c7 43 1f 00 00    	movw   $0x0,0x1f(%rbx)
ffffffff80140e14:	8b 4b 28             	mov    0x28(%rbx),%ecx
ffffffff80140e17:	31 d2                	xor    %edx,%edx
ffffffff80140e19:	83 f9 ff             	cmp    $0xffffffff,%ecx
ffffffff80140e1c:	0f 95 c2             	setne  %dl
ffffffff80140e1f:	01 ca                	add    %ecx,%edx
ffffffff80140e21:	89 53 28             	mov    %edx,0x28(%rbx)
ffffffff80140e24:	48 83 c4 08          	add    $0x8,%rsp
ffffffff80140e28:	5b                   	pop    %rbx
ffffffff80140e29:	41 5e                	pop    %r14
ffffffff80140e2b:	c3                   	ret
ffffffff80140e2c:	cc                   	int3
ffffffff80140e2d:	cc                   	int3
ffffffff80140e2e:	cc                   	int3
ffffffff80140e2f:	cc                   	int3

vos_rust_keyboard_pointer_lost
ffffffff80140d40:	31 c0                	xor    %eax,%eax
ffffffff80140d42:	48 85 ff             	test   %rdi,%rdi
ffffffff80140d45:	74 1c                	je     ffffffff80140d63 <vos_rust_keyboard_pointer_lost+0x23>
ffffffff80140d47:	40 f6 c7 07          	test   $0x7,%dil
ffffffff80140d4b:	0f 95 c1             	setne  %cl
ffffffff80140d4e:	48 83 ff a8          	cmp    $0xffffffffffffffa8,%rdi
ffffffff80140d52:	0f 93 c2             	setae  %dl
ffffffff80140d55:	08 ca                	or     %cl,%dl
ffffffff80140d57:	75 0a                	jne    ffffffff80140d63 <vos_rust_keyboard_pointer_lost+0x23>
ffffffff80140d59:	8b 47 28             	mov    0x28(%rdi),%eax
ffffffff80140d5c:	c7 47 28 00 00 00 00 	movl   $0x0,0x28(%rdi)
ffffffff80140d63:	c3                   	ret
ffffffff80140d64:	cc                   	int3
ffffffff80140d65:	cc                   	int3
ffffffff80140d66:	cc                   	int3
ffffffff80140d67:	cc                   	int3
ffffffff80140d68:	cc                   	int3
ffffffff80140d69:	cc                   	int3
ffffffff80140d6a:	cc                   	int3
ffffffff80140d6b:	cc                   	int3
ffffffff80140d6c:	cc                   	int3
ffffffff80140d6d:	cc                   	int3
ffffffff80140d6e:	cc                   	int3
ffffffff80140d6f:	cc                   	int3