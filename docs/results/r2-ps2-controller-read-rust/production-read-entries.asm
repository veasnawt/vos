# Final kernel 7b27f1e6f87ed04495500ddfbdcc159c1f8cfb3a6b42fa6abee21eb2800b4ef3: the two Rust read entries and every C call site.
# Call sites (7): C callers forward to the Rust entries; write helpers and lifecycle remain C.
<command>:  ffffffff80123b64:	call   ffffffff80140dd0 <vos_rust_keyboard_read_keyboard>
<aux_command>:  ffffffff80123c14:	call   ffffffff80140d60 <vos_rust_keyboard_read_aux>
<keyboard_init>:  ffffffff80123d4d:	call   ffffffff80140dd0 <vos_rust_keyboard_read_keyboard>
<keyboard_suspend>:  ffffffff80123f24:	call   ffffffff80140dd0 <vos_rust_keyboard_read_keyboard>
<keyboard_resume>:  ffffffff80123ffd:	call   ffffffff80140dd0 <vos_rust_keyboard_read_keyboard>
<keyboard_pointer_start>:  ffffffff8012415a:	call   ffffffff80140d60 <vos_rust_keyboard_read_aux>
<keyboard_pointer_start>:  ffffffff80124176:	call   ffffffff80140d60 <vos_rust_keyboard_read_aux>

ffffffff80140d60 <vos_rust_keyboard_read_aux>:
ffffffff80140d60:	push   %rbp
ffffffff80140d61:	push   %r15
ffffffff80140d63:	push   %r14
ffffffff80140d65:	push   %r12
ffffffff80140d67:	push   %rbx
ffffffff80140d68:	xor    %ebx,%ebx
ffffffff80140d6a:	test   %rsi,%rsi
ffffffff80140d6d:	je     ffffffff80140dc3 <vos_rust_keyboard_read_aux+0x63>
ffffffff80140d6f:	mov    %rdi,%r14
ffffffff80140d72:	lea    0x58(%rdi),%rax
ffffffff80140d76:	cmp    $0x59,%rax
ffffffff80140d7a:	jb     ffffffff80140dc3 <vos_rust_keyboard_read_aux+0x63>
ffffffff80140d7c:	mov    %r14d,%eax
ffffffff80140d7f:	and    $0x7,%eax
ffffffff80140d82:	jne    ffffffff80140dc3 <vos_rust_keyboard_read_aux+0x63>
ffffffff80140d84:	mov    %rsi,%r15
ffffffff80140d87:	mov    $0x186a0,%r12d
ffffffff80140d8d:	jmp    ffffffff80140d95 <vos_rust_keyboard_read_aux+0x35>
ffffffff80140d8f:	nop
ffffffff80140d90:	dec    %r12d
ffffffff80140d93:	je     ffffffff80140dc3 <vos_rust_keyboard_read_aux+0x63>
ffffffff80140d95:	mov    (%r14),%rax
ffffffff80140d98:	mov    $0x64,%edi
ffffffff80140d9d:	call   *(%rax)
ffffffff80140d9f:	test   $0x1,%al
ffffffff80140da1:	je     ffffffff80140d90 <vos_rust_keyboard_read_aux+0x30>
ffffffff80140da3:	mov    %eax,%ebp
ffffffff80140da5:	mov    (%r14),%rax
ffffffff80140da8:	mov    $0x60,%edi
ffffffff80140dad:	call   *(%rax)
ffffffff80140daf:	cmp    $0x3f,%bpl
ffffffff80140db3:	ja     ffffffff80140dc3 <vos_rust_keyboard_read_aux+0x63>
ffffffff80140db5:	cmp    $0x20,%bpl
ffffffff80140db9:	jb     ffffffff80140d90 <vos_rust_keyboard_read_aux+0x30>
ffffffff80140dbb:	mov    %al,(%r15)
ffffffff80140dbe:	mov    $0x1,%ebx
ffffffff80140dc3:	mov    %ebx,%eax
ffffffff80140dc5:	pop    %rbx
ffffffff80140dc6:	pop    %r12
ffffffff80140dc8:	pop    %r14
ffffffff80140dca:	pop    %r15
ffffffff80140dcc:	pop    %rbp
ffffffff80140dcd:	ret
ffffffff80140dce:	int3
ffffffff80140dcf:	int3

ffffffff80140dd0 <vos_rust_keyboard_read_keyboard>:
ffffffff80140dd0:	push   %rbp
ffffffff80140dd1:	push   %r15
ffffffff80140dd3:	push   %r14
ffffffff80140dd5:	push   %r12
ffffffff80140dd7:	push   %rbx
ffffffff80140dd8:	xor    %ebx,%ebx
ffffffff80140dda:	test   %rsi,%rsi
ffffffff80140ddd:	je     ffffffff80140e33 <vos_rust_keyboard_read_keyboard+0x63>
ffffffff80140ddf:	mov    %rdi,%r14
ffffffff80140de2:	lea    0x58(%rdi),%rax
ffffffff80140de6:	cmp    $0x59,%rax
ffffffff80140dea:	jb     ffffffff80140e33 <vos_rust_keyboard_read_keyboard+0x63>
ffffffff80140dec:	mov    %r14d,%eax
ffffffff80140def:	and    $0x7,%eax
ffffffff80140df2:	jne    ffffffff80140e33 <vos_rust_keyboard_read_keyboard+0x63>
ffffffff80140df4:	mov    %rsi,%r15
ffffffff80140df7:	mov    $0x186a0,%r12d
ffffffff80140dfd:	jmp    ffffffff80140e05 <vos_rust_keyboard_read_keyboard+0x35>
ffffffff80140dff:	nop
ffffffff80140e00:	dec    %r12d
ffffffff80140e03:	je     ffffffff80140e33 <vos_rust_keyboard_read_keyboard+0x63>
ffffffff80140e05:	mov    (%r14),%rax
ffffffff80140e08:	mov    $0x64,%edi
ffffffff80140e0d:	call   *(%rax)
ffffffff80140e0f:	test   $0x1,%al
ffffffff80140e11:	je     ffffffff80140e00 <vos_rust_keyboard_read_keyboard+0x30>
ffffffff80140e13:	mov    %eax,%ebp
ffffffff80140e15:	mov    (%r14),%rax
ffffffff80140e18:	mov    $0x60,%edi
ffffffff80140e1d:	call   *(%rax)
ffffffff80140e1f:	cmp    $0x3f,%bpl
ffffffff80140e23:	ja     ffffffff80140e33 <vos_rust_keyboard_read_keyboard+0x63>
ffffffff80140e25:	cmp    $0x1f,%bpl
ffffffff80140e29:	ja     ffffffff80140e00 <vos_rust_keyboard_read_keyboard+0x30>
ffffffff80140e2b:	mov    %al,(%r15)
ffffffff80140e2e:	mov    $0x1,%ebx
ffffffff80140e33:	mov    %ebx,%eax
ffffffff80140e35:	pop    %rbx
ffffffff80140e36:	pop    %r12
ffffffff80140e38:	pop    %r14
ffffffff80140e3a:	pop    %r15
ffffffff80140e3c:	pop    %rbp
ffffffff80140e3d:	ret
ffffffff80140e3e:	int3
ffffffff80140e3f:	int3
