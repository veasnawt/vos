_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step
ffffffff80138760:	48 8d 4f 22          	lea    0x22(%rdi),%rcx
ffffffff80138764:	b8 ff ff ff ff       	mov    $0xffffffff,%eax
ffffffff80138769:	f6 c1 01             	test   $0x1,%cl
ffffffff8013876c:	0f 85 b9 00 00 00    	jne    ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff80138772:	4c 8d 4f 1c          	lea    0x1c(%rdi),%r9
ffffffff80138776:	4d 8d 41 05          	lea    0x5(%r9),%r8
ffffffff8013877a:	48 8d 51 06          	lea    0x6(%rcx),%rdx
ffffffff8013877e:	4c 39 ca             	cmp    %r9,%rdx
ffffffff80138781:	0f 97 c2             	seta   %dl
ffffffff80138784:	49 39 c8             	cmp    %rcx,%r8
ffffffff80138787:	41 0f 97 c0          	seta   %r8b
ffffffff8013878b:	49 83 f9 fa          	cmp    $0xfffffffffffffffa,%r9
ffffffff8013878f:	0f 87 96 00 00 00    	ja     ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff80138795:	48 83 f9 f9          	cmp    $0xfffffffffffffff9,%rcx
ffffffff80138799:	0f 87 8c 00 00 00    	ja     ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff8013879f:	41 20 d0             	and    %dl,%r8b
ffffffff801387a2:	0f 85 83 00 00 00    	jne    ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff801387a8:	0f b6 4f 1f          	movzbl 0x1f(%rdi),%ecx
ffffffff801387ac:	48 83 f9 02          	cmp    $0x2,%rcx
ffffffff801387b0:	77 0b                	ja     ffffffff801387bd <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0x5d>
ffffffff801387b2:	48 85 c9             	test   %rcx,%rcx
ffffffff801387b5:	74 0d                	je     ffffffff801387c4 <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0x64>
ffffffff801387b7:	80 7f 20 10          	cmpb   $0x10,0x20(%rdi)
ffffffff801387bb:	72 07                	jb     ffffffff801387c4 <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0x64>
ffffffff801387bd:	66 c7 47 1f 00 00    	movw   $0x0,0x1f(%rdi)
ffffffff801387c3:	c3                   	ret
ffffffff801387c4:	c6 47 20 00          	movb   $0x0,0x20(%rdi)
ffffffff801387c8:	89 f2                	mov    %esi,%edx
ffffffff801387ca:	80 e2 08             	and    $0x8,%dl
ffffffff801387cd:	08 ca                	or     %cl,%dl
ffffffff801387cf:	74 5a                	je     ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff801387d1:	40 88 74 0f 1c       	mov    %sil,0x1c(%rdi,%rcx,1)
ffffffff801387d6:	0f b6 4f 1f          	movzbl 0x1f(%rdi),%ecx
ffffffff801387da:	fe c1                	inc    %cl
ffffffff801387dc:	88 4f 1f             	mov    %cl,0x1f(%rdi)
ffffffff801387df:	31 c0                	xor    %eax,%eax
ffffffff801387e1:	80 f9 03             	cmp    $0x3,%cl
ffffffff801387e4:	72 45                	jb     ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff801387e6:	c6 47 1f 00          	movb   $0x0,0x1f(%rdi)
ffffffff801387ea:	0f b6 4f 1c          	movzbl 0x1c(%rdi),%ecx
ffffffff801387ee:	b8 ff ff ff ff       	mov    $0xffffffff,%eax
ffffffff801387f3:	80 f9 3f             	cmp    $0x3f,%cl
ffffffff801387f6:	77 33                	ja     ffffffff8013882b <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step+0xcb>
ffffffff801387f8:	0f b6 47 1d          	movzbl 0x1d(%rdi),%eax
ffffffff801387fc:	8d 90 00 ff ff ff    	lea    -0x100(%rax),%edx
ffffffff80138802:	f6 c1 10             	test   $0x10,%cl
ffffffff80138805:	0f 45 c2             	cmovne %edx,%eax
ffffffff80138808:	0f b6 57 1e          	movzbl 0x1e(%rdi),%edx
ffffffff8013880c:	8d b2 00 ff ff ff    	lea    -0x100(%rdx),%esi
ffffffff80138812:	80 f9 20             	cmp    $0x20,%cl
ffffffff80138815:	0f 43 d6             	cmovae %esi,%edx
ffffffff80138818:	80 e1 01             	and    $0x1,%cl
ffffffff8013881b:	66 89 47 22          	mov    %ax,0x22(%rdi)
ffffffff8013881f:	66 89 57 24          	mov    %dx,0x24(%rdi)
ffffffff80138823:	88 4f 26             	mov    %cl,0x26(%rdi)
ffffffff80138826:	b8 01 00 00 00       	mov    $0x1,%eax
ffffffff8013882b:	c3                   	ret
ffffffff8013882c:	cc                   	int3
ffffffff8013882d:	cc                   	int3
ffffffff8013882e:	cc                   	int3
ffffffff8013882f:	cc                   	int3

vos_rust_keyboard_routed_drain
ffffffff80140de0:	48 85 ff             	test   %rdi,%rdi
ffffffff80140de3:	74 24                	je     ffffffff80140e09 <vos_rust_keyboard_routed_drain+0x29>
ffffffff80140de5:	55                   	push   %rbp
ffffffff80140de6:	41 57                	push   %r15
ffffffff80140de8:	41 56                	push   %r14
ffffffff80140dea:	41 54                	push   %r12
ffffffff80140dec:	53                   	push   %rbx
ffffffff80140ded:	48 89 fb             	mov    %rdi,%rbx
ffffffff80140df0:	f6 c3 07             	test   $0x7,%bl
ffffffff80140df3:	0f 95 c0             	setne  %al
ffffffff80140df6:	48 83 ff a8          	cmp    $0xffffffffffffffa8,%rdi
ffffffff80140dfa:	0f 93 c1             	setae  %cl
ffffffff80140dfd:	08 c1                	or     %al,%cl
ffffffff80140dff:	74 09                	je     ffffffff80140e0a <vos_rust_keyboard_routed_drain+0x2a>
ffffffff80140e01:	5b                   	pop    %rbx
ffffffff80140e02:	41 5c                	pop    %r12
ffffffff80140e04:	41 5e                	pop    %r14
ffffffff80140e06:	41 5f                	pop    %r15
ffffffff80140e08:	5d                   	pop    %rbp
ffffffff80140e09:	c3                   	ret
ffffffff80140e0a:	4c 8d 7b 1c          	lea    0x1c(%rbx),%r15
ffffffff80140e0e:	4c 8d 73 08          	lea    0x8(%rbx),%r14
ffffffff80140e12:	41 bc 20 00 00 00    	mov    $0x20,%r12d
ffffffff80140e18:	eb 1a                	jmp    ffffffff80140e34 <vos_rust_keyboard_routed_drain+0x54>
ffffffff80140e1a:	66 0f 1f 44 00 00    	nopw   0x0(%rax,%rax,1)
ffffffff80140e20:	41 c7 46 03 00 00 00 	movl   $0x0,0x3(%r14)
ffffffff80140e27:	00 
ffffffff80140e28:	41 c7 06 00 00 00 00 	movl   $0x0,(%r14)
ffffffff80140e2f:	41 ff cc             	dec    %r12d
ffffffff80140e32:	74 cd                	je     ffffffff80140e01 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e34:	83 7b 18 00          	cmpl   $0x0,0x18(%rbx)
ffffffff80140e38:	75 c7                	jne    ffffffff80140e01 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e3a:	83 7b 30 20          	cmpl   $0x20,0x30(%rbx)
ffffffff80140e3e:	74 c1                	je     ffffffff80140e01 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e40:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140e43:	bf 64 00 00 00       	mov    $0x64,%edi
ffffffff80140e48:	ff 10                	call   *(%rax)
ffffffff80140e4a:	a8 01                	test   $0x1,%al
ffffffff80140e4c:	74 b3                	je     ffffffff80140e01 <vos_rust_keyboard_routed_drain+0x21>
ffffffff80140e4e:	89 c5                	mov    %eax,%ebp
ffffffff80140e50:	48 8b 03             	mov    (%rbx),%rax
ffffffff80140e53:	bf 60 00 00 00       	mov    $0x60,%edi
ffffffff80140e58:	ff 10                	call   *(%rax)
ffffffff80140e5a:	40 f6 c5 20          	test   $0x20,%bpl
ffffffff80140e5e:	75 40                	jne    ffffffff80140ea0 <vos_rust_keyboard_routed_drain+0xc0>
ffffffff80140e60:	40 80 fd 40          	cmp    $0x40,%bpl
ffffffff80140e64:	0f 92 c1             	setb   %cl
ffffffff80140e67:	8d 50 ff             	lea    -0x1(%rax),%edx
ffffffff80140e6a:	80 fa fe             	cmp    $0xfe,%dl
ffffffff80140e6d:	0f 92 c2             	setb   %dl
ffffffff80140e70:	84 d1                	test   %dl,%cl
ffffffff80140e72:	74 ac                	je     ffffffff80140e20 <vos_rust_keyboard_routed_drain+0x40>
ffffffff80140e74:	0f b6 f0             	movzbl %al,%esi
ffffffff80140e77:	4c 89 f7             	mov    %r14,%rdi
ffffffff80140e7a:	e8 b1 f3 ff ff       	call   ffffffff80140230 <keyboard_decode>
ffffffff80140e7f:	84 c0                	test   %al,%al
ffffffff80140e81:	74 ac                	je     ffffffff80140e2f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140e83:	8b 4b 30             	mov    0x30(%rbx),%ecx
ffffffff80140e86:	8b 53 2c             	mov    0x2c(%rbx),%edx
ffffffff80140e89:	01 ca                	add    %ecx,%edx
ffffffff80140e8b:	83 e2 1f             	and    $0x1f,%edx
ffffffff80140e8e:	88 44 13 34          	mov    %al,0x34(%rbx,%rdx,1)
ffffffff80140e92:	ff c1                	inc    %ecx
ffffffff80140e94:	89 4b 30             	mov    %ecx,0x30(%rbx)
ffffffff80140e97:	eb 96                	jmp    ffffffff80140e2f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140e99:	0f 1f 80 00 00 00 00 	nopl   0x0(%rax)
ffffffff80140ea0:	40 80 fd 40          	cmp    $0x40,%bpl
ffffffff80140ea4:	73 2a                	jae    ffffffff80140ed0 <vos_rust_keyboard_routed_drain+0xf0>
ffffffff80140ea6:	0f b6 f0             	movzbl %al,%esi
ffffffff80140ea9:	48 89 df             	mov    %rbx,%rdi
ffffffff80140eac:	e8 af 78 ff ff       	call   ffffffff80138760 <_RNvNtCs5YZeFliBs6N_10vos_kernel10ps2_routed12pointer_step>
ffffffff80140eb1:	85 c0                	test   %eax,%eax
ffffffff80140eb3:	78 27                	js     ffffffff80140edc <vos_rust_keyboard_routed_drain+0xfc>
ffffffff80140eb5:	0f 84 74 ff ff ff    	je     ffffffff80140e2f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140ebb:	c7 43 18 01 00 00 00 	movl   $0x1,0x18(%rbx)
ffffffff80140ec2:	e9 68 ff ff ff       	jmp    ffffffff80140e2f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140ec7:	66 0f 1f 84 00 00 00 	nopw   0x0(%rax,%rax,1)
ffffffff80140ece:	00 00 
ffffffff80140ed0:	41 c6 47 04 00       	movb   $0x0,0x4(%r15)
ffffffff80140ed5:	41 c7 07 00 00 00 00 	movl   $0x0,(%r15)
ffffffff80140edc:	8b 43 28             	mov    0x28(%rbx),%eax
ffffffff80140edf:	31 c9                	xor    %ecx,%ecx
ffffffff80140ee1:	83 f8 ff             	cmp    $0xffffffff,%eax
ffffffff80140ee4:	0f 95 c1             	setne  %cl
ffffffff80140ee7:	01 c1                	add    %eax,%ecx
ffffffff80140ee9:	89 4b 28             	mov    %ecx,0x28(%rbx)
ffffffff80140eec:	e9 3e ff ff ff       	jmp    ffffffff80140e2f <vos_rust_keyboard_routed_drain+0x4f>
ffffffff80140ef1:	cc                   	int3
ffffffff80140ef2:	cc                   	int3
ffffffff80140ef3:	cc                   	int3
ffffffff80140ef4:	cc                   	int3
ffffffff80140ef5:	cc                   	int3
ffffffff80140ef6:	cc                   	int3
ffffffff80140ef7:	cc                   	int3
ffffffff80140ef8:	cc                   	int3
ffffffff80140ef9:	cc                   	int3
ffffffff80140efa:	cc                   	int3
ffffffff80140efb:	cc                   	int3
ffffffff80140efc:	cc                   	int3
ffffffff80140efd:	cc                   	int3
ffffffff80140efe:	cc                   	int3
ffffffff80140eff:	cc                   	int3