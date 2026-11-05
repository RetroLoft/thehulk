// File names between the Atari character set (GEMDOS) and UTF-8 (the TNFS server)
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Always succeeds: every Atari character has a Unicode one
void atari_to_utf8(const char *in, char *out, size_t size);
// False when a character has no Atari counterpart, or out is too small
bool utf8_to_atari(const char *in, char *out, size_t size);
