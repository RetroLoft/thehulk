// Disk image formats, see floppy_image.h
#include <string.h>
#include "floppy_image.h"

#define SECTOR 512u

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static bool spt_ok(uint32_t spt)
{
    return (spt >= 9 && spt <= 11) || spt == 18;
}

bool floppy_st_geometry(const uint8_t *head, size_t head_len, uint32_t size, floppy_geometry_t *g)
{
    uint32_t total = size / SECTOR;     // a few bytes too many or too few occur in dumps
    if (total < 300)
        return false;
    uint32_t bpb_spt = 0, bpb_sides = 0;
    if (head_len >= 28 && le16(head + 11) == SECTOR) {
        bpb_spt = le16(head + 24);
        bpb_sides = le16(head + 26);
        if (bpb_spt < 8 || bpb_spt > 36 || (bpb_sides != 1 && bpb_sides != 2))
            bpb_spt = 0;
    }
    if (bpb_spt && le16(head + 19) == total && total % (bpb_spt * bpb_sides) == 0) {
        *g = (floppy_geometry_t){ bpb_sides, bpb_spt, total / (bpb_spt * bpb_sides), total };
        return g->tracks >= 38 && g->tracks <= 90;
    }
    static const uint8_t tracks[] = { 80, 81, 82, 83, 84, 85, 86, 40, 41, 42 };
    static const uint8_t spts[] = { 9, 10, 11, 18 };
    for (size_t t = 0; t < sizeof(tracks); t++)
        for (uint32_t sides = 2; sides >= 1; sides--)
            for (size_t s = 0; s < sizeof(spts); s++)
                if (tracks[t] * sides * spts[s] == total) {
                    *g = (floppy_geometry_t){ sides, spts[s], tracks[t], total };
                    return true;
                }
    if (bpb_spt) {                      // no standard size: what the boot sector says
        *g = (floppy_geometry_t){ bpb_sides, bpb_spt, (total + bpb_spt * bpb_sides - 1) / (bpb_spt * bpb_sides), total };
        return g->tracks >= 38 && g->tracks <= 90;
    }
    return false;
}

bool floppy_msa_header(const uint8_t *head, size_t head_len, uint32_t size, floppy_geometry_t *g)
{
    if (head_len < 10 || size < 10 || be16(head) != 0x0E0F)
        return false;
    uint32_t spt = be16(head + 2), sides = be16(head + 4) + 1, first = be16(head + 6), last = be16(head + 8);
    if (!spt_ok(spt) || sides > 2 || first > last || last >= 86)
        return false;
    *g = (floppy_geometry_t){ sides, spt, last + 1, (last + 1) * sides * spt };
    return true;
}

bool floppy_msa_unpack(const uint8_t *file, uint32_t size, const floppy_geometry_t *g, uint8_t *out)
{
    uint32_t track_size = g->sectors_per_track * SECTOR;
    uint32_t first = be16(file + 6), last = be16(file + 8);
    uint32_t pos = 10;
    memset(out, 0, g->sectors * SECTOR);        // tracks before the first one: empty
    for (uint32_t t = first; t <= last; t++) {
        for (uint32_t side = 0; side < g->sides; side++) {
            uint8_t *dst = out + (t * g->sides + side) * track_size;
            if (pos + 2 > size)
                return false;
            uint32_t len = be16(file + pos);
            pos += 2;
            if (pos + len > size)
                return false;
            if (len == track_size) {            // stored as it is
                memcpy(dst, file + pos, len);
                pos += len;
                continue;
            }
            uint32_t end = pos + len, n = 0;    // $E5, the byte, a word with the count
            while (pos < end) {
                uint8_t b = file[pos++];
                uint32_t count = 1;
                if (b == 0xE5) {
                    if (pos + 3 > end)
                        return false;
                    b = file[pos];
                    count = be16(file + pos + 1);
                    pos += 3;
                }
                if (n + count > track_size)
                    return false;
                memset(dst + n, b, count);
                n += count;
            }
            if (n != track_size)
                return false;
        }
    }
    return true;
}

void floppy_make_bpb(const uint8_t *s, uint32_t sectors, uint16_t bpb[9])
{
    uint32_t recsize = le16(s + 11), clsiz = s[13], res = le16(s + 14), fats = s[16];
    uint32_t dirs = le16(s + 17), spf = le16(s + 22);
    if (recsize != SECTOR || !clsiz || !spf || !fats || !dirs) {
        // Not a GEMDOS disk (a game with its own loader): as a normal one
        clsiz = 2;
        res = 1;
        fats = 2;
        dirs = 112;
        spf = 5;
        recsize = SECTOR;
    }
    uint32_t rdlen = (dirs * 32 + recsize - 1) / recsize;
    uint32_t fatrec = res + (fats - 1) * spf;   // the first sector of the last FAT
    uint32_t datrec = res + fats * spf + rdlen;
    uint32_t numcl = sectors > datrec ? (sectors - datrec) / clsiz : 0;
    bpb[0] = recsize;
    bpb[1] = clsiz;
    bpb[2] = recsize * clsiz;
    bpb[3] = rdlen;
    bpb[4] = spf;
    bpb[5] = fatrec;
    bpb[6] = datrec;
    bpb[7] = numcl;
    bpb[8] = (numcl > 4084 ? 1 : 0) | (fats < 2 ? 2 : 0);
}
