# Final kernel 7c8dd5dece76ddfde861afefb8f43eb56131c55440ed2d95676fc0c19526b766: the five Rust lifecycle entries and every reference to them.
# References (5): the C ABI functions forward (call/tail-jump) and pointer_start reaches suspend/resume out of line.
<keyboard_init>:  ffffffff80123ab0:	jmp    ffffffff80140610 <vos_rust_keyboard_init>
<keyboard_suspend>:  ffffffff80123ae0:	jmp    ffffffff80140e70 <vos_rust_keyboard_suspend>
<keyboard_resume>:  ffffffff80123af0:	jmp    ffffffff80140c90 <vos_rust_keyboard_resume>
<keyboard_pointer_start>:  ffffffff80123b00:	jmp    ffffffff801409a0 <vos_rust_keyboard_pointer_start>
<keyboard_pointer_stop>:  ffffffff80123b10:	jmp    ffffffff80140b20 <vos_rust_keyboard_pointer_stop>

ffffffff80140610 <vos_rust_keyboard_init>:
ffffffff80140610:	lea    -0x1(%rdi),%rax
ffffffff80140614:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140618:	setae  %cl
ffffffff8014061b:	test   $0x7,%dil
ffffffff8014061f:	setne  %dl
ffffffff80140622:	xor    %eax,%eax
ffffffff80140624:	or     %cl,%dl
ffffffff80140626:	jne    ffffffff801407ff <vos_rust_keyboard_init+0x1ef>
ffffffff8014062c:	push   %rbp
ffffffff8014062d:	push   %r14
ffffffff8014062f:	push   %rbx
ffffffff80140630:	sub    $0x10,%rsp
ffffffff80140634:	mov    %rsi,%r14
ffffffff80140637:	movq   $0x0,0x50(%rdi)
ffffffff8014063f:	movq   $0x0,0x48(%rdi)
ffffffff80140647:	movq   $0x0,0x40(%rdi)
ffffffff8014064f:	movq   $0x0,0x38(%rdi)
ffffffff80140657:	movq   $0x0,0x30(%rdi)
ffffffff8014065f:	movq   $0x0,0x28(%rdi)
ffffffff80140667:	movq   $0x0,0x20(%rdi)
ffffffff8014066f:	movq   $0x0,0x18(%rdi)
ffffffff80140677:	movq   $0x0,0x10(%rdi)
ffffffff8014067f:	movq   $0x0,0x8(%rdi)
ffffffff80140687:	mov    %rsi,(%rdi)
ffffffff8014068a:	test   %rsi,%rsi
ffffffff8014068d:	je     ffffffff801407f7 <vos_rust_keyboard_init+0x1e7>
ffffffff80140693:	cmpq   $0x0,(%r14)
ffffffff80140697:	je     ffffffff801407f7 <vos_rust_keyboard_init+0x1e7>
ffffffff8014069d:	cmpq   $0x0,0x8(%r14)
ffffffff801406a2:	je     ffffffff801407f7 <vos_rust_keyboard_init+0x1e7>
ffffffff801406a8:	mov    %rdi,%rbx
ffffffff801406ab:	mov    $0x64,%esi
ffffffff801406b0:	mov    $0xad,%edx
ffffffff801406b5:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff801406ba:	test   %eax,%eax
ffffffff801406bc:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff801406c2:	mov    %rbx,%rdi
ffffffff801406c5:	mov    $0x64,%esi
ffffffff801406ca:	mov    $0xa7,%edx
ffffffff801406cf:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff801406d4:	test   %eax,%eax
ffffffff801406d6:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff801406dc:	mov    $0x20,%ebp
ffffffff801406e1:	data16 data16 data16 data16 data16 cs nopw 0x0(%rax,%rax,1)
ffffffff801406f0:	mov    $0x64,%edi
ffffffff801406f5:	call   *(%r14)
ffffffff801406f8:	test   $0x1,%al
ffffffff801406fa:	je     ffffffff80140708 <vos_rust_keyboard_init+0xf8>
ffffffff801406fc:	mov    $0x60,%edi
ffffffff80140701:	call   *(%r14)
ffffffff80140704:	dec    %ebp
ffffffff80140706:	jne    ffffffff801406f0 <vos_rust_keyboard_init+0xe0>
ffffffff80140708:	mov    $0x64,%edi
ffffffff8014070d:	call   *(%r14)
ffffffff80140710:	test   $0x1,%al
ffffffff80140712:	jne    ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff80140718:	mov    %rbx,%rdi
ffffffff8014071b:	mov    $0x64,%esi
ffffffff80140720:	mov    $0x20,%edx
ffffffff80140725:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff8014072a:	test   %eax,%eax
ffffffff8014072c:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff80140732:	movb   $0x0,0xf(%rsp)
ffffffff80140737:	lea    0xf(%rsp),%rsi
ffffffff8014073c:	mov    %rbx,%rdi
ffffffff8014073f:	call   ffffffff80140c20 <vos_rust_keyboard_read_keyboard>
ffffffff80140744:	test   %eax,%eax
ffffffff80140746:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff8014074c:	movzbl 0xf(%rsp),%ebp
ffffffff80140751:	mov    %rbx,%rdi
ffffffff80140754:	mov    $0x64,%esi
ffffffff80140759:	mov    $0x60,%edx
ffffffff8014075e:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140763:	test   %eax,%eax
ffffffff80140765:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff8014076b:	and    $0x8c,%bpl
ffffffff8014076f:	or     $0x60,%bpl
ffffffff80140773:	movzbl %bpl,%edx
ffffffff80140777:	mov    %rbx,%rdi
ffffffff8014077a:	mov    $0x60,%esi
ffffffff8014077f:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140784:	test   %eax,%eax
ffffffff80140786:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff80140788:	mov    %rbx,%rdi
ffffffff8014078b:	mov    $0x64,%esi
ffffffff80140790:	mov    $0xae,%edx
ffffffff80140795:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff8014079a:	test   %eax,%eax
ffffffff8014079c:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff8014079e:	mov    %rbx,%rdi
ffffffff801407a1:	mov    $0xf5,%esi
ffffffff801407a6:	call   ffffffff80140560 <vos_rust_keyboard_command>
ffffffff801407ab:	test   %eax,%eax
ffffffff801407ad:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff801407af:	mov    %rbx,%rdi
ffffffff801407b2:	mov    $0xf0,%esi
ffffffff801407b7:	call   ffffffff80140560 <vos_rust_keyboard_command>
ffffffff801407bc:	test   %eax,%eax
ffffffff801407be:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff801407c0:	mov    %rbx,%rdi
ffffffff801407c3:	mov    $0x2,%esi
ffffffff801407c8:	call   ffffffff80140560 <vos_rust_keyboard_command>
ffffffff801407cd:	test   %eax,%eax
ffffffff801407cf:	je     ffffffff801407f5 <vos_rust_keyboard_init+0x1e5>
ffffffff801407d1:	mov    %rbx,%rdi
ffffffff801407d4:	mov    $0xf4,%esi
ffffffff801407d9:	call   ffffffff80140560 <vos_rust_keyboard_command>
ffffffff801407de:	test   %eax,%eax
ffffffff801407e0:	mov    $0x0,%eax
ffffffff801407e5:	je     ffffffff801407f7 <vos_rust_keyboard_init+0x1e7>
ffffffff801407e7:	movl   $0x1,0x10(%rbx)
ffffffff801407ee:	mov    $0x1,%eax
ffffffff801407f3:	jmp    ffffffff801407f7 <vos_rust_keyboard_init+0x1e7>
ffffffff801407f5:	xor    %eax,%eax
ffffffff801407f7:	add    $0x10,%rsp
ffffffff801407fb:	pop    %rbx
ffffffff801407fc:	pop    %r14
ffffffff801407fe:	pop    %rbp
ffffffff801407ff:	ret

ffffffff801409a0 <vos_rust_keyboard_pointer_start>:
ffffffff801409a0:	push   %rbp
ffffffff801409a1:	push   %rbx
ffffffff801409a2:	push   %rax
ffffffff801409a3:	mov    %rdi,%rbx
ffffffff801409a6:	lea    -0x1(%rdi),%rax
ffffffff801409aa:	cmp    $0xffffffffffffffa7,%rax
ffffffff801409ae:	setae  %cl
ffffffff801409b1:	test   $0x7,%bl
ffffffff801409b4:	setne  %dl
ffffffff801409b7:	xor    %eax,%eax
ffffffff801409b9:	or     %cl,%dl
ffffffff801409bb:	jne    ffffffff80140aea <vos_rust_keyboard_pointer_start+0x14a>
ffffffff801409c1:	cmpl   $0x0,0x10(%rbx)
ffffffff801409c5:	je     ffffffff80140aea <vos_rust_keyboard_pointer_start+0x14a>
ffffffff801409cb:	cmpl   $0x0,0x14(%rbx)
ffffffff801409cf:	jne    ffffffff80140aea <vos_rust_keyboard_pointer_start+0x14a>
ffffffff801409d5:	mov    %rbx,%rdi
ffffffff801409d8:	call   ffffffff80140e70 <vos_rust_keyboard_suspend>
ffffffff801409dd:	test   %eax,%eax
ffffffff801409df:	je     ffffffff80140ae8 <vos_rust_keyboard_pointer_start+0x148>
ffffffff801409e5:	mov    %rbx,%rdi
ffffffff801409e8:	mov    $0x64,%esi
ffffffff801409ed:	mov    $0xad,%edx
ffffffff801409f2:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff801409f7:	test   %eax,%eax
ffffffff801409f9:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff801409ff:	mov    %rbx,%rdi
ffffffff80140a02:	mov    $0x64,%esi
ffffffff80140a07:	mov    $0xa8,%edx
ffffffff80140a0c:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140a11:	test   %eax,%eax
ffffffff80140a13:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a19:	mov    %rbx,%rdi
ffffffff80140a1c:	mov    $0xff,%esi
ffffffff80140a21:	call   ffffffff80140460 <vos_rust_keyboard_aux_command>
ffffffff80140a26:	test   %eax,%eax
ffffffff80140a28:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a2a:	movb   $0x0,0x7(%rsp)
ffffffff80140a2f:	lea    0x7(%rsp),%rsi
ffffffff80140a34:	mov    %rbx,%rdi
ffffffff80140a37:	call   ffffffff80140bb0 <vos_rust_keyboard_read_aux>
ffffffff80140a3c:	test   %eax,%eax
ffffffff80140a3e:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a40:	cmpb   $0xaa,0x7(%rsp)
ffffffff80140a45:	jne    ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a47:	movb   $0x0,0x6(%rsp)
ffffffff80140a4c:	lea    0x6(%rsp),%rsi
ffffffff80140a51:	mov    %rbx,%rdi
ffffffff80140a54:	call   ffffffff80140bb0 <vos_rust_keyboard_read_aux>
ffffffff80140a59:	test   %eax,%eax
ffffffff80140a5b:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a5d:	cmpb   $0x0,0x6(%rsp)
ffffffff80140a62:	jne    ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a64:	mov    %rbx,%rdi
ffffffff80140a67:	mov    $0xf6,%esi
ffffffff80140a6c:	call   ffffffff80140460 <vos_rust_keyboard_aux_command>
ffffffff80140a71:	test   %eax,%eax
ffffffff80140a73:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a75:	mov    %rbx,%rdi
ffffffff80140a78:	mov    $0xe6,%esi
ffffffff80140a7d:	call   ffffffff80140460 <vos_rust_keyboard_aux_command>
ffffffff80140a82:	test   %eax,%eax
ffffffff80140a84:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a86:	mov    %rbx,%rdi
ffffffff80140a89:	mov    $0xf4,%esi
ffffffff80140a8e:	call   ffffffff80140460 <vos_rust_keyboard_aux_command>
ffffffff80140a93:	test   %eax,%eax
ffffffff80140a95:	je     ffffffff80140a9b <vos_rust_keyboard_pointer_start+0xfb>
ffffffff80140a97:	xor    %ebp,%ebp
ffffffff80140a99:	jmp    ffffffff80140ab0 <vos_rust_keyboard_pointer_start+0x110>
ffffffff80140a9b:	mov    %rbx,%rdi
ffffffff80140a9e:	mov    $0x64,%esi
ffffffff80140aa3:	mov    $0xa7,%edx
ffffffff80140aa8:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140aad:	mov    $0x1,%bpl
ffffffff80140ab0:	mov    %rbx,%rdi
ffffffff80140ab3:	mov    $0x64,%esi
ffffffff80140ab8:	mov    $0xae,%edx
ffffffff80140abd:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140ac2:	test   %eax,%eax
ffffffff80140ac4:	je     ffffffff80140ad6 <vos_rust_keyboard_pointer_start+0x136>
ffffffff80140ac6:	mov    %rbx,%rdi
ffffffff80140ac9:	call   ffffffff80140c90 <vos_rust_keyboard_resume>
ffffffff80140ace:	not    %bpl
ffffffff80140ad1:	test   %al,%bpl
ffffffff80140ad4:	jne    ffffffff80140af1 <vos_rust_keyboard_pointer_start+0x151>
ffffffff80140ad6:	mov    %rbx,%rdi
ffffffff80140ad9:	mov    $0x64,%esi
ffffffff80140ade:	mov    $0xa7,%edx
ffffffff80140ae3:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140ae8:	xor    %eax,%eax
ffffffff80140aea:	add    $0x8,%rsp
ffffffff80140aee:	pop    %rbx
ffffffff80140aef:	pop    %rbp
ffffffff80140af0:	ret
ffffffff80140af1:	movb   $0x0,0x20(%rbx)
ffffffff80140af5:	movl   $0x0,0x1c(%rbx)
ffffffff80140afc:	movq   $0x0,0x28(%rbx)
ffffffff80140b04:	movl   $0x0,0x30(%rbx)
ffffffff80140b0b:	movq   $0x1,0x14(%rbx)
ffffffff80140b13:	mov    $0x1,%eax
ffffffff80140b18:	jmp    ffffffff80140aea <vos_rust_keyboard_pointer_start+0x14a>
ffffffff80140b1a:	int3
ffffffff80140b1b:	int3
ffffffff80140b1c:	int3
ffffffff80140b1d:	int3
ffffffff80140b1e:	int3
ffffffff80140b1f:	int3

ffffffff80140b20 <vos_rust_keyboard_pointer_stop>:
ffffffff80140b20:	lea    -0x1(%rdi),%rax
ffffffff80140b24:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140b28:	setae  %al
ffffffff80140b2b:	test   $0x7,%dil
ffffffff80140b2f:	setne  %cl
ffffffff80140b32:	or     %al,%cl
ffffffff80140b34:	jne    ffffffff80140ba3 <vos_rust_keyboard_pointer_stop+0x83>
ffffffff80140b36:	cmpl   $0x0,0x14(%rdi)
ffffffff80140b3a:	je     ffffffff80140ba3 <vos_rust_keyboard_pointer_stop+0x83>
ffffffff80140b3c:	push   %rbx
ffffffff80140b3d:	mov    %rdi,%rbx
ffffffff80140b40:	mov    $0x64,%esi
ffffffff80140b45:	mov    $0xad,%edx
ffffffff80140b4a:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140b4f:	mov    %rbx,%rdi
ffffffff80140b52:	mov    $0xf5,%esi
ffffffff80140b57:	call   ffffffff80140460 <vos_rust_keyboard_aux_command>
ffffffff80140b5c:	mov    %rbx,%rdi
ffffffff80140b5f:	mov    $0x64,%esi
ffffffff80140b64:	mov    $0xa7,%edx
ffffffff80140b69:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140b6e:	mov    %rbx,%rdi
ffffffff80140b71:	mov    $0x64,%esi
ffffffff80140b76:	mov    $0xae,%edx
ffffffff80140b7b:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140b80:	movq   $0x0,0x14(%rbx)
ffffffff80140b88:	movl   $0x0,0x1c(%rbx)
ffffffff80140b8f:	movb   $0x0,0x20(%rbx)
ffffffff80140b93:	movl   $0x0,0x30(%rbx)
ffffffff80140b9a:	movq   $0x0,0x28(%rbx)
ffffffff80140ba2:	pop    %rbx
ffffffff80140ba3:	ret
ffffffff80140ba4:	int3
ffffffff80140ba5:	int3
ffffffff80140ba6:	int3
ffffffff80140ba7:	int3
ffffffff80140ba8:	int3
ffffffff80140ba9:	int3
ffffffff80140baa:	int3
ffffffff80140bab:	int3
ffffffff80140bac:	int3
ffffffff80140bad:	int3
ffffffff80140bae:	int3
ffffffff80140baf:	int3

ffffffff80140c90 <vos_rust_keyboard_resume>:
ffffffff80140c90:	lea    -0x1(%rdi),%rax
ffffffff80140c94:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140c98:	setae  %cl
ffffffff80140c9b:	test   $0x7,%dil
ffffffff80140c9f:	setne  %dl
ffffffff80140ca2:	xor    %eax,%eax
ffffffff80140ca4:	or     %cl,%dl
ffffffff80140ca6:	jne    ffffffff80140cb4 <vos_rust_keyboard_resume+0x24>
ffffffff80140ca8:	cmpq   $0x0,(%rdi)
ffffffff80140cac:	je     ffffffff80140cb4 <vos_rust_keyboard_resume+0x24>
ffffffff80140cae:	cmpl   $0x0,0x10(%rdi)
ffffffff80140cb2:	je     ffffffff80140cb5 <vos_rust_keyboard_resume+0x25>
ffffffff80140cb4:	ret
ffffffff80140cb5:	push   %rbx
ffffffff80140cb6:	movl   $0x0,0xb(%rdi)
ffffffff80140cbd:	movl   $0x0,0x8(%rdi)
ffffffff80140cc4:	mov    %rdi,%rbx
ffffffff80140cc7:	mov    $0xf4,%esi
ffffffff80140ccc:	call   ffffffff80140560 <vos_rust_keyboard_command>
ffffffff80140cd1:	mov    %eax,%ecx
ffffffff80140cd3:	xor    %eax,%eax
ffffffff80140cd5:	test   %ecx,%ecx
ffffffff80140cd7:	je     ffffffff80140ce5 <vos_rust_keyboard_resume+0x55>
ffffffff80140cd9:	movl   $0x1,0x10(%rbx)
ffffffff80140ce0:	mov    $0x1,%eax
ffffffff80140ce5:	pop    %rbx
ffffffff80140ce6:	ret
ffffffff80140ce7:	int3
ffffffff80140ce8:	int3
ffffffff80140ce9:	int3
ffffffff80140cea:	int3
ffffffff80140ceb:	int3
ffffffff80140cec:	int3
ffffffff80140ced:	int3
ffffffff80140cee:	int3
ffffffff80140cef:	int3

ffffffff80140e70 <vos_rust_keyboard_suspend>:
ffffffff80140e70:	push   %rbp
ffffffff80140e71:	push   %r14
ffffffff80140e73:	push   %rbx
ffffffff80140e74:	sub    $0x10,%rsp
ffffffff80140e78:	mov    %rdi,%rbx
ffffffff80140e7b:	lea    -0x1(%rdi),%rax
ffffffff80140e7f:	cmp    $0xffffffffffffffa7,%rax
ffffffff80140e83:	setae  %al
ffffffff80140e86:	test   $0x7,%bl
ffffffff80140e89:	setne  %cl
ffffffff80140e8c:	or     %al,%cl
ffffffff80140e8e:	jne    ffffffff80140f1c <vos_rust_keyboard_suspend+0xac>
ffffffff80140e94:	cmpq   $0x0,(%rbx)
ffffffff80140e98:	je     ffffffff80140f1c <vos_rust_keyboard_suspend+0xac>
ffffffff80140e9e:	mov    $0x1,%eax
ffffffff80140ea3:	cmpl   $0x0,0x10(%rbx)
ffffffff80140ea7:	je     ffffffff80140f1e <vos_rust_keyboard_suspend+0xae>
ffffffff80140ea9:	movl   $0x0,0x10(%rbx)
ffffffff80140eb0:	mov    %rbx,%rdi
ffffffff80140eb3:	mov    $0x60,%esi
ffffffff80140eb8:	mov    $0xf5,%edx
ffffffff80140ebd:	call   ffffffff80140f40 <vos_rust_keyboard_write_port>
ffffffff80140ec2:	test   %eax,%eax
ffffffff80140ec4:	je     ffffffff80140f1c <vos_rust_keyboard_suspend+0xac>
ffffffff80140ec6:	mov    $0x20,%ebp
ffffffff80140ecb:	lea    0xf(%rsp),%r14
ffffffff80140ed0:	movb   $0x0,0xf(%rsp)
ffffffff80140ed5:	mov    %rbx,%rdi
ffffffff80140ed8:	mov    %r14,%rsi
ffffffff80140edb:	call   ffffffff80140c20 <vos_rust_keyboard_read_keyboard>
ffffffff80140ee0:	test   %eax,%eax
ffffffff80140ee2:	je     ffffffff80140f1c <vos_rust_keyboard_suspend+0xac>
ffffffff80140ee4:	cmpb   $0xfa,0xf(%rsp)
ffffffff80140ee9:	je     ffffffff80140ef1 <vos_rust_keyboard_suspend+0x81>
ffffffff80140eeb:	dec    %ebp
ffffffff80140eed:	jne    ffffffff80140ed0 <vos_rust_keyboard_suspend+0x60>
ffffffff80140eef:	jmp    ffffffff80140f1c <vos_rust_keyboard_suspend+0xac>
ffffffff80140ef1:	mov    $0x20,%ebp
ffffffff80140ef6:	cs nopw 0x0(%rax,%rax,1)
ffffffff80140f00:	mov    (%rbx),%rax
ffffffff80140f03:	mov    $0x64,%edi
ffffffff80140f08:	call   *(%rax)
ffffffff80140f0a:	test   $0x1,%al
ffffffff80140f0c:	je     ffffffff80140f27 <vos_rust_keyboard_suspend+0xb7>
ffffffff80140f0e:	mov    (%rbx),%rax
ffffffff80140f11:	mov    $0x60,%edi
ffffffff80140f16:	call   *(%rax)
ffffffff80140f18:	dec    %ebp
ffffffff80140f1a:	jne    ffffffff80140f00 <vos_rust_keyboard_suspend+0x90>
ffffffff80140f1c:	xor    %eax,%eax
ffffffff80140f1e:	add    $0x10,%rsp
ffffffff80140f22:	pop    %rbx
ffffffff80140f23:	pop    %r14
ffffffff80140f25:	pop    %rbp
ffffffff80140f26:	ret
ffffffff80140f27:	movl   $0x0,0xb(%rbx)
ffffffff80140f2e:	movl   $0x0,0x8(%rbx)
ffffffff80140f35:	mov    $0x1,%eax
ffffffff80140f3a:	jmp    ffffffff80140f1e <vos_rust_keyboard_suspend+0xae>
ffffffff80140f3c:	int3
ffffffff80140f3d:	int3
ffffffff80140f3e:	int3
ffffffff80140f3f:	int3
