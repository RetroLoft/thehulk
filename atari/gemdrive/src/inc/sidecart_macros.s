; SidecarTridge Multi-device macros for the shared SidecarTridge library

; Macros

; Send a synchronous command to the Multi-device passing arguments in the Dx registers
; /1 : The command code
; /2 : The payload size (even number always)
send_sync           macro
                    moveq.l #\2, d1                      ; Set the payload size of the command
                    move.w #\1,d0                        ; Command code
                    bsr send_sync_command_to_sidecart    ; Send the command to the Multi-device
                    endm    

; Send a synchronous write command to the Multi-device passing arguments in the D3-D5 registers
; A4 address of the buffer to send
; /1 : The command code
; /2 : The buffer size to send in bytes (will be rounded to the next word)
send_write_sync     macro
                    move.w #\1,d0                           ; Command code
                    moveq.l #12, d1                         ; Set the payload size of the command (d3.l, d4.l and d5.l)
                    move.l #\2,d6                           ; Number of bytes to send
                    bsr send_sync_write_command_to_sidecart ; Send the command to the Multi-device
                    endm    

; Send one 16-bit word to TheHulk as two reads of ROM3 on even addresses, high byte
; first: 0xFB0000 + 2 * byte. A0 is never used, because the Falcon drives /UDS and
; /LDS low on every read of the cartridge port, also for a byte at an odd address.
; /1 : data register with the word (not d0)
; d0.l high word must hold the ROM3 address high word ($00FB); d0.w and a1 are
; scratch
send_word           macro
                    move.w \1, d0
                    lsr.w #7, d0                            ; (word >> 8) * 2
                    and.w #$1fe, d0
                    move.l d0, a1
                    move.b (a1), d0                         ; High byte
                    move.w \1, d0
                    add.w d0, d0                            ; (word & $ff) * 2
                    and.w #$1fe, d0
                    move.l d0, a1
                    move.b (a1), d0                         ; Low byte
                    endm

; Wait for second (aprox 50 VBlanks)
wait_sec                macro
                        move.l d7, -(sp)                    ; Save the number counter reg
                        move.w #50, d7                      ; Loop to wait a second (aprox 50 VBlanks)
.\@wait_sec_loop:
                        move.w 	#37,-(sp)                   ; Wait for the VBlank. Add a delay
                        trap 	#14
                        addq.l 	#2,sp
                        dbf d7, .\@wait_sec_loop
                        move.l (sp)+, d7                    ; Restore the number counter reg
                        endm

; Wait for second (aprox 50 VBlanks) and cancel if a key is pressed
wait_sec_or_key_press   macro
                        move.l d7, -(sp)                    ; Save the number counter reg
                        move.w #50, d7                      ; Loop to wait a second (aprox 50 VBlanks)
.\@wait_sec_key_loop:
                        move.w  #$ff,-(sp)
                        move.w  #Crawio,-(sp)               ; Check if a key is pressed
                        trap    #1
                        addq.l  #4,sp
                        tst.l   d0                          ; Returns 0 in the register if no key is pressed
                        bne.s   .\@wait_sec_key_loop_exit   ; If no key is pressed, continue waiting
                        move.w 	#37,-(sp)                   ; Wait for the VBlank. Add a delay
                        trap 	#14
                        addq.l 	#2,sp
                        dbf d7, .\@wait_sec_key_loop
                        clr.l   d0                          ; Clear the register because no key was pressed
.\@wait_sec_key_loop_exit:
                        move.l (sp)+, d7                    ; Restore the number counter reg
                        endm
