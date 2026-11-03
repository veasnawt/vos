
./build/queue-optimized/release-driver.o:     file format elf64-x86-64


Disassembly of section .text.write_port:

Disassembly of section .text.read_keyboard:

Disassembly of section .text.read_aux:

Disassembly of section .text.command:

Disassembly of section .text.aux_command:

Disassembly of section .text.keyboard_init:

Disassembly of section .text.keyboard_poll:

0000000000000000 <keyboard_poll>:
   0:	f3 0f 1e fa          	endbr64
   4:	8b 57 14             	mov    0x14(%rdi),%edx
   7:	85 d2                	test   %edx,%edx
   9:	0f 85 91 00 00 00    	jne    a0 <keyboard_poll+0xa0>
   f:	8b 47 10             	mov    0x10(%rdi),%eax
  12:	85 c0                	test   %eax,%eax
  14:	0f 84 7e 00 00 00    	je     98 <keyboard_poll+0x98>
  1a:	41 55                	push   %r13
  1c:	41 54                	push   %r12
  1e:	41 bc 20 00 00 00    	mov    $0x20,%r12d
  24:	55                   	push   %rbp
  25:	48 89 fd             	mov    %rdi,%rbp
  28:	53                   	push   %rbx
  29:	48 83 ec 08          	sub    $0x8,%rsp
  2d:	eb 17                	jmp    46 <keyboard_poll+0x46>
  2f:	90                   	nop
  30:	0f b6 f0             	movzbl %al,%esi
  33:	48 8d 7d 08          	lea    0x8(%rbp),%rdi
  37:	e8 00 00 00 00       	call   3c <keyboard_poll+0x3c>
			38: R_X86_64_PLT32	keyboard_decode-0x4
  3c:	84 c0                	test   %al,%al
  3e:	75 4a                	jne    8a <keyboard_poll+0x8a>
  40:	41 83 ec 01          	sub    $0x1,%r12d
  44:	74 42                	je     88 <keyboard_poll+0x88>
  46:	48 8b 45 00          	mov    0x0(%rbp),%rax
  4a:	bf 64 00 00 00       	mov    $0x64,%edi
  4f:	ff 10                	call   *(%rax)
  51:	89 c3                	mov    %eax,%ebx
  53:	a8 01                	test   $0x1,%al
  55:	74 31                	je     88 <keyboard_poll+0x88>
  57:	48 8b 45 00          	mov    0x0(%rbp),%rax
  5b:	bf 60 00 00 00       	mov    $0x60,%edi
  60:	ff 10                	call   *(%rax)
  62:	f6 c3 20             	test   $0x20,%bl
  65:	75 d9                	jne    40 <keyboard_poll+0x40>
  67:	80 fb 3f             	cmp    $0x3f,%bl
  6a:	77 08                	ja     74 <keyboard_poll+0x74>
  6c:	8d 50 ff             	lea    -0x1(%rax),%edx
  6f:	80 fa fd             	cmp    $0xfd,%dl
  72:	76 bc                	jbe    30 <keyboard_poll+0x30>
  74:	c7 45 08 00 00 00 00 	movl   $0x0,0x8(%rbp)
  7b:	c7 45 0b 00 00 00 00 	movl   $0x0,0xb(%rbp)
  82:	41 83 ec 01          	sub    $0x1,%r12d
  86:	75 be                	jne    46 <keyboard_poll+0x46>
  88:	31 c0                	xor    %eax,%eax
  8a:	48 83 c4 08          	add    $0x8,%rsp
  8e:	5b                   	pop    %rbx
  8f:	5d                   	pop    %rbp
  90:	41 5c                	pop    %r12
  92:	41 5d                	pop    %r13
  94:	c3                   	ret
  95:	0f 1f 00             	nopl   (%rax)
  98:	31 c0                	xor    %eax,%eax
  9a:	c3                   	ret
  9b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
  a0:	e9 00 00 00 00       	jmp    a5 <keyboard_poll+0xa5>
			a1: R_X86_64_PLT32	vos_rust_keyboard_routed_poll-0x4

Disassembly of section .text.keyboard_suspend:

Disassembly of section .text.keyboard_resume:

Disassembly of section .text.keyboard_pointer_start:

Disassembly of section .text.keyboard_pointer_stop:

Disassembly of section .text.keyboard_pointer_poll:

Disassembly of section .text.keyboard_pointer_lost:
