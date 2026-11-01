
/home/veasnawt/src/vos-r2-console-line/build/clear-probe/clear.o:     file format elf64-x86-64


Disassembly of section .text.fields:

0000000000000000 <fields>:
   0:	53                   	push   %rbx
   1:	48 89 fb             	mov    %rdi,%rbx
   4:	c7 07 00 00 00 00    	movl   $0x0,(%rdi)
   a:	48 83 c7 04          	add    $0x4,%rdi
   e:	ba 81 00 00 00       	mov    $0x81,%edx
  13:	31 f6                	xor    %esi,%esi
  15:	ff 15 00 00 00 00    	call   *0x0(%rip)        # 1b <fields+0x1b>
			17: R_X86_64_GOTPCREL	memset-0x4
  1b:	48 c7 83 88 00 00 00 	movq   $0x0,0x88(%rbx)
  22:	00 00 00 00 
  26:	5b                   	pop    %rbx
  27:	c3                   	ret

Disassembly of section .text.wide:

0000000000000000 <wide>:
   0:	ba 90 00 00 00       	mov    $0x90,%edx
   5:	31 f6                	xor    %esi,%esi
   7:	ff 25 00 00 00 00    	jmp    *0x0(%rip)        # d <wide+0xd>
			9: R_X86_64_GOTPCREL	memset-0x4
