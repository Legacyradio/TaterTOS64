; Switch to a new stack and call a target function (x86_64 SysV)
; Args:
;   rdi = new_rsp
;   rsi = target function pointer
;   rdx = arg0 for target (passed in rdi)

global stack_switch_and_call

section .text
stack_switch_and_call:
    ; Save arg0 for target before corrupting RDX with debugcon port
    mov r8, rdx
    ; Debug: mark stack switch entry
    mov dx, 0xE9
    mov al, 'S'
    out dx, al
    mov rsp, rdi
    and rsp, -16
    ; Debug: mark just before call
    mov al, 'C'
    out dx, al
    mov rdi, r8
    call rsi
.return:
    ; Debug: mark unexpected return
    mov dx, 0xE9
    mov al, 'R'
    out dx, al
.hang:
    hlt
    jmp .hang
