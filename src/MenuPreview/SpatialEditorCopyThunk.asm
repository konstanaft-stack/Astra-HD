EXTERN CvrEditorCopyActive:DWORD
EXTERN CvrEditorCopyOriginal:QWORD
EXTERN CvrEditorCopyBefore:PROC
.code
CvrEditorCopyDetour PROC
    cmp r8,90h
    jne passThrough
    cmp DWORD PTR [CvrEditorCopyActive],0
    je passThrough
    sub rsp,0E8h
    ; Match Luke's relay: edit the source, then enter the native copy with its
    ; original arguments. Nothing runs after the native return.
    mov [rsp+20h],rax
    mov [rsp+28h],rcx
    mov [rsp+30h],rdx
    mov [rsp+38h],r8
    mov [rsp+40h],r9
    mov [rsp+48h],r10
    mov [rsp+50h],r11
    pushfq
    pop r11
    mov [rsp+0D0h],r11
    movdqu [rsp+60h],xmm0
    movdqu [rsp+70h],xmm1
    movdqu [rsp+80h],xmm2
    movdqu [rsp+90h],xmm3
    movdqu [rsp+0A0h],xmm4
    movdqu [rsp+0B0h],xmm5
    mov rcx,[rsp+30h]
    mov rdx,[rsp+38h]
    call CvrEditorCopyBefore
    movdqu xmm0,[rsp+60h]
    movdqu xmm1,[rsp+70h]
    movdqu xmm2,[rsp+80h]
    movdqu xmm3,[rsp+90h]
    movdqu xmm4,[rsp+0A0h]
    movdqu xmm5,[rsp+0B0h]
    mov rax,[rsp+20h]
    mov rcx,[rsp+28h]
    mov rdx,[rsp+30h]
    mov r8,[rsp+38h]
    mov r9,[rsp+40h]
    mov r10,[rsp+48h]
    push QWORD PTR [rsp+0D0h]
    popfq
    mov r11,[rsp+50h]
    lea rsp,[rsp+0E8h]
    jmp QWORD PTR [CvrEditorCopyOriginal]
passThrough:
    jmp QWORD PTR [CvrEditorCopyOriginal]
CvrEditorCopyDetour ENDP
END
