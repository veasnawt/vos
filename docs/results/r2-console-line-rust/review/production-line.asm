
/home/veasnawt/src/vos-r2-console-line/build/final/vos.bin:     file format elf64-x86-64


Disassembly of section .boot.text:

Disassembly of section .text:

ffffffff801400e0 <input_console_take_line>:
ffffffff801400e0:	48 8d 8f 90 00 00 00 	lea    0x90(%rdi),%rcx
ffffffff801400e7:	b8 fe ff ff ff       	mov    $0xfffffffe,%eax
ffffffff801400ec:	48 81 f9 91 00 00 00 	cmp    $0x91,%rcx
ffffffff801400f3:	73 01                	jae    ffffffff801400f6 <input_console_take_line+0x16>
ffffffff801400f5:	c3                   	ret
ffffffff801400f6:	83 bf 88 00 00 00 00 	cmpl   $0x0,0x88(%rdi)
ffffffff801400fd:	74 19                	je     ffffffff80140118 <input_console_take_line+0x38>
ffffffff801400ff:	83 bf 8c 00 00 00 00 	cmpl   $0x0,0x8c(%rdi)
ffffffff80140106:	74 16                	je     ffffffff8014011e <input_console_take_line+0x3e>
ffffffff80140108:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff8014010d:	31 c0                	xor    %eax,%eax
ffffffff8014010f:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff80140112:	b8 fd ff ff ff       	mov    $0xfffffffd,%eax
ffffffff80140117:	c3                   	ret
ffffffff80140118:	b8 ff ff ff ff       	mov    $0xffffffff,%eax
ffffffff8014011d:	c3                   	ret
ffffffff8014011e:	44 8b 07             	mov    (%rdi),%r8d
ffffffff80140121:	81 fa 81 00 00 00    	cmp    $0x81,%edx
ffffffff80140127:	b9 81 00 00 00       	mov    $0x81,%ecx
ffffffff8014012c:	0f 42 ca             	cmovb  %edx,%ecx
ffffffff8014012f:	41 39 c8             	cmp    %ecx,%r8d
ffffffff80140132:	73 c1                	jae    ffffffff801400f5 <input_console_take_line+0x15>
ffffffff80140134:	49 8d 48 01          	lea    0x1(%r8),%rcx
ffffffff80140138:	48 89 f2             	mov    %rsi,%rdx
ffffffff8014013b:	48 f7 da             	neg    %rdx
ffffffff8014013e:	48 39 ca             	cmp    %rcx,%rdx
ffffffff80140141:	76 b2                	jbe    ffffffff801400f5 <input_console_take_line+0x15>
ffffffff80140143:	45 85 c0             	test   %r8d,%r8d
ffffffff80140146:	74 0f                	je     ffffffff80140157 <input_console_take_line+0x77>
ffffffff80140148:	89 c8                	mov    %ecx,%eax
ffffffff8014014a:	83 e0 03             	and    $0x3,%eax
ffffffff8014014d:	41 83 f8 03          	cmp    $0x3,%r8d
ffffffff80140151:	73 17                	jae    ffffffff8014016a <input_console_take_line+0x8a>
ffffffff80140153:	31 d2                	xor    %edx,%edx
ffffffff80140155:	eb 52                	jmp    ffffffff801401a9 <input_console_take_line+0xc9>
ffffffff80140157:	0f b6 47 04          	movzbl 0x4(%rdi),%eax
ffffffff8014015b:	88 06                	mov    %al,(%rsi)
ffffffff8014015d:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff80140162:	31 c0                	xor    %eax,%eax
ffffffff80140164:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff80140167:	31 c0                	xor    %eax,%eax
ffffffff80140169:	c3                   	ret
ffffffff8014016a:	83 e1 fc             	and    $0xfffffffc,%ecx
ffffffff8014016d:	31 d2                	xor    %edx,%edx
ffffffff8014016f:	90                   	nop
ffffffff80140170:	44 0f b6 4c 17 04    	movzbl 0x4(%rdi,%rdx,1),%r9d
ffffffff80140176:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
ffffffff8014017a:	44 0f b6 4c 17 05    	movzbl 0x5(%rdi,%rdx,1),%r9d
ffffffff80140180:	44 88 4c 16 01       	mov    %r9b,0x1(%rsi,%rdx,1)
ffffffff80140185:	44 0f b6 4c 17 06    	movzbl 0x6(%rdi,%rdx,1),%r9d
ffffffff8014018b:	44 88 4c 16 02       	mov    %r9b,0x2(%rsi,%rdx,1)
ffffffff80140190:	44 0f b6 4c 17 07    	movzbl 0x7(%rdi,%rdx,1),%r9d
ffffffff80140196:	44 88 4c 16 03       	mov    %r9b,0x3(%rsi,%rdx,1)
ffffffff8014019b:	48 83 c2 04          	add    $0x4,%rdx
ffffffff8014019f:	48 39 d1             	cmp    %rdx,%rcx
ffffffff801401a2:	75 cc                	jne    ffffffff80140170 <input_console_take_line+0x90>
ffffffff801401a4:	48 85 c0             	test   %rax,%rax
ffffffff801401a7:	74 28                	je     ffffffff801401d1 <input_console_take_line+0xf1>
ffffffff801401a9:	48 01 d6             	add    %rdx,%rsi
ffffffff801401ac:	48 8d 0c 3a          	lea    (%rdx,%rdi,1),%rcx
ffffffff801401b0:	48 83 c1 04          	add    $0x4,%rcx
ffffffff801401b4:	31 d2                	xor    %edx,%edx
ffffffff801401b6:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
ffffffff801401bd:	00 00 00 
ffffffff801401c0:	44 0f b6 0c 11       	movzbl (%rcx,%rdx,1),%r9d
ffffffff801401c5:	44 88 0c 16          	mov    %r9b,(%rsi,%rdx,1)
ffffffff801401c9:	48 ff c2             	inc    %rdx
ffffffff801401cc:	48 39 d0             	cmp    %rdx,%rax
ffffffff801401cf:	75 ef                	jne    ffffffff801401c0 <input_console_take_line+0xe0>
ffffffff801401d1:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff801401d6:	31 c0                	xor    %eax,%eax
ffffffff801401d8:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff801401db:	44 89 c0             	mov    %r8d,%eax
ffffffff801401de:	c3                   	ret
