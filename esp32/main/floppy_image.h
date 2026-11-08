// What a disk image is, without the card or the ESP32 (also compiled on a PC to test it)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t sides, sectors_per_track, tracks;
    uint32_t sectors;
} floppy_geometry_t;

// An .ST: the boot sector when that agrees with the size, else the size alone (a game disk
// with its own loader often has rubbish there). head: the first 32 bytes or more.
bool floppy_st_geometry(const uint8_t *head, size_t head_len, uint32_t size, floppy_geometry_t *g);
// An .MSA: the geometry of its header (10 bytes) when it can be right
bool floppy_msa_header(const uint8_t *head, size_t head_len, uint32_t size, floppy_geometry_t *g);
// Unpacks an .MSA into out, g->sectors * 512 bytes; false when the file is damaged
bool floppy_msa_unpack(const uint8_t *file, uint32_t size, const floppy_geometry_t *g, uint8_t *out);
// The 9 words TOS wants from Getbpb, from the boot sector or else for a normal disk
void floppy_make_bpb(const uint8_t *sector0, uint32_t sectors, uint16_t bpb[9]);
