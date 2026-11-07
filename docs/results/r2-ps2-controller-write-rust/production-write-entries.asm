# Final kernel de8f1fa483620418ae9bb2200db76315cd5fc2e2fd9692693a954738cece98b5: the three Rust write entries and every C call site.
# Call sites (26): C lifecycle callers forward to the Rust entries; lifecycle remains C.
<keyboard_init>:  ffffffff80123aeb:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123b07:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123b50:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123b86:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123b9c:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123bb6:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_init>:  ffffffff80123bcb:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_init>:  ffffffff80123be0:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_init>:  ffffffff80123bf5:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_init>:  ffffffff80123c0a:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_suspend>:  ffffffff80123c8a:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_resume>:  ffffffff80123d54:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_pointer_start>:  ffffffff80123dd6:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_start>:  ffffffff80123dec:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_start>:  ffffffff80123e06:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_start>:  ffffffff80123e18:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_start>:  ffffffff80123e3b:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_start>:  ffffffff80123e5e:	call   ffffffff80140a00 <vos_rust_keyboard_command>
<keyboard_pointer_start>:  ffffffff80123ea8:	call   ffffffff80140900 <vos_rust_keyboard_aux_command>
<keyboard_pointer_start>:  ffffffff80123efd:	call   ffffffff80140900 <vos_rust_keyboard_aux_command>
<keyboard_pointer_start>:  ffffffff80123f12:	call   ffffffff80140900 <vos_rust_keyboard_aux_command>
<keyboard_pointer_start>:  ffffffff80123f27:	call   ffffffff80140900 <vos_rust_keyboard_aux_command>
<keyboard_pointer_stop>:  ffffffff80123f5e:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_stop>:  ffffffff80123f6b:	call   ffffffff80140900 <vos_rust_keyboard_aux_command>
<keyboard_pointer_stop>:  ffffffff80123f7d:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>
<keyboard_pointer_stop>:  ffffffff80123f8f:	call   ffffffff80140eb0 <vos_rust_keyboard_write_port>

ffffffff80140900 <vos_rust_keyboard_aux_command>:
ffffffff80140900:	push   %rbp
ffffffff80140901:	push   %r15
ffffffff80140903:	push   %r14
ffffffff80140905:	push   %rbx
ffffffff80140906:	push   %rax
ffffffff80140907:	mov    %rdi,%rbx
ffffffff8014090a:	lea    -0x1(%rdi),%rax
ffffffff8014090e:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140912:	setae  %al
ffffffff80140915:	test   $0x7,%bl
ffffffff80140918:	setne  %cl
ffffffff8014091b:	xor    %ebp,%ebp
ffffffff8014091d:	or     %al,%cl
ffffffff8014091f:	jne    ffffffff801409eb <vos_rust_keyboard_aux_command+0xeb>
ffffffff80140925:	mov    %esi,%r14d
ffffffff80140928:	mov    $0x186a0,%r15d
ffffffff8014092e:	xchg   %ax,%ax
ffffffff80140930:	mov    (%rbx),%rax
ffffffff80140933:	mov    $0x64,%edi
ffffffff80140938:	call   *(%rax)
ffffffff8014093a:	test   $0x2,%al
ffffffff8014093c:	je     ffffffff80140948 <vos_rust_keyboard_aux_command+0x48>
ffffffff8014093e:	dec    %r15d
ffffffff80140941:	jne    ffffffff80140930 <vos_rust_keyboard_aux_command+0x30>
ffffffff80140943:	jmp    ffffffff801409eb <vos_rust_keyboard_aux_command+0xeb>
ffffffff80140948:	mov    (%rbx),%rax
ffffffff8014094b:	mov    0x8(%rax),%rax
ffffffff8014094f:	test   %rax,%rax
ffffffff80140952:	je     ffffffff801409eb <vos_rust_keyboard_aux_command+0xeb>
ffffffff80140958:	mov    $0x64,%edi
ffffffff8014095d:	mov    $0xd4,%esi
ffffffff80140962:	call   *%rax
ffffffff80140964:	mov    $0x186a0,%r15d
ffffffff8014096a:	nopw   0x0(%rax,%rax,1)
ffffffff80140970:	mov    (%rbx),%rax
ffffffff80140973:	mov    $0x64,%edi
ffffffff80140978:	call   *(%rax)
ffffffff8014097a:	test   $0x2,%al
ffffffff8014097c:	je     ffffffff80140985 <vos_rust_keyboard_aux_command+0x85>
ffffffff8014097e:	dec    %r15d
ffffffff80140981:	jne    ffffffff80140970 <vos_rust_keyboard_aux_command+0x70>
ffffffff80140983:	jmp    ffffffff801409eb <vos_rust_keyboard_aux_command+0xeb>
ffffffff80140985:	mov    (%rbx),%rax
ffffffff80140988:	mov    0x8(%rax),%rax
ffffffff8014098c:	test   %rax,%rax
ffffffff8014098f:	je     ffffffff801409eb <vos_rust_keyboard_aux_command+0xeb>
ffffffff80140991:	movzbl %r14b,%esi
ffffffff80140995:	mov    $0x60,%edi
ffffffff8014099a:	call   *%rax
ffffffff8014099c:	mov    $0x186a0,%r14d
ffffffff801409a2:	jmp    ffffffff801409b5 <vos_rust_keyboard_aux_command+0xb5>
ffffffff801409a4:	data16 data16 cs nopw 0x0(%rax,%rax,1)
ffffffff801409b0:	dec    %r14d
ffffffff801409b3:	je     ffffffff801409df <vos_rust_keyboard_aux_command+0xdf>
ffffffff801409b5:	mov    (%rbx),%rax
ffffffff801409b8:	mov    $0x64,%edi
ffffffff801409bd:	call   *(%rax)
ffffffff801409bf:	test   $0x1,%al
ffffffff801409c1:	je     ffffffff801409b0 <vos_rust_keyboard_aux_command+0xb0>
ffffffff801409c3:	mov    %eax,%ebp
ffffffff801409c5:	mov    (%rbx),%rax
ffffffff801409c8:	mov    $0x60,%edi
ffffffff801409cd:	call   *(%rax)
ffffffff801409cf:	cmp    $0x3f,%bpl
ffffffff801409d3:	ja     ffffffff801409df <vos_rust_keyboard_aux_command+0xdf>
ffffffff801409d5:	cmp    $0x20,%bpl
ffffffff801409d9:	jb     ffffffff801409b0 <vos_rust_keyboard_aux_command+0xb0>
ffffffff801409db:	mov    $0x1,%cl
ffffffff801409dd:	jmp    ffffffff801409e1 <vos_rust_keyboard_aux_command+0xe1>
ffffffff801409df:	xor    %ecx,%ecx
ffffffff801409e1:	cmp    $0xfa,%al
ffffffff801409e3:	sete   %al
ffffffff801409e6:	and    %cl,%al
ffffffff801409e8:	movzbl %al,%ebp
ffffffff801409eb:	mov    %ebp,%eax
ffffffff801409ed:	add    $0x8,%rsp
ffffffff801409f1:	pop    %rbx
ffffffff801409f2:	pop    %r14
ffffffff801409f4:	pop    %r15
ffffffff801409f6:	pop    %rbp
ffffffff801409f7:	ret
ffffffff801409f8:	int3
ffffffff801409f9:	int3
ffffffff801409fa:	int3
ffffffff801409fb:	int3
ffffffff801409fc:	int3
ffffffff801409fd:	int3
ffffffff801409fe:	int3
ffffffff801409ff:	int3

ffffffff80140a00 <vos_rust_keyboard_command>:
ffffffff80140a00:	push   %rbp
ffffffff80140a01:	push   %r15
ffffffff80140a03:	push   %r14
ffffffff80140a05:	push   %rbx
ffffffff80140a06:	push   %rax
ffffffff80140a07:	mov    %rdi,%r14
ffffffff80140a0a:	lea    -0x1(%rdi),%rax
ffffffff80140a0e:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140a12:	setae  %al
ffffffff80140a15:	test   $0x7,%r14b
ffffffff80140a19:	setne  %cl
ffffffff80140a1c:	xor    %ebx,%ebx
ffffffff80140a1e:	or     %al,%cl
ffffffff80140a20:	jne    ffffffff80140aa2 <vos_rust_keyboard_command+0xa2>
ffffffff80140a26:	mov    %esi,%ebp
ffffffff80140a28:	mov    $0x186a0,%r15d
ffffffff80140a2e:	xchg   %ax,%ax
ffffffff80140a30:	mov    (%r14),%rax
ffffffff80140a33:	mov    $0x64,%edi
ffffffff80140a38:	call   *(%rax)
ffffffff80140a3a:	test   $0x2,%al
ffffffff80140a3c:	je     ffffffff80140a45 <vos_rust_keyboard_command+0x45>
ffffffff80140a3e:	dec    %r15d
ffffffff80140a41:	jne    ffffffff80140a30 <vos_rust_keyboard_command+0x30>
ffffffff80140a43:	jmp    ffffffff80140aa2 <vos_rust_keyboard_command+0xa2>
ffffffff80140a45:	mov    (%r14),%rax
ffffffff80140a48:	mov    0x8(%rax),%rax
ffffffff80140a4c:	test   %rax,%rax
ffffffff80140a4f:	je     ffffffff80140aa2 <vos_rust_keyboard_command+0xa2>
ffffffff80140a51:	movzbl %bpl,%esi
ffffffff80140a55:	mov    $0x60,%edi
ffffffff80140a5a:	call   *%rax
ffffffff80140a5c:	mov    $0x186a0,%r15d
ffffffff80140a62:	jmp    ffffffff80140a75 <vos_rust_keyboard_command+0x75>
ffffffff80140a64:	data16 data16 cs nopw 0x0(%rax,%rax,1)
ffffffff80140a70:	dec    %r15d
ffffffff80140a73:	je     ffffffff80140aa2 <vos_rust_keyboard_command+0xa2>
ffffffff80140a75:	mov    (%r14),%rax
ffffffff80140a78:	mov    $0x64,%edi
ffffffff80140a7d:	call   *(%rax)
ffffffff80140a7f:	test   $0x1,%al
ffffffff80140a81:	je     ffffffff80140a70 <vos_rust_keyboard_command+0x70>
ffffffff80140a83:	mov    %eax,%ebp
ffffffff80140a85:	mov    (%r14),%rax
ffffffff80140a88:	mov    $0x60,%edi
ffffffff80140a8d:	call   *(%rax)
ffffffff80140a8f:	cmp    $0x3f,%bpl
ffffffff80140a93:	ja     ffffffff80140aa2 <vos_rust_keyboard_command+0xa2>
ffffffff80140a95:	cmp    $0x1f,%bpl
ffffffff80140a99:	ja     ffffffff80140a70 <vos_rust_keyboard_command+0x70>
ffffffff80140a9b:	xor    %ebx,%ebx
ffffffff80140a9d:	cmp    $0xfa,%al
ffffffff80140a9f:	sete   %bl
ffffffff80140aa2:	mov    %ebx,%eax
ffffffff80140aa4:	add    $0x8,%rsp
ffffffff80140aa8:	pop    %rbx
ffffffff80140aa9:	pop    %r14
ffffffff80140aab:	pop    %r15
ffffffff80140aad:	pop    %rbp
ffffffff80140aae:	ret
ffffffff80140aaf:	int3

ffffffff80140eb0 <vos_rust_keyboard_write_port>:
ffffffff80140eb0:	push   %rbp
ffffffff80140eb1:	push   %r15
ffffffff80140eb3:	push   %r14
ffffffff80140eb5:	push   %rbx
ffffffff80140eb6:	push   %rax
ffffffff80140eb7:	mov    %rdi,%rbx
ffffffff80140eba:	lea    -0x1(%rdi),%rax
ffffffff80140ebe:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140ec2:	setae  %cl
ffffffff80140ec5:	test   $0x7,%bl
ffffffff80140ec8:	setne  %dil
ffffffff80140ecc:	xor    %eax,%eax
ffffffff80140ece:	or     %cl,%dil
ffffffff80140ed1:	jne    ffffffff80140f25 <vos_rust_keyboard_write_port+0x75>
ffffffff80140ed3:	movzwl %si,%ebp
ffffffff80140ed6:	mov    %ebp,%ecx
ffffffff80140ed8:	or     $0x4,%ecx
ffffffff80140edb:	cmp    $0x64,%ecx
ffffffff80140ede:	jne    ffffffff80140f25 <vos_rust_keyboard_write_port+0x75>
ffffffff80140ee0:	mov    %edx,%r14d
ffffffff80140ee3:	mov    $0x186a0,%r15d
ffffffff80140ee9:	nopl   0x0(%rax)
ffffffff80140ef0:	mov    (%rbx),%rax
ffffffff80140ef3:	mov    $0x64,%edi
ffffffff80140ef8:	call   *(%rax)
ffffffff80140efa:	test   $0x2,%al
ffffffff80140efc:	je     ffffffff80140f07 <vos_rust_keyboard_write_port+0x57>
ffffffff80140efe:	dec    %r15d
ffffffff80140f01:	jne    ffffffff80140ef0 <vos_rust_keyboard_write_port+0x40>
ffffffff80140f03:	xor    %eax,%eax
ffffffff80140f05:	jmp    ffffffff80140f25 <vos_rust_keyboard_write_port+0x75>
ffffffff80140f07:	mov    (%rbx),%rax
ffffffff80140f0a:	mov    0x8(%rax),%rcx
ffffffff80140f0e:	test   %rcx,%rcx
ffffffff80140f11:	mov    $0x0,%eax
ffffffff80140f16:	je     ffffffff80140f25 <vos_rust_keyboard_write_port+0x75>
ffffffff80140f18:	movzbl %r14b,%esi
ffffffff80140f1c:	mov    %ebp,%edi
ffffffff80140f1e:	call   *%rcx
ffffffff80140f20:	mov    $0x1,%eax
ffffffff80140f25:	add    $0x8,%rsp
ffffffff80140f29:	pop    %rbx
ffffffff80140f2a:	pop    %r14
ffffffff80140f2c:	pop    %r15
ffffffff80140f2e:	pop    %rbp
ffffffff80140f2f:	ret
