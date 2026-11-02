
/home/veasnawt/src/vos-r2-pointer-poll/build/production/vos.bin:     file format elf64-x86-64


Disassembly of section .boot.text:

Disassembly of section .text:

ffffffff80140d60 <vos_rust_keyboard_pointer_lost>:
ffffffff80140d60:	31 c0                	xor    %eax,%eax
ffffffff80140d62:	48 85 ff             	test   %rdi,%rdi
ffffffff80140d65:	74 1c                	je     ffffffff80140d83 <vos_rust_keyboard_pointer_lost+0x23>
ffffffff80140d67:	40 f6 c7 07          	test   $0x7,%dil
ffffffff80140d6b:	0f 95 c1             	setne  %cl
ffffffff80140d6e:	48 83 ff a8          	cmp    $0xffffffffffffffa8,%rdi
ffffffff80140d72:	0f 93 c2             	setae  %dl
ffffffff80140d75:	08 ca                	or     %cl,%dl
ffffffff80140d77:	75 0a                	jne    ffffffff80140d83 <vos_rust_keyboard_pointer_lost+0x23>
ffffffff80140d79:	8b 47 28             	mov    0x28(%rdi),%eax
ffffffff80140d7c:	c7 47 28 00 00 00 00 	movl   $0x0,0x28(%rdi)
ffffffff80140d83:	c3                   	ret
