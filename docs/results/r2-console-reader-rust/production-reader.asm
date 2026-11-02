ffffffff8013bec0 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted>:
ffffffff8013bec0:	55                   	push   %rbp
ffffffff8013bec1:	41 57                	push   %r15
ffffffff8013bec3:	41 56                	push   %r14
ffffffff8013bec5:	41 55                	push   %r13
ffffffff8013bec7:	41 54                	push   %r12
ffffffff8013bec9:	53                   	push   %rbx
ffffffff8013beca:	48 81 ec a8 00 00 00 	sub    $0xa8,%rsp
ffffffff8013bed1:	4d 89 cf             	mov    %r9,%r15
ffffffff8013bed4:	4d 89 c4             	mov    %r8,%r12
ffffffff8013bed7:	49 89 cd             	mov    %rcx,%r13
ffffffff8013beda:	89 54 24 0c          	mov    %edx,0xc(%rsp)
ffffffff8013bede:	48 89 74 24 10       	mov    %rsi,0x10(%rsp)
ffffffff8013bee3:	49 89 fe             	mov    %rdi,%r14
ffffffff8013bee6:	48 8d 5c 24 18       	lea    0x18(%rsp),%rbx
ffffffff8013beeb:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff8013bef0:	31 c0                	xor    %eax,%eax
ffffffff8013bef2:	48 89 df             	mov    %rbx,%rdi
ffffffff8013bef5:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff8013bef8:	4c 89 ef             	mov    %r13,%rdi
ffffffff8013befb:	e8 90 c8 fc ff       	call   ffffffff80108790 <serial_puts>
ffffffff8013bf00:	48 8d ac 24 a8 00 00 	lea    0xa8(%rsp),%rbp
ffffffff8013bf07:	00 
ffffffff8013bf08:	eb 10                	jmp    ffffffff8013bf1a <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x5a>
ffffffff8013bf0a:	66 0f 1f 44 00 00    	nopw   0x0(%rax,%rax,1)
ffffffff8013bf10:	bf 01 00 00 00       	mov    $0x1,%edi
ffffffff8013bf15:	e8 96 4f fd ff       	call   ffffffff80110eb0 <thread_sleep>
ffffffff8013bf1a:	4d 85 e4             	test   %r12,%r12
ffffffff8013bf1d:	74 0a                	je     ffffffff8013bf29 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x69>
ffffffff8013bf1f:	4c 89 ff             	mov    %r15,%rdi
ffffffff8013bf22:	41 ff d4             	call   *%r12
ffffffff8013bf25:	85 c0                	test   %eax,%eax
ffffffff8013bf27:	75 5c                	jne    ffffffff8013bf85 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0xc5>
ffffffff8013bf29:	4c 89 f7             	mov    %r14,%rdi
ffffffff8013bf2c:	e8 4f 81 fe ff       	call   ffffffff80124080 <keyboard_poll>
ffffffff8013bf31:	0f be f0             	movsbl %al,%esi
ffffffff8013bf34:	48 89 df             	mov    %rbx,%rdi
ffffffff8013bf37:	e8 34 41 00 00       	call   ffffffff80140070 <input_console_accept>
ffffffff8013bf3c:	83 bc 24 a0 00 00 00 	cmpl   $0x0,0xa0(%rsp)
ffffffff8013bf43:	00 
ffffffff8013bf44:	74 ca                	je     ffffffff8013bf10 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x50>
ffffffff8013bf46:	b8 fe ff ff ff       	mov    $0xfffffffe,%eax
ffffffff8013bf4b:	48 81 fd 91 00 00 00 	cmp    $0x91,%rbp
ffffffff8013bf52:	0f 82 9c 00 00 00    	jb     ffffffff8013bff4 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x134>
ffffffff8013bf58:	83 bc 24 a4 00 00 00 	cmpl   $0x0,0xa4(%rsp)
ffffffff8013bf5f:	00 
ffffffff8013bf60:	74 2a                	je     ffffffff8013bf8c <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0xcc>
ffffffff8013bf62:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff8013bf67:	31 c0                	xor    %eax,%eax
ffffffff8013bf69:	48 89 df             	mov    %rbx,%rdi
ffffffff8013bf6c:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff8013bf6f:	48 c7 c7 7b dd 14 80 	mov    $0xffffffff8014dd7b,%rdi
ffffffff8013bf76:	e8 15 c8 fc ff       	call   ffffffff80108790 <serial_puts>
ffffffff8013bf7b:	4c 89 ef             	mov    %r13,%rdi
ffffffff8013bf7e:	e8 0d c8 fc ff       	call   ffffffff80108790 <serial_puts>
ffffffff8013bf83:	eb 8b                	jmp    ffffffff8013bf10 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x50>
ffffffff8013bf85:	b8 fb ff ff ff       	mov    $0xfffffffb,%eax
ffffffff8013bf8a:	eb 68                	jmp    ffffffff8013bff4 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x134>
ffffffff8013bf8c:	44 8b 74 24 18       	mov    0x18(%rsp),%r14d
ffffffff8013bf91:	8b 54 24 0c          	mov    0xc(%rsp),%edx
ffffffff8013bf95:	81 fa 81 00 00 00    	cmp    $0x81,%edx
ffffffff8013bf9b:	b9 81 00 00 00       	mov    $0x81,%ecx
ffffffff8013bfa0:	0f 42 ca             	cmovb  %edx,%ecx
ffffffff8013bfa3:	41 39 ce             	cmp    %ecx,%r14d
ffffffff8013bfa6:	73 4c                	jae    ffffffff8013bff4 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x134>
ffffffff8013bfa8:	49 8d 56 01          	lea    0x1(%r14),%rdx
ffffffff8013bfac:	48 8b 7c 24 10       	mov    0x10(%rsp),%rdi
ffffffff8013bfb1:	48 89 f9             	mov    %rdi,%rcx
ffffffff8013bfb4:	48 f7 d9             	neg    %rcx
ffffffff8013bfb7:	48 39 d1             	cmp    %rdx,%rcx
ffffffff8013bfba:	76 38                	jbe    ffffffff8013bff4 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x134>
ffffffff8013bfbc:	45 85 f6             	test   %r14d,%r14d
ffffffff8013bfbf:	74 1d                	je     ffffffff8013bfde <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x11e>
ffffffff8013bfc1:	48 8d 74 24 1c       	lea    0x1c(%rsp),%rsi
ffffffff8013bfc6:	ff 15 04 c2 01 00    	call   *0x1c204(%rip)        # ffffffff801581d0 <_RNvNtCs5YZeFliBs6N_10vos_kernel8xhci_dma8XHCI_DMA+0xf8>
ffffffff8013bfcc:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff8013bfd1:	31 c0                	xor    %eax,%eax
ffffffff8013bfd3:	48 89 df             	mov    %rbx,%rdi
ffffffff8013bfd6:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff8013bfd9:	44 89 f0             	mov    %r14d,%eax
ffffffff8013bfdc:	eb 16                	jmp    ffffffff8013bff4 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted+0x134>
ffffffff8013bfde:	0f b6 44 24 1c       	movzbl 0x1c(%rsp),%eax
ffffffff8013bfe3:	88 07                	mov    %al,(%rdi)
ffffffff8013bfe5:	b9 12 00 00 00       	mov    $0x12,%ecx
ffffffff8013bfea:	31 c0                	xor    %eax,%eax
ffffffff8013bfec:	48 89 df             	mov    %rbx,%rdi
ffffffff8013bfef:	f3 48 ab             	rep stos %rax,(%rdi)
ffffffff8013bff2:	31 c0                	xor    %eax,%eax
ffffffff8013bff4:	48 81 c4 a8 00 00 00 	add    $0xa8,%rsp
ffffffff8013bffb:	5b                   	pop    %rbx
ffffffff8013bffc:	41 5c                	pop    %r12
ffffffff8013bffe:	41 5d                	pop    %r13
ffffffff8013c000:	41 5e                	pop    %r14
ffffffff8013c002:	41 5f                	pop    %r15
ffffffff8013c004:	5d                   	pop    %rbp
ffffffff8013c005:	c3                   	ret
ffffffff8013c006:	cc                   	int3
ffffffff8013c007:	cc                   	int3
ffffffff8013c008:	cc                   	int3
ffffffff8013c009:	cc                   	int3
ffffffff8013c00a:	cc                   	int3
ffffffff8013c00b:	cc                   	int3
ffffffff8013c00c:	cc                   	int3
ffffffff8013c00d:	cc                   	int3
ffffffff8013c00e:	cc                   	int3
ffffffff8013c00f:	cc                   	int3

ffffffff80140120 <input_console_read_line_for>:
ffffffff80140120:	4c 8d 57 14          	lea    0x14(%rdi),%r10
ffffffff80140124:	b8 fc ff ff ff       	mov    $0xfffffffc,%eax
ffffffff80140129:	49 83 fa 15          	cmp    $0x15,%r10
ffffffff8014012d:	72 24                	jb     ffffffff80140153 <input_console_read_line_for+0x33>
ffffffff8014012f:	83 7f 10 00          	cmpl   $0x0,0x10(%rdi)
ffffffff80140133:	74 1e                	je     ffffffff80140153 <input_console_read_line_for+0x33>
ffffffff80140135:	48 85 f6             	test   %rsi,%rsi
ffffffff80140138:	0f 94 c0             	sete   %al
ffffffff8014013b:	81 fa 81 00 00 00    	cmp    $0x81,%edx
ffffffff80140141:	41 0f 92 c2          	setb   %r10b
ffffffff80140145:	41 08 c2             	or     %al,%r10b
ffffffff80140148:	b8 fe ff ff ff       	mov    $0xfffffffe,%eax
ffffffff8014014d:	0f 84 6d bd ff ff    	je     ffffffff8013bec0 <_RNvNtCs5YZeFliBs6N_10vos_kernel14console_reader8admitted>
ffffffff80140153:	c3                   	ret
ffffffff80140154:	cc                   	int3
ffffffff80140155:	cc                   	int3
ffffffff80140156:	cc                   	int3
ffffffff80140157:	cc                   	int3
ffffffff80140158:	cc                   	int3
ffffffff80140159:	cc                   	int3
ffffffff8014015a:	cc                   	int3
ffffffff8014015b:	cc                   	int3
ffffffff8014015c:	cc                   	int3
ffffffff8014015d:	cc                   	int3
ffffffff8014015e:	cc                   	int3
ffffffff8014015f:	cc                   	int3
