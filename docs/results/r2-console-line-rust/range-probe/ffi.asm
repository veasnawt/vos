
/home/veasnawt/src/vos-r2-console-line/build/range-probe/ffi.o:     file format elf64-x86-64


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
  41:	49 81 f8 80 00 00 00 	cmp    $0x80,%r8
  48:	77 cb                	ja     15 <input_console_take_line+0x15>
  4a:	41 39 d0             	cmp    %edx,%r8d
  4d:	73 c6                	jae    15 <input_console_take_line+0x15>
  4f:	4c 89 c1             	mov    %r8,%rcx
  52:	48 ff c1             	inc    %rcx
  55:	48 89 f2             	mov    %rsi,%rdx
  58:	48 f7 da             	neg    %rdx
  5b:	48 39 ca             	cmp    %rcx,%rdx
  5e:	76 b5                	jbe    15 <input_console_take_line+0x15>
  60:	89 c8                	mov    %ecx,%eax
  62:	83 e0 03             	and    $0x3,%eax
  65:	41 83 f8 03          	cmp    $0x3,%r8d
  69:	73 04                	jae    6f <input_console_take_line+0x6f>
  6b:	31 d2                	xor    %edx,%edx
  6d:	eb 4a                	jmp    b9 <input_console_take_line+0xb9>
  6f:	81 e1 fc 01 00 00    	and    $0x1fc,%ecx
  75:	31 d2                	xor    %edx,%edx
  77:	66 0f 1f 84 00 00 00 	nopw   0x0(%rax,%rax,1)
  7e:	00 00 
  80:	44 0f b6 4c 17 04    	movzbl 0x4(%rdi,%rdx,1),%r9d
  86:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
  8a:	44 0f b6 4c 17 05    	movzbl 0x5(%rdi,%rdx,1),%r9d
  90:	44 88 4c 16 01       	mov    %r9b,0x1(%rsi,%rdx,1)
  95:	44 0f b6 4c 17 06    	movzbl 0x6(%rdi,%rdx,1),%r9d
  9b:	44 88 4c 16 02       	mov    %r9b,0x2(%rsi,%rdx,1)
  a0:	44 0f b6 4c 17 07    	movzbl 0x7(%rdi,%rdx,1),%r9d
  a6:	44 88 4c 16 03       	mov    %r9b,0x3(%rsi,%rdx,1)
  ab:	48 83 c2 04          	add    $0x4,%rdx
  af:	48 39 d1             	cmp    %rdx,%rcx
  b2:	75 cc                	jne    80 <input_console_take_line+0x80>
  b4:	48 85 c0             	test   %rax,%rax
  b7:	74 28                	je     e1 <input_console_take_line+0xe1>
  b9:	48 01 d6             	add    %rdx,%rsi
  bc:	48 8d 0c 3a          	lea    (%rdx,%rdi,1),%rcx
  c0:	48 83 c1 04          	add    $0x4,%rcx
  c4:	31 d2                	xor    %edx,%edx
  c6:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
  cd:	00 00 00 
  d0:	44 0f b6 0c 11       	movzbl (%rcx,%rdx,1),%r9d
  d5:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
  d9:	48 ff c2             	inc    %rdx
  dc:	48 39 d0             	cmp    %rdx,%rax
  df:	75 ef                	jne    d0 <input_console_take_line+0xd0>
  e1:	b9 12 00 00 00       	mov    $0x12,%ecx
  e6:	31 c0                	xor    %eax,%eax
  e8:	f3 48 ab             	rep stos %rax,(%rdi)
  eb:	44 89 c0             	mov    %r8d,%eax
  ee:	c3                   	ret
