
/home/veasnawt/src/vos-r2-console-accept/build/console-accept-final/release-reference.o:     file format elf64-x86-64


Disassembly of section .text.input_console_accept_reference:

0000000000000000 <input_console_accept_reference>:
   0:	8b 8f 88 00 00 00    	mov    0x88(%rdi),%ecx
   6:	85 c9                	test   %ecx,%ecx
   8:	75 36                	jne    40 <input_console_accept_reference+0x40>
   a:	48 83 ec 18          	sub    $0x18,%rsp
   e:	40 80 fe 0a          	cmp    $0xa,%sil
  12:	74 5c                	je     70 <input_console_accept_reference+0x70>
  14:	40 80 fe 08          	cmp    $0x8,%sil
  18:	74 2e                	je     48 <input_console_accept_reference+0x48>
  1a:	8d 56 e0             	lea    -0x20(%rsi),%edx
  1d:	80 fa 5e             	cmp    $0x5e,%dl
  20:	77 14                	ja     36 <input_console_accept_reference+0x36>
  22:	8b 17                	mov    (%rdi),%edx
  24:	81 fa 80 00 00 00    	cmp    $0x80,%edx
  2a:	75 6c                	jne    98 <input_console_accept_reference+0x98>
  2c:	c7 87 8c 00 00 00 01 	movl   $0x1,0x8c(%rdi)
  33:	00 00 00 
  36:	48 83 c4 18          	add    $0x18,%rsp
  3a:	c3                   	ret
  3b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
  40:	c3                   	ret
  41:	0f 1f 80 00 00 00 00 	nopl   0x0(%rax)
  48:	8b 17                	mov    (%rdi),%edx
  4a:	85 d2                	test   %edx,%edx
  4c:	74 e8                	je     36 <input_console_accept_reference+0x36>
  4e:	48 89 7c 24 08       	mov    %rdi,0x8(%rsp)
  53:	e8 00 00 00 00       	call   58 <input_console_accept_reference+0x58>
			54: R_X86_64_PLT32	console_erase_previous-0x4
  58:	48 8b 44 24 08       	mov    0x8(%rsp),%rax
  5d:	8b 08                	mov    (%rax),%ecx
  5f:	8d 51 ff             	lea    -0x1(%rcx),%edx
  62:	89 10                	mov    %edx,(%rax)
  64:	c6 44 10 04 00       	movb   $0x0,0x4(%rax,%rdx,1)
  69:	eb cb                	jmp    36 <input_console_accept_reference+0x36>
  6b:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
  70:	48 89 7c 24 08       	mov    %rdi,0x8(%rsp)
  75:	bf 00 00 00 00       	mov    $0x0,%edi
			76: R_X86_64_32	.rodata.input_console_accept_reference.str1.1
  7a:	e8 00 00 00 00       	call   7f <input_console_accept_reference+0x7f>
			7b: R_X86_64_PLT32	serial_puts-0x4
  7f:	48 8b 44 24 08       	mov    0x8(%rsp),%rax
  84:	c7 80 88 00 00 00 01 	movl   $0x1,0x88(%rax)
  8b:	00 00 00 
  8e:	48 83 c4 18          	add    $0x18,%rsp
  92:	c3                   	ret
  93:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
  98:	8d 4a 01             	lea    0x1(%rdx),%ecx
  9b:	89 0f                	mov    %ecx,(%rdi)
  9d:	40 88 74 17 04       	mov    %sil,0x4(%rdi,%rdx,1)
  a2:	c6 44 0f 04 00       	movb   $0x0,0x4(%rdi,%rcx,1)
  a7:	40 0f be fe          	movsbl %sil,%edi
  ab:	48 83 c4 18          	add    $0x18,%rsp
  af:	e9 00 00 00 00       	jmp    b4 <input_console_accept_reference+0xb4>
			b0: R_X86_64_PLT32	serial_putc-0x4
