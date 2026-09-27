/* ============================================================================
 * ziputil.c — 最小 ZIP 读取 + 手写 RFC 1951 inflate（deflate）解压。
 * 不依赖任何第三方库，用于替代 PowerShell Expand-Archive（大包会内存溢出）。
 * ========================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ziputil.h"

/* ------------------------------------------------------------------ */
/* 位读取器（LSB first，RFC1951 约定）                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    const BYTE *in;
    size_t pos, len;
    DWORD bitbuf;
    int bitcnt;
    int err;
} BR;

static DWORD br_get(BR *b, int n)
{
    DWORD v = 0;
    if (b->err)
        return 0;
    while (b->bitcnt < n) {
        if (b->pos >= b->len) {
            b->err = 1;
            return 0;
        }
        b->bitbuf |= (DWORD)b->in[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    v = b->bitbuf & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return v;
}

static void br_align(BR *b)
{
    b->bitbuf = 0;
    b->bitcnt = 0;
}

/* ------------------------------------------------------------------ */
/* Huffman（规范编码）                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    int count[16];
    int sym[288];
} HUF;

static int huf_build(HUF *h, const int *lens, int nsyms)
{
    int i, len, offs[16], left = 1;

    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < nsyms; i++) {
        if (lens[i] < 0 || lens[i] > 15)
            return -1;
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;               /* 超订（over-subscribed） */
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (i = 0; i < nsyms; i++)
        if (lens[i])
            h->sym[offs[lens[i]]++] = i;
    return 0;
}

static int huf_decode(BR *b, const HUF *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        code |= (int)br_get(b, 1);
        if (code - first < h->count[len])
            return h->sym[index + (code - first)];
        index += h->count[len];
        first = (first + h->count[len]) << 1;
        code <<= 1;
    }
    b->err = 1;
    return -1;
}

static void huf_build_fixed(HUF *lit, HUF *dist)
{
    int lens[288];
    int i;
    for (i = 0; i <= 143; i++) lens[i] = 8;
    for (; i <= 255; i++) lens[i] = 9;
    for (; i <= 279; i++) lens[i] = 7;
    for (; i <= 287; i++) lens[i] = 8;
    huf_build(lit, lens, 288);
    for (i = 0; i < 30; i++) lens[i] = 5;
    huf_build(dist, lens, 30);
}

static const int CLEN_ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int huf_build_dynamic(BR *b, HUF *lit, HUF *dist)
{
    int nlit = (int)br_get(b, 5) + 257;
    int ndist = (int)br_get(b, 5) + 1;
    int nclen = (int)br_get(b, 4) + 4;
    int i, lens[320];
    HUF clh;

    memset(lens, 0, sizeof(lens));
    for (i = 0; i < nclen; i++)
        lens[CLEN_ORDER[i]] = (int)br_get(b, 3);
    if (huf_build(&clh, lens, 19))
        return -1;

    memset(lens, 0, sizeof(lens));
    i = 0;
    while (i < nlit + ndist) {
        int sym = huf_decode(b, &clh);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lens[i++] = sym;
        } else if (sym == 16) {
            int prev, rep = 3 + (int)br_get(b, 2);
            if (i == 0)
                return -1;
            prev = lens[i - 1];
            while (rep-- && i < nlit + ndist)
                lens[i++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)br_get(b, 3);
            while (rep-- && i < nlit + ndist)
                lens[i++] = 0;
        } else {
            int rep = 11 + (int)br_get(b, 7);
            while (rep-- && i < nlit + ndist)
                lens[i++] = 0;
        }
    }
    if (huf_build(lit, lens, nlit))
        return -1;
    if (huf_build(dist, lens + nlit, ndist))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 输出（32KB 环形历史窗口 + 批量写文件）                               */
/* ------------------------------------------------------------------ */
#define WSIZE 32768

typedef struct {
    HANDLE h;
    BYTE outbuf[65536];
    DWORD outlen;
    BYTE win[WSIZE];
    DWORD total;
    int err;
} WOUT;

static void wo_flush(WOUT *w)
{
    DWORD wr = 0;
    if (!w->outlen)
        return;
    if (!WriteFile(w->h, w->outbuf, w->outlen, &wr, NULL) || wr != w->outlen)
        w->err = 1;
    w->outlen = 0;
}

static void wo_byte(WOUT *w, BYTE c)
{
    if (w->err)
        return;
    w->win[w->total & (WSIZE - 1)] = c;
    w->total++;
    w->outbuf[w->outlen++] = c;
    if (w->outlen == sizeof(w->outbuf))
        wo_flush(w);
}

static void wo_copy(WOUT *w, DWORD dist, DWORD len)
{
    DWORD base = (w->total - dist) & (WSIZE - 1);
    DWORD i;
    for (i = 0; i < len && !w->err; i++) {
        BYTE c = w->win[(base + i) & (WSIZE - 1)];
        w->win[w->total & (WSIZE - 1)] = c;
        w->total++;
        w->outbuf[w->outlen++] = c;
        if (w->outlen == sizeof(w->outbuf))
            wo_flush(w);
    }
}

static const int LENBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int LENEXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const int DISTBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const int DISTEXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inflate_blocks(BR *b, const HUF *lit, const HUF *dist, WOUT *w)
{
    for (;;) {
        int sym = huf_decode(b, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            wo_byte(w, (BYTE)sym);
            continue;
        }
        if (sym == 256)
            return 0;
        if (sym > 285)
            return -1;
        {
            int lsym = sym - 257;
            DWORD len = (DWORD)LENBASE[lsym] + br_get(b, LENEXTRA[lsym]);
            int dsym = huf_decode(b, dist);
            DWORD d;
            if (dsym < 0 || dsym > 29)
                return -1;
            d = (DWORD)DISTBASE[dsym] + br_get(b, DISTEXTRA[dsym]);
            if (d > w->total)
                return -1;
            wo_copy(w, d, len);
        }
        if (w->err)
            return -1;
    }
}

static int inflate_raw(const BYTE *in, size_t inLen, HANDLE hOut, DWORD *outTotal)
{
    BR b;
    WOUT w;
    int final = 0;

    b.in = in;
    b.pos = 0;
    b.len = inLen;
    b.bitbuf = 0;
    b.bitcnt = 0;
    b.err = 0;
    memset(&w, 0, sizeof(w));
    w.h = hOut;

    while (!final && !b.err && !w.err) {
        HUF lit, dist;
        final = (int)br_get(&b, 1);
        switch (br_get(&b, 2)) {
        case 0: {   /* 未压缩块 */
            DWORD len, nlen, i;
            br_align(&b);
            len = br_get(&b, 16);
            nlen = br_get(&b, 16);
            if ((len ^ 0xFFFFu) != nlen) {
                b.err = 1;
                break;
            }
            if (b.pos + len > b.len) {
                b.err = 1;
                break;
            }
            for (i = 0; i < len; i++)
                wo_byte(&w, b.in[b.pos++]);
            break;
        }
        case 1:
            huf_build_fixed(&lit, &dist);
            if (inflate_blocks(&b, &lit, &dist, &w))
                b.err = 1;
            break;
        case 2:
            if (huf_build_dynamic(&b, &lit, &dist) ||
                inflate_blocks(&b, &lit, &dist, &w))
                b.err = 1;
            break;
        default:
            b.err = 1;
            break;
        }
    }
    wo_flush(&w);
    if (outTotal)
        *outTotal = w.total;
    return (b.err || w.err) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* ZIP 读取                                                            */
/* ------------------------------------------------------------------ */
static DWORD rd16(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8); }
static DWORD rd32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) |
           ((DWORD)p[3] << 24);
}

int ZipExtractEntry(const wchar_t *zipPath, const wchar_t *nameSuffix,
                    const wchar_t *dest)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    DWORD rd = 0;
    BYTE eocd[22];
    int i, rc = -1;
    DWORD nEntries, cdSize, cdOff;
    BYTE *cd = NULL;
    char suffixUtf8[256];
    int suffixLen;

    suffixLen = WideCharToMultiByte(CP_UTF8, 0, nameSuffix, -1, suffixUtf8,
                                    (int)sizeof(suffixUtf8), NULL, NULL) - 1;
    if (suffixLen <= 0 || suffixLen > 250)
        return -1;

    h = CreateFileW(zipPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    if (!GetFileSizeEx(h, &size) || size.QuadPart < 22)
        goto done;

    /* 扫描 EOCD（最后 64KB+22 内） */
    {
        DWORD tailLen = (DWORD)(size.QuadPart < 65557 ? size.QuadPart : 65557);
        BYTE *tail = (BYTE *)malloc(tailLen);
        LARGE_INTEGER off;
        if (!tail)
            goto done;
        off.QuadPart = size.QuadPart - tailLen;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
        ReadFile(h, tail, tailLen, &rd, NULL);
        if (rd != tailLen) {
            free(tail);
            goto done;
        }
        for (i = (int)tailLen - 22; i >= 0; i--) {
            if (rd32(tail + i) == 0x06054b50)
                break;
        }
        if (i < 0) {
            free(tail);
            goto done;
        }
        memcpy(eocd, tail + i, 22);
        free(tail);
    }

    nEntries = rd16(eocd + 10);
    cdSize = rd32(eocd + 12);
    cdOff = rd32(eocd + 16);
    if (!cdSize || cdSize > 268435456 || !nEntries)
        goto done;

    cd = (BYTE *)malloc(cdSize);
    if (!cd)
        goto done;
    {
        LARGE_INTEGER off;
        off.QuadPart = cdOff;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
        ReadFile(h, cd, cdSize, &rd, NULL);
        if (rd != cdSize)
            goto done;
    }

    /* 遍历中央目录，找名字以 suffix 结尾的普通文件条目 */
    {
        DWORD off = 0;
        while (off + 46 <= cdSize && rd32(cd + off) == 0x02014b50) {
            WORD method = (WORD)rd16(cd + off + 10);
            DWORD csize = rd32(cd + off + 20);
            DWORD usize = rd32(cd + off + 24);
            WORD namelen = (WORD)rd16(cd + off + 28);
            WORD extralen = (WORD)rd16(cd + off + 30);
            WORD commentlen = (WORD)rd16(cd + off + 32);
            DWORD localoff = rd32(cd + off + 42);
            const char *name = (const char *)(cd + off + 46);

            if (off + 46 + namelen > cdSize)
                goto done;

            if (namelen >= (WORD)suffixLen &&
                (csize != 0xFFFFFFFFu) && (usize != 0xFFFFFFFFu) &&
                (localoff != 0xFFFFFFFFu)) {
                int match = 0;
                if (namelen == (WORD)suffixLen) {
                    match = _strnicmp(name, suffixUtf8, suffixLen) == 0;
                } else {
                    char c = name[namelen - suffixLen - 1];
                    if (c == '/' || c == '\\')
                        match = _strnicmp(name + namelen - suffixLen,
                                          suffixUtf8, suffixLen) == 0;
                }
                if (match && name[namelen - 1] != '/' && usize > 0) {
                    /* 找到目标条目：读 local header 定位数据 */
                    BYTE lh[30];
                    DWORD dataOff, lnamelen, lextralen;
                    LARGE_INTEGER lo;
                    HANDLE out;
                    BYTE *cbuf = NULL;
                    int ir;
                    DWORD written = 0;

                    lo.QuadPart = localoff;
                    SetFilePointerEx(h, lo, NULL, FILE_BEGIN);
                    ReadFile(h, lh, 30, &rd, NULL);
                    if (rd != 30 || rd32(lh) != 0x04034b50)
                        goto done;
                    lnamelen = rd16(lh + 26);
                    lextralen = rd16(lh + 28);
                    dataOff = localoff + 30 + lnamelen + lextralen;
                    if (method != 0 && method != 8)
                        goto done;   /* 不支持的压缩方法 */

                    out = CreateFileW(dest, GENERIC_WRITE, 0, NULL,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (out == INVALID_HANDLE_VALUE)
                        goto done;

                    cbuf = (BYTE *)malloc(csize ? csize : 1);
                    if (!cbuf) {
                        CloseHandle(out);
                        DeleteFileW(dest);
                        goto done;
                    }
                    lo.QuadPart = dataOff;
                    SetFilePointerEx(h, lo, NULL, FILE_BEGIN);
                    ReadFile(h, cbuf, csize, &rd, NULL);
                    if (rd != csize) {
                        free(cbuf);
                        CloseHandle(out);
                        DeleteFileW(dest);
                        goto done;
                    }

                    if (method == 8) {
                        ir = inflate_raw(cbuf, csize, out, &written);
                    } else {
                        DWORD w2 = 0;
                        ir = (!WriteFile(out, cbuf, csize, &w2, NULL) || w2 != csize)
                             ? -1 : 0;
                        written = csize;
                    }
                    free(cbuf);
                    CloseHandle(out);
                    if (ir != 0 || written != usize) {
                        DeleteFileW(dest);
                        goto done;
                    }
                    rc = 0;
                    goto done;
                }
            }
            off += 46 + namelen + extralen + commentlen;
        }
    }

done:
    if (cd)
        free(cd);
    CloseHandle(h);
    return rc;
}
