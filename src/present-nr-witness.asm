option casemap:none
EXTERN PresentAdapterNrEnter:PROC
EXTERN PresentAdapterSrEnter:PROC
EXTERN g_present_nr_original:QWORD
EXTERN g_present_sr_original:QWORD
.code
PresentAdapterNrHook PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov [rsp+30h], rcx
    mov [rsp+38h], rdx
    mov [rsp+40h], r8
    mov [rsp+48h], r9
    call PresentAdapterNrEnter
    mov rcx, [rsp+30h]
    mov rdx, [rsp+38h]
    mov r8, [rsp+40h]
    mov r9, [rsp+48h]
    add rsp, 68h
    jmp QWORD PTR [g_present_nr_original]
PresentAdapterNrHook ENDP
PresentAdapterSrHook PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov [rsp+30h], rcx
    mov [rsp+38h], rdx
    mov [rsp+40h], r8
    mov [rsp+48h], r9
    call PresentAdapterSrEnter
    test eax, eax
    jnz sr_handled
    mov rcx, [rsp+30h]
    mov rdx, [rsp+38h]
    mov r8, [rsp+40h]
    mov r9, [rsp+48h]
    add rsp, 68h
    jmp QWORD PTR [g_present_sr_original]
sr_handled:
    add rsp, 68h
    ret
PresentAdapterSrHook ENDP
END
