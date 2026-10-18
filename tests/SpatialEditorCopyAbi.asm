EXTERN CvrEditorCopyDetour:PROC
EXTERN SpatialCopyOriginalSawExpanded:DWORD
.code
SpatialCopyOriginalFixture PROC
    mov DWORD PTR [SpatialCopyOriginalSawExpanded],0
    cmp r8,90h
    jne nativeCopy
    cmp DWORD PTR [rdx+4],0
    jne nativeCopy
    mov DWORD PTR [SpatialCopyOriginalSawExpanded],1
nativeCopy:
    mov rax,rcx
    push rsi
    push rdi
    mov rdi,rcx
    mov rsi,rdx
    mov rcx,r8
    rep movsb
    pop rdi
    pop rsi
    mov r9,1111h
    mov r10,2222h
    mov r11,3333h
    movq xmm0,r9
    stc
    ret
SpatialCopyOriginalFixture ENDP
RunSpatialCopyAbi PROC
    sub rsp,28h
    mov [rsp+20h],rcx
    call CvrEditorCopyDetour
    jnc failed
    cmp rax,[rsp+20h]
    jne failed
    cmp r9,1111h
    jne failed
    cmp r10,2222h
    jne failed
    cmp r11,3333h
    jne failed
    movq rax,xmm0
    cmp rax,1111h
    jne failed
    mov eax,1
    add rsp,28h
    ret
failed:
    xor eax,eax
    add rsp,28h
    ret
RunSpatialCopyAbi ENDP
END
