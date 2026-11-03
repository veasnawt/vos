
./build/queue-optimized/release-reference.o:     file format elf64-x86-64


Disassembly of section .text.write_port:

Disassembly of section .text.read_keyboard:

Disassembly of section .text.read_aux:

Disassembly of section .text.command:

Disassembly of section .text.aux_command:

Disassembly of section .text.keyboard_init_reference:

Disassembly of section .text.keyboard_poll_reference:

0000000000000000 <keyboard_poll_reference>:
   0:	f3 0f 1e fa          	endbr64
   4:	41 55                	push   %r13
   6:	41 54                	push   %r12
   8:	55                   	push   %rbp
   9:	48 89 fd             	mov    %rdi,%rbp
   c:	53                   	push   %rbx
   d:	48 83 ec 08          	sub    $0x8,%rsp
  11:	8b 57 14             	mov    0x14(%rdi),%edx
  14:	8b 47 10             	mov    0x10(%rdi),%eax
  17:	85 d2                	test   %edx,%edx
  19:	0f 85 81 00 00 00    	jne    a0 <keyboard_poll_reference+0xa0>
  1f:	85 c0                	test   %eax,%eax
  21:	74 6d                	je     90 <keyboard_poll_reference+0x90>
  23:	41 bc 20 00 00 00    	mov    $0x20,%r12d
  29:	eb 1b                	jmp    46 <keyboard_poll_reference+0x46>
  2b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
  30:	0f b6 f0             	movzbl %al,%esi
  33:	48 8d 7d 08          	lea    0x8(%rbp),%rdi
  37:	e8 00 00 00 00       	call   3c <keyboard_poll_reference+0x3c>
			38: R_X86_64_PLT32	keyboard_decode-0x4
  3c:	84 c0                	test   %al,%al
  3e:	75 52                	jne    92 <keyboard_poll_reference+0x92>
  40:	41 83 ec 01          	sub    $0x1,%r12d
  44:	74 4a                	je     90 <keyboard_poll_reference+0x90>
  46:	48 8b 45 00          	mov    0x0(%rbp),%rax
  4a:	bf 64 00 00 00       	mov    $0x64,%edi
  4f:	ff 10                	call   *(%rax)
  51:	89 c3                	mov    %eax,%ebx
  53:	a8 01                	test   $0x1,%al
  55:	74 39                	je     90 <keyboard_poll_reference+0x90>
  57:	48 8b 45 00          	mov    0x0(%rbp),%rax
  5b:	bf 60 00 00 00       	mov    $0x60,%edi
  60:	ff 10                	call   *(%rax)
  62:	f6 c3 20             	test   $0x20,%bl
  65:	75 d9                	jne    40 <keyboard_poll_reference+0x40>
  67:	80 fb 3f             	cmp    $0x3f,%bl
  6a:	77 08                	ja     74 <keyboard_poll_reference+0x74>
  6c:	8d 50 ff             	lea    -0x1(%rax),%edx
  6f:	80 fa fd             	cmp    $0xfd,%dl
  72:	76 bc                	jbe    30 <keyboard_poll_reference+0x30>
  74:	c7 45 08 00 00 00 00 	movl   $0x0,0x8(%rbp)
  7b:	c7 45 0b 00 00 00 00 	movl   $0x0,0xb(%rbp)
  82:	41 83 ec 01          	sub    $0x1,%r12d
  86:	75 be                	jne    46 <keyboard_poll_reference+0x46>
  88:	0f 1f 84 00 00 00 00 	nopl   0x0(%rax,%rax,1)
  8f:	00 
  90:	31 c0                	xor    %eax,%eax
  92:	48 83 c4 08          	add    $0x8,%rsp
  96:	5b                   	pop    %rbx
  97:	5d                   	pop    %rbp
  98:	41 5c                	pop    %r12
  9a:	41 5d                	pop    %r13
  9c:	c3                   	ret
  9d:	0f 1f 00             	nopl   (%rax)
  a0:	85 c0                	test   %eax,%eax
  a2:	74 ec                	je     90 <keyboard_poll_reference+0x90>
  a4:	e8 00 00 00 00       	call   a9 <keyboard_poll_reference+0xa9>
			a5: R_X86_64_PLT32	vos_rust_keyboard_routed_drain-0x4
  a9:	8b 4d 30             	mov    0x30(%rbp),%ecx
  ac:	85 c9                	test   %ecx,%ecx
  ae:	74 e0                	je     90 <keyboard_poll_reference+0x90>
  b0:	8b 75 2c             	mov    0x2c(%rbp),%esi
  b3:	83 e9 01             	sub    $0x1,%ecx
  b6:	48 89 f2             	mov    %rsi,%rdx
  b9:	0f b6 44 35 34       	movzbl 0x34(%rbp,%rsi,1),%eax
  be:	c6 44 35 34 00       	movb   $0x0,0x34(%rbp,%rsi,1)
  c3:	83 c2 01             	add    $0x1,%edx
  c6:	89 4d 30             	mov    %ecx,0x30(%rbp)
  c9:	83 e2 1f             	and    $0x1f,%edx
  cc:	89 55 2c             	mov    %edx,0x2c(%rbp)
  cf:	eb c1                	jmp    92 <keyboard_poll_reference+0x92>

Disassembly of section .text.keyboard_suspend_reference:

Disassembly of section .text.keyboard_resume_reference:

Disassembly of section .text.keyboard_pointer_start_reference:

Disassembly of section .text.keyboard_pointer_stop_reference:

Disassembly of section .text.keyboard_pointer_poll_reference:

Disassembly of section .text.keyboard_pointer_lost_reference:

Disassembly of section .text.routed_poll_reference:

Disassembly of section .text.routed_drain_reference:
