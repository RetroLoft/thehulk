; TheHulk: the virtual floppy drive (docs/FLOPPY.md). After the driver of SideTNFS.
;
; Works on the level of BIOS and XBIOS (getbpb/rwabs/mediach vectors and Floprd/Flopwr/
; Flopfmt/Flopver), not on the WD1772: a program that talks to the floppy controller itself
; does not see the image. Drive A: or B:, read-only. The boot sector comes through rwabs, so
; TOS boots from the image by itself.
;
; Installed apart from GEMDRIVE: INSTALL_FLOPPY without INSTALL_GEMDRIVE needs no GEMDRIVE
; at all. Then the wait code goes on the stack (SHARED_VARIABLE_BUFFER_TYPE): a game may
; use all the memory and runs its boot sector from _dskbufp, so neither is ours to write.
;
; What these hooks may change: the BIOS vectors are also called directly (disk drivers), as
; C functions: only d0-d2/a0-a2. XBIOS comes straight from the trap: nothing but d0.

; Offsets in ROM3, the same as esp32/main/gemdrv_layout.h
GEMDRVEMUL_FLOPPY_SESSION              equ (ROM3_START_ADDR+38864)
GEMDRVEMUL_FLOPPY_SESSION_STATUS       equ (GEMDRVEMUL_FLOPPY_SESSION+0)    ; long, 0 = an image is ready
GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE equ (GEMDRVEMUL_FLOPPY_SESSION+8)
GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY   equ (GEMDRVEMUL_FLOPPY_SESSION+10)
GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER equ (GEMDRVEMUL_FLOPPY_SESSION+12)   ; word, 0 = A:, 1 = B:
GEMDRVEMUL_FLOPPY_SESSION_SIDES        equ (GEMDRVEMUL_FLOPPY_SESSION+528)
GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK equ (GEMDRVEMUL_FLOPPY_SESSION+530)
GEMDRVEMUL_FLOPPY_SESSION_SECTOR_DATA  equ (GEMDRVEMUL_FLOPPY_SESSION+544)  ; 512 bytes
GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB     equ (GEMDRVEMUL_FLOPPY_SESSION+1056)
GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW      equ (GEMDRVEMUL_FLOPPY_SESSION+1060)
GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH equ (GEMDRVEMUL_FLOPPY_SESSION+1064)
GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR equ (GEMDRVEMUL_FLOPPY_SESSION+1068)
GEMDRVEMUL_FLOPPY_SESSION_BPB          equ (GEMDRVEMUL_FLOPPY_SESSION+1072) ; 9 words
GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED equ (GEMDRVEMUL_FLOPPY_SESSION+1090) ; word, non-zero: another image

CMD_FLOPPY_READ_SECTOR       equ ($2A + APP_GEMDRVEMUL)  ; d3 = sector number
CMD_FLOPPY_SAVE_VECTORS      equ ($2C + APP_GEMDRVEMUL)  ; d3-d5 = old getbpb, rwabs, mediach
CMD_FLOPPY_SAVE_XBIOS_VECTOR equ ($2D + APP_GEMDRVEMUL)  ; d3 = old XBIOS vector
CMD_FLOPPY_MEDIA_CHANGE_ACK  equ ($2E + APP_GEMDRVEMUL)  ; clears MEDIA_CHANGED

Flopver equ 19
FLOPPY_SECTOR_SIZE equ 512

; The Mega STE: its cache would keep old contents of ROM3. Around everything that reads
; ROM3 for an answer. /1: a data register for the old value, not changed until the restore.
disable_floppy_cache    macro
                    cmp.l #COOKIE_JAR_MEGASTE, (GEMDRVEMUL_SHARED_VARIABLES + SHARED_VARIABLE_HARDWARE_TYPE)
                    bne.s .\@disable_floppy_cache_continue
                    move.b MEGASTE_SPEED_CACHE_REG.w, \1
                    and.b #%00000001,MEGASTE_SPEED_CACHE_REG.w
.\@disable_floppy_cache_continue:
                    endm

restore_floppy_cache    macro
                    cmp.l #COOKIE_JAR_MEGASTE, (GEMDRVEMUL_SHARED_VARIABLES + SHARED_VARIABLE_HARDWARE_TYPE)
                    bne.s .\@restore_floppy_cache_continue
                    move.b \1, MEGASTE_SPEED_CACHE_REG.w
.\@restore_floppy_cache_continue:
                    endm

; From rom_function, after the TOS version and the machine type are known. The ESP32 answers
; SAVE_VECTORS once the image is ready (it waits for the card at power-on).
install_floppy_hooks:
    move.l  getbpb.vector.w,d3
    move.l  rwabs.vector.w,d4
    move.l  mediach.vector.w,d5
    send_sync CMD_FLOPPY_SAVE_VECTORS,12

    move.l  XBIOS_TRAP_ADDR.w,d3
    send_sync CMD_FLOPPY_SAVE_XBIOS_VECTOR,4

    move.l  #new_getbpb_routine,getbpb.vector.w
    move.l  #new_rwabs_routine,rwabs.vector.w
    move.l  #new_mediach_routine,mediach.vector.w
    move.l  #new_floppy_xbios_trap,XBIOS_TRAP_ADDR.w

    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne.s   .install_done                ; no image: the Atari keeps what it has
    tst.w   _nflops.w
    bne.s   .install_has_drive
    move.w  #1,_nflops.w                 ; no real drive: TOS would not boot from a floppy
.install_has_drive:
    move.w  GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,d0
    move.l  _drvbits.w,d1
    bset    d0,d1
    move.l  d1,_drvbits.w
.install_done:
    rts

; getbpb: drive at 4(sp), d0 = the address of the 9 words of the BPB
new_getbpb_routine:
    disable_floppy_cache d1
    move.w  GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,d0
    cmp.w   4(sp),d0
    bne.s   .bpb_chain
    tst.w   GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY
    beq.s   .bpb_chain
    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne.s   .bpb_chain
    move.l  #GEMDRVEMUL_FLOPPY_SESSION_BPB,d0
    restore_floppy_cache d1
    rts
.bpb_chain:
    move.l  GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB,-(sp)
    restore_floppy_cache d1
    rts

; mediach: drive at 4(sp), d0 = 0 unchanged, 2 changed. "Changed" once per new image.
new_mediach_routine:
    disable_floppy_cache d1
    move.w  GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,d0
    cmp.w   4(sp),d0
    bne.s   .mc_chain
    tst.w   GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY
    beq.s   .mc_chain
    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne.s   .mc_chain
    tst.w   GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED
    beq.s   .mc_unchanged
    movem.l d1-d7/a3-a6,-(sp)
    send_sync CMD_FLOPPY_MEDIA_CHANGE_ACK,0
    movem.l (sp)+,d1-d7/a3-a6
    moveq   #2,d0
    restore_floppy_cache d1
    rts
.mc_unchanged:
    moveq   #0,d0
    restore_floppy_cache d1
    rts
.mc_chain:
    move.l  GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH,-(sp)
    restore_floppy_cache d1
    rts

; rwabs: rwflag 4(sp).w, buffer 6(sp).l, count 10(sp).w, sector 12(sp).w, drive 14(sp).w
new_rwabs_routine:
    disable_floppy_cache d1
    move.w  GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,d0
    cmp.w   14(sp),d0
    bne     .rw_chain
    tst.w   GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY
    beq     .rw_chain
    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne     .rw_chain
    btst    #0,5(sp)                     ; rwflag bit 0: write
    beq.s   .rw_read
    moveq   #EWRPRO,d0
    restore_floppy_cache d1
    rts
.rw_read:
    movem.l d1-d7/a2-a6,-(sp)            ; 48 bytes, d1 is the old cache value
    move.l  6+48(sp),a0
    move.w  10+48(sp),d6
    moveq   #0,d5
    move.w  12+48(sp),d5
    bsr     floppy_read_sectors
    movem.l (sp)+,d1-d7/a2-a6
    restore_floppy_cache d1
    rts
.rw_chain:
    move.l  GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW,-(sp)
    restore_floppy_cache d1
    rts

; a0 = buffer, d5.w = first sector, d6.w = number of sectors. Returns d0 = 0 or an error.
; Changes d1-d7/a0-a3.
floppy_read_sectors:
    moveq   #0,d0
    subq.w  #1,d6
    bcs     .read_done
.read_loop:
    movem.l d5-d6/a0,-(sp)
    moveq   #0,d3
    move.w  d5,d3
    send_sync CMD_FLOPPY_READ_SECTOR,4
    movem.l (sp)+,d5-d6/a0
    tst.w   d0
    bne.s   .read_timeout
    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne.s   .read_not_found
    moveq   #FREAD_RETRIES,d3
.read_copy:
    move.l  a0,a2                        ; the start of this sector in RAM
    lea     GEMDRVEMUL_FLOPPY_SESSION_SECTOR_DATA,a1
    move.l  a0,d7
    btst    #0,d7
    bne.s   .read_odd
    moveq   #(FLOPPY_SECTOR_SIZE/4)-1,d7
.read_long:
    move.l  (a1)+,(a0)+
    dbf     d7,.read_long
    bra.s   .read_check
.read_odd:
    move.w  #FLOPPY_SECTOR_SIZE-1,d7
.read_byte:
    move.b  (a1)+,(a0)+
    dbf     d7,.read_byte
.read_check:
    ; The Falcon can read a wrong word from the cartridge now and then, see .fread_copy_exit
    tst.l   (GEMDRVEMUL_SHARED_VARIABLES + (SHARED_VARIABLE_READ_CHECK * 4))
    beq.s   .read_good
    move.l  a2,a1
    moveq   #0,d1
    move.w  #(FLOPPY_SECTOR_SIZE/2)-1,d7
.read_sum:
    move.b  (a1)+,d2
    lsl.w   #8,d2
    move.b  (a1)+,d2
    add.w   d2,d1
    dbf     d7,.read_sum
    move.l  (GEMDRVEMUL_SHARED_VARIABLES + (SHARED_VARIABLE_READ_CHK * 4)),d2
    cmp.w   d2,d1
    beq.s   .read_good
    move.l  a2,a0
    dbf     d3,.read_copy
    moveq   #EREADF,d0
    rts
.read_good:
    addq.w  #1,d5
    dbf     d6,.read_loop
    moveq   #0,d0
.read_done:
    rts
.read_timeout:
    moveq   #EREADF,d0
    rts
.read_not_found:
    moveq   #ESECNF,d0
    rts

; XBIOS: a0 ends up 6 bytes below the function number, as in the GEMDOS hook
new_floppy_xbios_trap:
    movem.l d0/a0,-(sp)
    btst    #5,8(sp)
    bne.s   .fx_super
    move.l  usp,a0
    subq.l  #6,a0
    bra.s   .fx_frame
.fx_super:
    lea     8(sp),a0
    tst.w   _longframe.w
    beq.s   .fx_frame
    addq.w  #2,a0
.fx_frame:
    move.w  6(a0),d0
    cmp.w   #Floprd,d0
    beq.s   .fx_ours
    cmp.w   #Flopwr,d0
    beq.s   .fx_ours
    cmp.w   #Flopfmt,d0
    beq.s   .fx_ours
    cmp.w   #Flopver,d0
    beq.s   .fx_ours
.fx_chain:
    movem.l (sp)+,d0/a0
    move.l  GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR,-(sp)
    rts

; Floprd(buffer 8, dummy 12, drive 16, sector 18, track 20, side 22, count 24), from a0
.fx_ours:
    movem.l d1-d7/a1-a6,-(sp)
    disable_floppy_cache d4
    move.w  GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,d0
    cmp.w   16(a0),d0
    bne     .fx_not_ours
    tst.w   GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY
    beq     .fx_not_ours
    tst.l   GEMDRVEMUL_FLOPPY_SESSION_STATUS
    bne     .fx_not_ours
    move.w  6(a0),d0
    cmp.w   #Floprd,d0
    beq.s   .fx_read
    cmp.w   #Flopver,d0
    beq.s   .fx_ok
    moveq   #EWRPRO,d0                   ; Flopwr, Flopfmt: as a write-protected disk
    bra.s   .fx_return
.fx_ok:
    moveq   #0,d0
    bra.s   .fx_return
.fx_read:
    move.w  22(a0),d6                    ; the side must exist
    cmp.w   GEMDRVEMUL_FLOPPY_SESSION_SIDES,d6
    bhs.s   .fx_not_found
    move.w  18(a0),d7                    ; the sector: 1 to sectors per track
    beq.s   .fx_not_found
    cmp.w   GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK,d7
    bhi.s   .fx_not_found
    moveq   #0,d5
    move.w  20(a0),d5                    ; track * sides * sectors per track
    move.w  GEMDRVEMUL_FLOPPY_SESSION_SIDES,d3
    mulu    GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK,d3
    mulu    d3,d5
    mulu    GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK,d6
    add.l   d6,d5                        ; + side * sectors per track
    subq.w  #1,d7
    add.w   d7,d5                        ; + sector - 1
    move.w  24(a0),d6
    move.l  8(a0),a0
    move.w  d4,-(sp)                     ; the old cache value
    bsr     floppy_read_sectors
    move.w  (sp)+,d4
    bra.s   .fx_return
.fx_not_found:
    moveq   #ESECNF,d0
.fx_return:
    restore_floppy_cache d4
    movem.l (sp)+,d1-d7/a1-a6
    addq.l  #4,sp                        ; the d0 of the caller: ours is the answer
    move.l  (sp)+,a0
    rte
.fx_not_ours:
    restore_floppy_cache d4
    movem.l (sp)+,d1-d7/a1-a6
    bra     .fx_chain
