
/home/veasnawt/src/vos-r2-console-accept/build/console-accept-final/target-ffi.o:     file format elf64-x86-64


Disassembly of section .text.input_console_accept:

0000000000000000 <input_console_accept>:
   0:	53                   	push   %rbx
   1:	40 84 f6             	test   %sil,%sil
   4:	0f 95 c0             	setne  %al
   7:	48 8d 8f 90 00 00 00 	lea    0x90(%rdi),%rcx
   e:	48 81 f9 91 00 00 00 	cmp    $0x91,%rcx
  15:	0f 93 c1             	setae  %cl
  18:	84 c8                	test   %cl,%al
  1a:	74 09                	je     25 <input_console_accept+0x25>
  1c:	83 bf 88 00 00 00 00 	cmpl   $0x0,0x88(%rdi)
  23:	74 02                	je     27 <input_console_accept+0x27>
  25:	5b                   	pop    %rbx
  26:	c3                   	ret
  27:	40 0f be c6          	movsbl %sil,%eax
  2b:	83 f8 08             	cmp    $0x8,%eax
  2e:	74 20                	je     50 <input_console_accept+0x50>
  30:	83 f8 0a             	cmp    $0xa,%eax
  33:	75 3f                	jne    74 <input_console_accept+0x74>
  35:	48 89 fb             	mov    %rdi,%rbx
  38:	48 c7 c7 00 00 00 00 	mov    $0x0,%rdi
			3b: R_X86_64_32S	.rodata.str1.1
  3f:	e8 00 00 00 00       	call   44 <input_console_accept+0x44>
			40: R_X86_64_PLT32	serial_puts-0x4
  44:	c7 83 88 00 00 00 01 	movl   $0x1,0x88(%rbx)
  4b:	00 00 00 
  4e:	5b                   	pop    %rbx
  4f:	c3                   	ret
  50:	83 3f 00             	cmpl   $0x0,(%rdi)
  53:	74 d0                	je     25 <input_console_accept+0x25>
  55:	48 89 fb             	mov    %rdi,%rbx
  58:	e8 00 00 00 00       	call   5d <input_console_accept+0x5d>
			59: R_X86_64_PLT32	console_erase_previous-0x4
  5d:	8b 0b                	mov    (%rbx),%ecx
  5f:	ff c9                	dec    %ecx
  61:	81 f9 80 00 00 00    	cmp    $0x80,%ecx
  67:	77 bc                	ja     25 <input_console_accept+0x25>
  69:	89 0b                	mov    %ecx,(%rbx)
  6b:	89 c9                	mov    %ecx,%ecx
  6d:	c6 44 0b 04 00       	movb   $0x0,0x4(%rbx,%rcx,1)
  72:	5b                   	pop    %rbx
  73:	c3                   	ret
  74:	8d 4e e0             	lea    -0x20(%rsi),%ecx
  77:	80 f9 5f             	cmp    $0x5f,%cl
  7a:	73 a9                	jae    25 <input_console_accept+0x25>
  7c:	8b 0f                	mov    (%rdi),%ecx
  7e:	48 81 f9 80 00 00 00 	cmp    $0x80,%rcx
  85:	75 0c                	jne    93 <input_console_accept+0x93>
  87:	c7 87 8c 00 00 00 01 	movl   $0x1,0x8c(%rdi)
  8e:	00 00 00 
  91:	5b                   	pop    %rbx
  92:	c3                   	ret
  93:	83 f9 7f             	cmp    $0x7f,%ecx
  96:	77 8d                	ja     25 <input_console_accept+0x25>
  98:	48 8d 51 01          	lea    0x1(%rcx),%rdx
  9c:	89 17                	mov    %edx,(%rdi)
  9e:	40 88 74 0f 04       	mov    %sil,0x4(%rdi,%rcx,1)
  a3:	c6 44 0f 05 00       	movb   $0x0,0x5(%rdi,%rcx,1)
  a8:	89 c7                	mov    %eax,%edi
  aa:	5b                   	pop    %rbx
  ab:	e9 00 00 00 00       	jmp    b0 <input_console_accept+0xb0>
			ac: R_X86_64_PLT32	serial_putc-0x4
