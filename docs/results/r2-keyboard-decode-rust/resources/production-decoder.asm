
/home/veasnawt/src/vos-r2-keyboard/build/candidate/vos.bin:     file format elf64-x86-64


Disassembly of section .boot.text:

Disassembly of section .text:

ffffffff80140200 <keyboard_decode>:
ffffffff80140200:	48 8d 47 ff          	lea    -0x1(%rdi),%rax
ffffffff80140204:	48 83 f8 f7          	cmp    $0xfffffffffffffff7,%rax
ffffffff80140208:	77 3a                	ja     ffffffff80140244 <keyboard_decode+0x44>
ffffffff8014020a:	53                   	push   %rbx
ffffffff8014020b:	48 83 ec 02          	sub    $0x2,%rsp
ffffffff8014020f:	0f b6 17             	movzbl (%rdi),%edx
ffffffff80140212:	44 0f b6 57 01       	movzbl 0x1(%rdi),%r10d
ffffffff80140217:	44 88 14 24          	mov    %r10b,(%rsp)
ffffffff8014021b:	44 0f b6 4f 02       	movzbl 0x2(%rdi),%r9d
ffffffff80140220:	44 88 4c 24 01       	mov    %r9b,0x1(%rsp)
ffffffff80140225:	0f b6 4f 03          	movzbl 0x3(%rdi),%ecx
ffffffff80140229:	44 0f b6 47 04       	movzbl 0x4(%rdi),%r8d
ffffffff8014022e:	0f b6 5f 05          	movzbl 0x5(%rdi),%ebx
ffffffff80140232:	44 0f b6 5f 06       	movzbl 0x6(%rdi),%r11d
ffffffff80140237:	45 84 db             	test   %r11b,%r11b
ffffffff8014023a:	74 0b                	je     ffffffff80140247 <keyboard_decode+0x47>
ffffffff8014023c:	41 fe cb             	dec    %r11b
ffffffff8014023f:	e9 90 00 00 00       	jmp    ffffffff801402d4 <keyboard_decode+0xd4>
ffffffff80140244:	31 c0                	xor    %eax,%eax
ffffffff80140246:	c3                   	ret
ffffffff80140247:	40 80 fe e0          	cmp    $0xe0,%sil
ffffffff8014024b:	74 12                	je     ffffffff8014025f <keyboard_decode+0x5f>
ffffffff8014024d:	44 0f b6 de          	movzbl %sil,%r11d
ffffffff80140251:	41 81 fb e1 00 00 00 	cmp    $0xe1,%r11d
ffffffff80140258:	75 0c                	jne    ffffffff80140266 <keyboard_decode+0x66>
ffffffff8014025a:	41 b3 05             	mov    $0x5,%r11b
ffffffff8014025d:	eb 73                	jmp    ffffffff801402d2 <keyboard_decode+0xd2>
ffffffff8014025f:	b3 01                	mov    $0x1,%bl
ffffffff80140261:	45 31 db             	xor    %r11d,%r11d
ffffffff80140264:	eb 6e                	jmp    ffffffff801402d4 <keyboard_decode+0xd4>
ffffffff80140266:	41 83 e3 7f          	and    $0x7f,%r11d
ffffffff8014026a:	84 db                	test   %bl,%bl
ffffffff8014026c:	74 17                	je     ffffffff80140285 <keyboard_decode+0x85>
ffffffff8014026e:	b0 02                	mov    $0x2,%al
ffffffff80140270:	41 80 fb 1d          	cmp    $0x1d,%r11b
ffffffff80140274:	74 24                	je     ffffffff8014029a <keyboard_decode+0x9a>
ffffffff80140276:	45 0f b6 db          	movzbl %r11b,%r11d
ffffffff8014027a:	41 83 fb 38          	cmp    $0x38,%r11d
ffffffff8014027e:	74 22                	je     ffffffff801402a2 <keyboard_decode+0xa2>
ffffffff80140280:	45 31 db             	xor    %r11d,%r11d
ffffffff80140283:	eb 4d                	jmp    ffffffff801402d2 <keyboard_decode+0xd2>
ffffffff80140285:	41 0f b6 db          	movzbl %r11b,%ebx
ffffffff80140289:	83 c3 e3             	add    $0xffffffe3,%ebx
ffffffff8014028c:	83 fb 1d             	cmp    $0x1d,%ebx
ffffffff8014028f:	77 63                	ja     ffffffff801402f4 <keyboard_decode+0xf4>
ffffffff80140291:	b0 01                	mov    $0x1,%al
ffffffff80140293:	ff 24 dd 50 59 15 80 	jmp    *-0x7feaa6b0(,%rbx,8)
ffffffff8014029a:	49 89 e3             	mov    %rsp,%r11
ffffffff8014029d:	45 89 d1             	mov    %r10d,%r9d
ffffffff801402a0:	eb 05                	jmp    ffffffff801402a7 <keyboard_decode+0xa7>
ffffffff801402a2:	4c 8d 5c 24 01       	lea    0x1(%rsp),%r11
ffffffff801402a7:	41 89 c2             	mov    %eax,%r10d
ffffffff801402aa:	41 f6 d2             	not    %r10b
ffffffff801402ad:	45 20 ca             	and    %r9b,%r10b
ffffffff801402b0:	41 08 c1             	or     %al,%r9b
ffffffff801402b3:	40 84 f6             	test   %sil,%sil
ffffffff801402b6:	41 0f b6 c1          	movzbl %r9b,%eax
ffffffff801402ba:	41 0f b6 f2          	movzbl %r10b,%esi
ffffffff801402be:	0f 49 f0             	cmovns %eax,%esi
ffffffff801402c1:	41 88 33             	mov    %sil,(%r11)
ffffffff801402c4:	44 0f b6 14 24       	movzbl (%rsp),%r10d
ffffffff801402c9:	45 31 db             	xor    %r11d,%r11d
ffffffff801402cc:	44 0f b6 4c 24 01    	movzbl 0x1(%rsp),%r9d
ffffffff801402d2:	31 db                	xor    %ebx,%ebx
ffffffff801402d4:	31 c0                	xor    %eax,%eax
ffffffff801402d6:	88 17                	mov    %dl,(%rdi)
ffffffff801402d8:	44 88 57 01          	mov    %r10b,0x1(%rdi)
ffffffff801402dc:	44 88 4f 02          	mov    %r9b,0x2(%rdi)
ffffffff801402e0:	88 4f 03             	mov    %cl,0x3(%rdi)
ffffffff801402e3:	44 88 47 04          	mov    %r8b,0x4(%rdi)
ffffffff801402e7:	88 5f 05             	mov    %bl,0x5(%rdi)
ffffffff801402ea:	44 88 5f 06          	mov    %r11b,0x6(%rdi)
ffffffff801402ee:	48 83 c4 02          	add    $0x2,%rsp
ffffffff801402f2:	5b                   	pop    %rbx
ffffffff801402f3:	c3                   	ret
ffffffff801402f4:	40 84 f6             	test   %sil,%sil
ffffffff801402f7:	0f 99 c0             	setns  %al
ffffffff801402fa:	44 89 d6             	mov    %r10d,%esi
ffffffff801402fd:	44 08 ce             	or     %r9b,%sil
ffffffff80140300:	40 0f 94 c6          	sete   %sil
ffffffff80140304:	40 20 c6             	and    %al,%sil
ffffffff80140307:	40 80 fe 01          	cmp    $0x1,%sil
ffffffff8014030b:	0f 85 6f ff ff ff    	jne    ffffffff80140280 <keyboard_decode+0x80>
ffffffff80140311:	41 0f b6 83 2d 60 15 	movzbl -0x7fea9fd3(%r11),%eax
ffffffff80140318:	80 
ffffffff80140319:	3c 60                	cmp    $0x60,%al
ffffffff8014031b:	76 5b                	jbe    ffffffff80140378 <keyboard_decode+0x178>
ffffffff8014031d:	84 d2                	test   %dl,%dl
ffffffff8014031f:	40 0f 95 c6          	setne  %sil
ffffffff80140323:	44 8d 48 e0          	lea    -0x20(%rax),%r9d
ffffffff80140327:	40 38 f1             	cmp    %sil,%cl
ffffffff8014032a:	0f b6 f0             	movzbl %al,%esi
ffffffff8014032d:	41 0f b6 c1          	movzbl %r9b,%eax
ffffffff80140331:	0f 44 c6             	cmove  %esi,%eax
ffffffff80140334:	eb 57                	jmp    ffffffff8014038d <keyboard_decode+0x18d>
ffffffff80140336:	b0 02                	mov    $0x2,%al
ffffffff80140338:	41 89 c3             	mov    %eax,%r11d
ffffffff8014033b:	41 f6 d3             	not    %r11b
ffffffff8014033e:	41 20 d3             	and    %dl,%r11b
ffffffff80140341:	08 d0                	or     %dl,%al
ffffffff80140343:	40 84 f6             	test   %sil,%sil
ffffffff80140346:	0f b6 c0             	movzbl %al,%eax
ffffffff80140349:	41 0f b6 d3          	movzbl %r11b,%edx
ffffffff8014034d:	0f 49 d0             	cmovns %eax,%edx
ffffffff80140350:	45 31 db             	xor    %r11d,%r11d
ffffffff80140353:	e9 7a ff ff ff       	jmp    ffffffff801402d2 <keyboard_decode+0xd2>
ffffffff80140358:	40 84 f6             	test   %sil,%sil
ffffffff8014035b:	0f 99 c0             	setns  %al
ffffffff8014035e:	45 84 c0             	test   %r8b,%r8b
ffffffff80140361:	40 0f 94 c6          	sete   %sil
ffffffff80140365:	40 20 c6             	and    %al,%sil
ffffffff80140368:	40 30 f1             	xor    %sil,%cl
ffffffff8014036b:	45 31 db             	xor    %r11d,%r11d
ffffffff8014036e:	31 db                	xor    %ebx,%ebx
ffffffff80140370:	41 89 c0             	mov    %eax,%r8d
ffffffff80140373:	e9 5c ff ff ff       	jmp    ffffffff801402d4 <keyboard_decode+0xd4>
ffffffff80140378:	84 d2                	test   %dl,%dl
ffffffff8014037a:	74 21                	je     ffffffff8014039d <keyboard_decode+0x19d>
ffffffff8014037c:	41 0f b6 b3 ad 60 15 	movzbl -0x7fea9f53(%r11),%esi
ffffffff80140383:	80 
ffffffff80140384:	40 84 f6             	test   %sil,%sil
ffffffff80140387:	0f b6 c0             	movzbl %al,%eax
ffffffff8014038a:	0f 45 c6             	cmovne %esi,%eax
ffffffff8014038d:	45 31 c9             	xor    %r9d,%r9d
ffffffff80140390:	45 31 d2             	xor    %r10d,%r10d
ffffffff80140393:	45 31 db             	xor    %r11d,%r11d
ffffffff80140396:	31 db                	xor    %ebx,%ebx
ffffffff80140398:	e9 39 ff ff ff       	jmp    ffffffff801402d6 <keyboard_decode+0xd6>
ffffffff8014039d:	45 31 c9             	xor    %r9d,%r9d
ffffffff801403a0:	45 31 d2             	xor    %r10d,%r10d
ffffffff801403a3:	45 31 db             	xor    %r11d,%r11d
ffffffff801403a6:	31 db                	xor    %ebx,%ebx
ffffffff801403a8:	31 d2                	xor    %edx,%edx
ffffffff801403aa:	e9 27 ff ff ff       	jmp    ffffffff801402d6 <keyboard_decode+0xd6>
