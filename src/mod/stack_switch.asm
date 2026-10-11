; CallOnStack(fn, arg, stackTop): run fn(arg) on another stack, then come back.
; rbp is the frame register in the unwind info, so exceptions can unwind through.
.code
CallOnStack PROC FRAME
    push rbp
    .pushreg rbp
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    mov rsp, r8
    and rsp, -16
    sub rsp, 32
    mov rax, rcx
    mov rcx, rdx
    call rax
    mov rsp, rbp
    pop rbp
    ret
CallOnStack ENDP
END
