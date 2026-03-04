; ISR stubs for TaterTOS64v3
; NASM syntax, 64-bit

BITS 64

section .text

global isr_stub_table
extern common_isr

%macro ISR_NOERR 1
    global isr_stub_%1
    isr_stub_%1:
        push qword 0
        push qword %1
        jmp common_isr
%endmacro

%macro ISR_ERR 1
    global isr_stub_%1
    isr_stub_%1:
        push qword %1
        jmp common_isr
%endmacro

; Exceptions with error code: 8,10,11,12,13,14,17
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

section .rodata
align 8
isr_stub_table:
%assign j 0
%rep 256
    dq isr_stub_%+j
%assign j j+1
%endrep
