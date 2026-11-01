
/home/veasnawt/src/vos-r2-console-line/build/branch-probe/ffi.o:     file format elf64-x86-64


Disassembly of section .text.input_console_take_line:

0000000000000000 <input_console_take_line>:
   0:	48 8d 8f 90 00 00 00 	lea    0x90(%rdi),%rcx
   7:	b8 fe ff ff ff       	mov    $0xfffffffe,%eax
   c:	48 81 f9 91 00 00 00 	cmp    $0x91,%rcx
  13:	73 01                	jae    16 <input_console_take_line+0x16>
  15:	c3                   	ret
  16:	83 bf 88 00 00 00 00 	cmpl   $0x0,0x88(%rdi)
  1d:	74 19                	je     38 <input_console_take_line+0x38>
  1f:	83 bf 8c 00 00 00 00 	cmpl   $0x0,0x8c(%rdi)
  26:	74 16                	je     3e <input_console_take_line+0x3e>
  28:	b9 12 00 00 00       	mov    $0x12,%ecx
  2d:	31 c0                	xor    %eax,%eax
  2f:	f3 48 ab             	rep stos %rax,(%rdi)
  32:	b8 fd ff ff ff       	mov    $0xfffffffd,%eax
  37:	c3                   	ret
  38:	b8 ff ff ff ff       	mov    $0xffffffff,%eax
  3d:	c3                   	ret
  3e:	44 8b 07             	mov    (%rdi),%r8d
  41:	41 39 d0             	cmp    %edx,%r8d
  44:	0f 93 c1             	setae  %cl
  47:	41 81 f8 81 00 00 00 	cmp    $0x81,%r8d
  4e:	0f 93 c2             	setae  %dl
  51:	08 ca                	or     %cl,%dl
  53:	75 c0                	jne    15 <input_console_take_line+0x15>
  55:	49 8d 48 01          	lea    0x1(%r8),%rcx
  59:	48 89 f2             	mov    %rsi,%rdx
  5c:	48 f7 da             	neg    %rdx
  5f:	48 39 ca             	cmp    %rcx,%rdx
  62:	76 b1                	jbe    15 <input_console_take_line+0x15>
  64:	45 85 c0             	test   %r8d,%r8d
  67:	74 0f                	je     78 <input_console_take_line+0x78>
  69:	89 c8                	mov    %ecx,%eax
  6b:	83 e0 03             	and    $0x3,%eax
  6e:	41 83 f8 03          	cmp    $0x3,%r8d
  72:	73 17                	jae    8b <input_console_take_line+0x8b>
  74:	31 d2                	xor    %edx,%edx
  76:	eb 61                	jmp    d9 <input_console_take_line+0xd9>
  78:	0f b6 47 04          	movzbl 0x4(%rdi),%eax
  7c:	88 06                	mov    %al,(%rsi)
  7e:	b9 12 00 00 00       	mov    $0x12,%ecx
  83:	31 c0                	xor    %eax,%eax
  85:	f3 48 ab             	rep stos %rax,(%rdi)
  88:	31 c0                	xor    %eax,%eax
  8a:	c3                   	ret
  8b:	81 e1 fc 01 00 00    	and    $0x1fc,%ecx
  91:	31 d2                	xor    %edx,%edx
  93:	66 66 66 66 2e 0f 1f 	data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
  9a:	84 00 00 00 00 00 
  a0:	44 0f b6 4c 17 04    	movzbl 0x4(%rdi,%rdx,1),%r9d
  a6:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
  aa:	44 0f b6 4c 17 05    	movzbl 0x5(%rdi,%rdx,1),%r9d
  b0:	44 88 4c 16 01       	mov    %r9b,0x1(%rsi,%rdx,1)
  b5:	44 0f b6 4c 17 06    	movzbl 0x6(%rdi,%rdx,1),%r9d
  bb:	44 88 4c 16 02       	mov    %r9b,0x2(%rsi,%rdx,1)
  c0:	44 0f b6 4c 17 07    	movzbl 0x7(%rdi,%rdx,1),%r9d
  c6:	44 88 4c 16 03       	mov    %r9b,0x3(%rsi,%rdx,1)
  cb:	48 83 c2 04          	add    $0x4,%rdx
  cf:	48 39 d1             	cmp    %rdx,%rcx
  d2:	75 cc                	jne    a0 <input_console_take_line+0xa0>
  d4:	48 85 c0             	test   %rax,%rax
  d7:	74 28                	je     101 <input_console_take_line+0x101>
  d9:	48 01 d6             	add    %rdx,%rsi
  dc:	48 8d 0c 3a          	lea    (%rdx,%rdi,1),%rcx
  e0:	48 83 c1 04          	add    $0x4,%rcx
  e4:	31 d2                	xor    %edx,%edx
  e6:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
  ed:	00 00 00 
  f0:	44 0f b6 0c 11       	movzbl (%rcx,%rdx,1),%r9d
  f5:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
  f9:	48 ff c2             	inc    %rdx
  fc:	48 39 d0             	cmp    %rdx,%rax
  ff:	75 ef                	jne    f0 <input_console_take_line+0xf0>
 101:	b9 12 00 00 00       	mov    $0x12,%ecx
 106:	31 c0                	xor    %eax,%eax
 108:	f3 48 ab             	rep stos %rax,(%rdi)
 10b:	44 89 c0             	mov    %r8d,%eax
 10e:	c3                   	ret
