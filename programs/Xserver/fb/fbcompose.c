/*
 *
 * Copyright © 2000 Keith Packard, member of The XFree86 Project, Inc.
 *             2005 Lars Knoll & Zack Rusin, Trolltech
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "fb.h"

#ifdef RENDER

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "picturestr.h"
#include "mipict.h"
#include "fbpict.h"

static unsigned int
SourcePictureClassify (PicturePtr pict,
		       int	  x,
		       int	  y,
		       int	  width,
		       int	  height)
{
    if (pict->pSourcePict->type == SourcePictTypeSolidFill)
    {
	pict->pSourcePict->solidFill.class = SourcePictClassHorizontal;
    }
    else if (pict->pSourcePict->type == SourcePictTypeLinear)
    {
	PictVector   v;
	xFixed_32_32 l;
	xFixed_48_16 dx, dy, a, b, off;
	xFixed_48_16 factors[4];
	int	     i;

	dx = pict->pSourcePict->linear.p2.x - pict->pSourcePict->linear.p1.x;
	dy = pict->pSourcePict->linear.p2.y - pict->pSourcePict->linear.p1.y;
	l = dx * dx + dy * dy;
	if (l)
	{
	    a = (dx << 32) / l;
	    b = (dy << 32) / l;
	}
	else
	{
	    a = b = 0;
	}

	off = (-a * pict->pSourcePict->linear.p1.x
	       -b * pict->pSourcePict->linear.p1.y) >> 16;

	for (i = 0; i < 3; i++)
	{
	    v.vector[0] = IntToxFixed ((i % 2) * (width  - 1) + x);
	    v.vector[1] = IntToxFixed ((i / 2) * (height - 1) + y);
	    v.vector[2] = xFixed1;

	    if (pict->transform)
	    {
		if (!PictureTransformPoint3d (pict->transform, &v))
		    return SourcePictClassUnknown;
	    }

	    factors[i] = ((a * v.vector[0] + b * v.vector[1]) >> 16) + off;
	}

	if (factors[2] == factors[0])
	    pict->pSourcePict->linear.class = SourcePictClassHorizontal;
	else if (factors[1] == factors[0])
	    pict->pSourcePict->linear.class = SourcePictClassVertical;
    }

    return pict->pSourcePict->solidFill.class;
}

#define mod(a,b)	((b) == 1 ? 0 : (a) >= 0 ? (a) % (b) : (b) - (-a) % (b))

#define SCANLINE_BUFFER_LENGTH 2048

typedef FASTCALL void (*fetchProc)(const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed);

/*
 * All of the fetch functions
 */

static FASTCALL void
fbFetch_a8r8g8b8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    MEMCPY_WRAPPED(buffer, (const CARD32 *)bits + x, width*sizeof(CARD32));
}

static FASTCALL void
fbFetch_x8r8g8b8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD32 *pixel = (const CARD32 *)bits + x;
    const CARD32 *end = pixel + width;
    while (pixel < end) {
        WRITE(buffer++, READ(pixel++) | 0xff000000);
    }
}

static FASTCALL void
fbFetch_a8b8g8r8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD32 *pixel = (CARD32 *)bits + x;
    const CARD32 *end = pixel + width;
    while (pixel < end) {
        WRITE(buffer++, ((READ(pixel) & 0xff00ff00) |
                         ((READ(pixel) >> 16) & 0xff) |
                         ((READ(pixel) & 0xff) << 16)));
        ++pixel;
    }
}

static FASTCALL void
fbFetch_x8b8g8r8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD32 *pixel = (CARD32 *)bits + x;
    const CARD32 *end = pixel + width;
    while (pixel < end) {
        WRITE(buffer++, 0xff000000 |
	      ((READ(pixel) & 0x0000ff00) |
	       ((READ(pixel) >> 16) & 0xff) |
	       ((READ(pixel) & 0xff) << 16)));
        ++pixel;
    }
}

static FASTCALL void
fbFetch_r8g8b8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + 3*x;
    const CARD8 *end = pixel + 3*width;
    while (pixel < end) {
        CARD32 b = Fetch24(pixel) | 0xff000000;
        pixel += 3;
        WRITE(buffer++, b);
    }
}

static FASTCALL void
fbFetch_b8g8r8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + 3*x;
    const CARD8 *end = pixel + 3*width;
    while (pixel < end) {
        CARD32 b = 0xff000000;
#if IMAGE_BYTE_ORDER == MSBFirst
        b |= (READ(pixel++));
        b |= (READ(pixel++) << 8);
        b |= (READ(pixel++) << 16);
#else
        b |= (READ(pixel++) << 16);
        b |= (READ(pixel++) << 8);
        b |= (READ(pixel++));
#endif
    }
}

static FASTCALL void
fbFetch_r5g6b5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32 r = (((p) << 3) & 0xf8) | 
	    (((p) << 5) & 0xfc00) |
	    (((p) << 8) & 0xf80000);
        r |= (r >> 5) & 0x70007;
        r |= (r >> 6) & 0x300;
        WRITE(buffer++, 0xff000000 | r);
    }
}

static FASTCALL void
fbFetch_b5g6r5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        b = ((p & 0xf800) | ((p & 0xe000) >> 5)) >> 8;
        g = ((p & 0x07e0) | ((p & 0x0600) >> 6)) << 5;
        r = ((p & 0x001c) | ((p & 0x001f) << 5)) << 14;
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a1r5g5b5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b, a;

        a = (CARD32) ((CARD8) (0 - ((p & 0x8000) >> 15))) << 24;
        r = ((p & 0x7c00) | ((p & 0x7000) >> 5)) << 9;
        g = ((p & 0x03e0) | ((p & 0x0380) >> 5)) << 6;
        b = ((p & 0x001c) | ((p & 0x001f) << 5)) >> 2;
        WRITE(buffer++, (a | r | g | b));
    }
}

static FASTCALL void
fbFetch_x1r5g5b5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        r = ((p & 0x7c00) | ((p & 0x7000) >> 5)) << 9;
        g = ((p & 0x03e0) | ((p & 0x0380) >> 5)) << 6;
        b = ((p & 0x001c) | ((p & 0x001f) << 5)) >> 2;
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a1b5g5r5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b, a;

        a = (CARD32) ((CARD8) (0 - ((p & 0x8000) >> 15))) << 24;
        b = ((p & 0x7c00) | ((p & 0x7000) >> 5)) >> 7;
        g = ((p & 0x03e0) | ((p & 0x0380) >> 5)) << 6;
        r = ((p & 0x001c) | ((p & 0x001f) << 5)) << 14;
        WRITE(buffer++, (a | r | g | b));
    }
}

static FASTCALL void
fbFetch_x1b5g5r5 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        b = ((p & 0x7c00) | ((p & 0x7000) >> 5)) >> 7;
        g = ((p & 0x03e0) | ((p & 0x0380) >> 5)) << 6;
        r = ((p & 0x001c) | ((p & 0x001f) << 5)) << 14;
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a4r4g4b4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b, a;

        a = ((p & 0xf000) | ((p & 0xf000) >> 4)) << 16;
        r = ((p & 0x0f00) | ((p & 0x0f00) >> 4)) << 12;
        g = ((p & 0x00f0) | ((p & 0x00f0) >> 4)) << 8;
        b = ((p & 0x000f) | ((p & 0x000f) << 4));
        WRITE(buffer++, (a | r | g | b));
    }
}

static FASTCALL void
fbFetch_x4r4g4b4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        r = ((p & 0x0f00) | ((p & 0x0f00) >> 4)) << 12;
        g = ((p & 0x00f0) | ((p & 0x00f0) >> 4)) << 8;
        b = ((p & 0x000f) | ((p & 0x000f) << 4));
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a4b4g4r4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b, a;

        a = ((p & 0xf000) | ((p & 0xf000) >> 4)) << 16;
        b = ((p & 0x0f00) | ((p & 0x0f00) >> 4)) >> 4;
        g = ((p & 0x00f0) | ((p & 0x00f0) >> 4)) << 8;
        r = ((p & 0x000f) | ((p & 0x000f) << 4)) << 16;
        WRITE(buffer++, (a | r | g | b));
    }
}

static FASTCALL void
fbFetch_x4b4g4r4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD16 *pixel = (const CARD16 *)bits + x;
    const CARD16 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        b = ((p & 0x0f00) | ((p & 0x0f00) >> 4)) >> 4;
        g = ((p & 0x00f0) | ((p & 0x00f0) >> 4)) << 8;
        r = ((p & 0x000f) | ((p & 0x000f) << 4)) << 16;
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        WRITE(buffer++, READ(pixel++) << 24);
    }
}

static FASTCALL void
fbFetch_r3g3b2 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        r = ((p & 0xe0) | ((p & 0xe0) >> 3) | ((p & 0xc0) >> 6)) << 16;
        g = ((p & 0x1c) | ((p & 0x18) >> 3) | ((p & 0x1c) << 3)) << 8;
        b = (((p & 0x03)     ) |
             ((p & 0x03) << 2) |
             ((p & 0x03) << 4) |
             ((p & 0x03) << 6));
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_b2g3r3 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32  r,g,b;

        b = (((p & 0xc0)     ) |
             ((p & 0xc0) >> 2) |
             ((p & 0xc0) >> 4) |
             ((p & 0xc0) >> 6));
        g = ((p & 0x38) | ((p & 0x38) >> 3) | ((p & 0x30) << 2)) << 8;
        r = (((p & 0x07)     ) |
             ((p & 0x07) << 3) |
             ((p & 0x06) << 6)) << 16;
        WRITE(buffer++, (0xff000000 | r | g | b));
    }
}

static FASTCALL void
fbFetch_a2r2g2b2 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32   a,r,g,b;

        a = ((p & 0xc0) * 0x55) << 18;
        r = ((p & 0x30) * 0x55) << 12;
        g = ((p & 0x0c) * 0x55) << 6;
        b = ((p & 0x03) * 0x55);
        WRITE(buffer++, a|r|g|b);
    }
}

static FASTCALL void
fbFetch_a2b2g2r2 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        CARD32   a,r,g,b;

        a = ((p & 0xc0) * 0x55) << 18;
        b = ((p & 0x30) * 0x55) >> 6;
        g = ((p & 0x0c) * 0x55) << 6;
        r = ((p & 0x03) * 0x55) << 16;
        WRITE(buffer++, a|r|g|b);
    }
}

static FASTCALL void
fbFetch_c8 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
        CARD32  p = READ(pixel++);
        WRITE(buffer++, indexed->rgba[p]);
    }
}

static FASTCALL void
fbFetch_x4a4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    const CARD8 *pixel = (const CARD8 *)bits + x;
    const CARD8 *end = pixel + width;
    while (pixel < end) {
	CARD8 p = READ(pixel++) & 0xf;
        WRITE(buffer++, (p | (p << 4)) << 24);
    }
}

#define Fetch8(l,o)    (((CARD8 *) (l))[(o) >> 2])
#if IMAGE_BYTE_ORDER == MSBFirst
#define Fetch4(l,o)    ((o) & 2 ? Fetch8(l,o) & 0xf : Fetch8(l,o) >> 4)
#else
#define Fetch4(l,o)    ((o) & 2 ? Fetch8(l,o) >> 4 : Fetch8(l,o) & 0xf)
#endif

static FASTCALL void
fbFetch_a4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);

        p |= p << 4;
        WRITE(buffer++, p << 24);
    }
}

static FASTCALL void
fbFetch_r1g2b1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);
        CARD32  r,g,b;

        r = ((p & 0x8) * 0xff) << 13;
        g = ((p & 0x6) * 0x55) << 7;
        b = ((p & 0x1) * 0xff);
        WRITE(buffer++, 0xff000000|r|g|b);
    }
}

static FASTCALL void
fbFetch_b1g2r1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);
        CARD32  r,g,b;

        b = ((p & 0x8) * 0xff) >> 3;
        g = ((p & 0x6) * 0x55) << 7;
        r = ((p & 0x1) * 0xff) << 16;
        WRITE(buffer++, 0xff000000|r|g|b);
    }
}

static FASTCALL void
fbFetch_a1r1g1b1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);
        CARD32  a,r,g,b;

        a = ((p & 0x8) * 0xff) << 21;
        r = ((p & 0x4) * 0xff) << 14;
        g = ((p & 0x2) * 0xff) << 7;
        b = ((p & 0x1) * 0xff);
        WRITE(buffer++, a|r|g|b);
    }
}

static FASTCALL void
fbFetch_a1b1g1r1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);
        CARD32  a,r,g,b;

        a = ((p & 0x8) * 0xff) << 21;
        r = ((p & 0x4) * 0xff) >> 3;
        g = ((p & 0x2) * 0xff) << 7;
        b = ((p & 0x1) * 0xff) << 16;
        WRITE(buffer++, a|r|g|b);
    }
}

static FASTCALL void
fbFetch_c4 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = Fetch4(bits, i + x);

        WRITE(buffer++, indexed->rgba[p]);
    }
}


static FASTCALL void
fbFetch_a1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = ((CARD32 *)bits)[(i + x) >> 5];
        CARD32  a;
#if BITMAP_BIT_ORDER == MSBFirst
        a = p >> (0x1f - ((i+x) & 0x1f));
#else
        a = p >> ((i+x) & 0x1f);
#endif
        a = a & 1;
        a |= a << 1;
        a |= a << 2;
        a |= a << 4;
        WRITE(buffer++, a << 24);
    }
}

static FASTCALL void
fbFetch_g1 (const FbBits *bits, int x, int width, CARD32 *buffer, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  p = ((CARD32 *)bits)[(i+x) >> 5];
        CARD32 a;
#if BITMAP_BIT_ORDER == MSBFirst
        a = p >> (0x1f - ((i+x) & 0x1f));
#else
        a = p >> ((i+x) & 0x1f);
#endif
        a = a & 1;
        WRITE(buffer++, indexed->rgba[a]);
    }
}

static fetchProc fetchProcForPicture (PicturePtr pict)
{
    switch(pict->format) {
    case PICT_a8r8g8b8: return fbFetch_a8r8g8b8;
    case PICT_x8r8g8b8: return fbFetch_x8r8g8b8;
    case PICT_a8b8g8r8: return fbFetch_a8b8g8r8;
    case PICT_x8b8g8r8: return fbFetch_x8b8g8r8;

        /* 24bpp formats */
    case PICT_r8g8b8: return fbFetch_r8g8b8;
    case PICT_b8g8r8: return fbFetch_b8g8r8;

        /* 16bpp formats */
    case PICT_r5g6b5: return fbFetch_r5g6b5;
    case PICT_b5g6r5: return fbFetch_b5g6r5;

    case PICT_a1r5g5b5: return fbFetch_a1r5g5b5;
    case PICT_x1r5g5b5: return fbFetch_x1r5g5b5;
    case PICT_a1b5g5r5: return fbFetch_a1b5g5r5;
    case PICT_x1b5g5r5: return fbFetch_x1b5g5r5;
    case PICT_a4r4g4b4: return fbFetch_a4r4g4b4;
    case PICT_x4r4g4b4: return fbFetch_x4r4g4b4;
    case PICT_a4b4g4r4: return fbFetch_a4b4g4r4;
    case PICT_x4b4g4r4: return fbFetch_x4b4g4r4;

        /* 8bpp formats */
    case PICT_a8: return  fbFetch_a8;
    case PICT_r3g3b2: return fbFetch_r3g3b2;
    case PICT_b2g3r3: return fbFetch_b2g3r3;
    case PICT_a2r2g2b2: return fbFetch_a2r2g2b2;
    case PICT_a2b2g2r2: return fbFetch_a2b2g2r2;
    case PICT_c8: return  fbFetch_c8;
    case PICT_g8: return  fbFetch_c8;
    case PICT_x4a4: return fbFetch_x4a4;

        /* 4bpp formats */
    case PICT_a4: return  fbFetch_a4;
    case PICT_r1g2b1: return fbFetch_r1g2b1;
    case PICT_b1g2r1: return fbFetch_b1g2r1;
    case PICT_a1r1g1b1: return fbFetch_a1r1g1b1;
    case PICT_a1b1g1r1: return fbFetch_a1b1g1r1;
    case PICT_c4: return  fbFetch_c4;
    case PICT_g4: return  fbFetch_c4;

        /* 1bpp formats */
    case PICT_a1: return  fbFetch_a1;
    case PICT_g1: return  fbFetch_g1;
    }

    return NULL;
}

/*
 * Pixel wise fetching
 */

typedef FASTCALL CARD32 (*fetchPixelProc)(const FbBits *bits, int offset, miIndexedPtr indexed);

static FASTCALL CARD32
fbFetchPixel_a8r8g8b8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    return READ((CARD32 *)bits + offset);
}

static FASTCALL CARD32
fbFetchPixel_x8r8g8b8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    return READ((CARD32 *)bits + offset) | 0xff000000;
}

static FASTCALL CARD32
fbFetchPixel_a8b8g8r8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD32 *)bits + offset);

    return ((pixel & 0xff000000) |
	    ((pixel >> 16) & 0xff) |
	    (pixel & 0x0000ff00) |
	    ((pixel & 0xff) << 16));
}

static FASTCALL CARD32
fbFetchPixel_x8b8g8r8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD32 *)bits + offset);

    return ((0xff000000) |
	    ((pixel >> 16) & 0xff) |
	    (pixel & 0x0000ff00) |
	    ((pixel & 0xff) << 16));
}

static FASTCALL CARD32
fbFetchPixel_r8g8b8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD8   *pixel = ((CARD8 *) bits) + (offset*3);
#if IMAGE_BYTE_ORDER == MSBFirst
    return (0xff000000 |
	    (READ(pixel + 0) << 16) |
	    (READ(pixel + 1) << 8) |
	    (READ(pixel + 2)));
#else
    return (0xff000000 |
            (READ(pixel + 2) << 16) |
            (READ(pixel + 1) << 8) |
            (READ(pixel + 0)));
#endif
}

static FASTCALL CARD32
fbFetchPixel_b8g8r8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD8   *pixel = ((CARD8 *) bits) + (offset*3);
#if IMAGE_BYTE_ORDER == MSBFirst
    return (0xff000000 |
	    (READ(pixel + 2) << 16) |
	    (READ(pixel + 1) << 8) |
	    (READ(pixel + 0)));
#else
    return (0xff000000 |
	    (READ(pixel + 0) << 16) |
	    (READ(pixel + 1) << 8) |
	    (READ(pixel + 2)));
#endif
}

static FASTCALL CARD32
fbFetchPixel_r5g6b5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    r = ((pixel & 0xf800) | ((pixel & 0xe000) >> 5)) << 8;
    g = ((pixel & 0x07e0) | ((pixel & 0x0600) >> 6)) << 5;
    b = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) >> 2;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_b5g6r5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    b = ((pixel & 0xf800) | ((pixel & 0xe000) >> 5)) >> 8;
    g = ((pixel & 0x07e0) | ((pixel & 0x0600) >> 6)) << 5;
    r = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) << 14;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a1r5g5b5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  a,r,g,b;

    a = (CARD32) ((CARD8) (0 - ((pixel & 0x8000) >> 15))) << 24;
    r = ((pixel & 0x7c00) | ((pixel & 0x7000) >> 5)) << 9;
    g = ((pixel & 0x03e0) | ((pixel & 0x0380) >> 5)) << 6;
    b = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) >> 2;
    return (a | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_x1r5g5b5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    r = ((pixel & 0x7c00) | ((pixel & 0x7000) >> 5)) << 9;
    g = ((pixel & 0x03e0) | ((pixel & 0x0380) >> 5)) << 6;
    b = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) >> 2;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a1b5g5r5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  a,r,g,b;

    a = (CARD32) ((CARD8) (0 - ((pixel & 0x8000) >> 15))) << 24;
    b = ((pixel & 0x7c00) | ((pixel & 0x7000) >> 5)) >> 7;
    g = ((pixel & 0x03e0) | ((pixel & 0x0380) >> 5)) << 6;
    r = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) << 14;
    return (a | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_x1b5g5r5 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    b = ((pixel & 0x7c00) | ((pixel & 0x7000) >> 5)) >> 7;
    g = ((pixel & 0x03e0) | ((pixel & 0x0380) >> 5)) << 6;
    r = ((pixel & 0x001c) | ((pixel & 0x001f) << 5)) << 14;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a4r4g4b4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  a,r,g,b;

    a = ((pixel & 0xf000) | ((pixel & 0xf000) >> 4)) << 16;
    r = ((pixel & 0x0f00) | ((pixel & 0x0f00) >> 4)) << 12;
    g = ((pixel & 0x00f0) | ((pixel & 0x00f0) >> 4)) << 8;
    b = ((pixel & 0x000f) | ((pixel & 0x000f) << 4));
    return (a | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_x4r4g4b4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    r = ((pixel & 0x0f00) | ((pixel & 0x0f00) >> 4)) << 12;
    g = ((pixel & 0x00f0) | ((pixel & 0x00f0) >> 4)) << 8;
    b = ((pixel & 0x000f) | ((pixel & 0x000f) << 4));
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a4b4g4r4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  a,r,g,b;

    a = ((pixel & 0xf000) | ((pixel & 0xf000) >> 4)) << 16;
    b = ((pixel & 0x0f00) | ((pixel & 0x0f00) >> 4)) >> 4;
    g = ((pixel & 0x00f0) | ((pixel & 0x00f0) >> 4)) << 8;
    r = ((pixel & 0x000f) | ((pixel & 0x000f) << 4)) << 16;
    return (a | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_x4b4g4r4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = READ((CARD16 *) bits + offset);
    CARD32  r,g,b;

    b = ((pixel & 0x0f00) | ((pixel & 0x0f00) >> 4)) >> 4;
    g = ((pixel & 0x00f0) | ((pixel & 0x00f0) >> 4)) << 8;
    r = ((pixel & 0x000f) | ((pixel & 0x000f) << 4)) << 16;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);

    return pixel << 24;
}

static FASTCALL CARD32
fbFetchPixel_r3g3b2 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);
    CARD32  r,g,b;

    r = ((pixel & 0xe0) | ((pixel & 0xe0) >> 3) | ((pixel & 0xc0) >> 6)) << 16;
    g = ((pixel & 0x1c) | ((pixel & 0x18) >> 3) | ((pixel & 0x1c) << 3)) << 8;
    b = (((pixel & 0x03)     ) |
	 ((pixel & 0x03) << 2) |
	 ((pixel & 0x03) << 4) |
	 ((pixel & 0x03) << 6));
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_b2g3r3 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);
    CARD32  r,g,b;

    b = (((pixel & 0xc0)     ) |
	 ((pixel & 0xc0) >> 2) |
	 ((pixel & 0xc0) >> 4) |
	 ((pixel & 0xc0) >> 6));
    g = ((pixel & 0x38) | ((pixel & 0x38) >> 3) | ((pixel & 0x30) << 2)) << 8;
    r = (((pixel & 0x07)     ) |
	 ((pixel & 0x07) << 3) |
	 ((pixel & 0x06) << 6)) << 16;
    return (0xff000000 | r | g | b);
}

static FASTCALL CARD32
fbFetchPixel_a2r2g2b2 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);
    CARD32   a,r,g,b;

    a = ((pixel & 0xc0) * 0x55) << 18;
    r = ((pixel & 0x30) * 0x55) << 12;
    g = ((pixel & 0x0c) * 0x55) << 6;
    b = ((pixel & 0x03) * 0x55);
    return a|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_a2b2g2r2 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);
    CARD32   a,r,g,b;

    a = ((pixel & 0xc0) * 0x55) << 18;
    b = ((pixel & 0x30) * 0x55) >> 6;
    g = ((pixel & 0x0c) * 0x55) << 6;
    r = ((pixel & 0x03) * 0x55) << 16;
    return a|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_c8 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);
    return indexed->rgba[pixel];
}

static FASTCALL CARD32
fbFetchPixel_x4a4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32   pixel = READ((CARD8 *) bits + offset);

    return ((pixel & 0xf) | ((pixel & 0xf) << 4)) << 24;
}

#define Fetch8(l,o)    (((CARD8 *) (l))[(o) >> 2])
#if IMAGE_BYTE_ORDER == MSBFirst
#define Fetch4(l,o)    ((o) & 2 ? Fetch8(l,o) & 0xf : Fetch8(l,o) >> 4)
#else
#define Fetch4(l,o)    ((o) & 2 ? Fetch8(l,o) >> 4 : Fetch8(l,o) & 0xf)
#endif

static FASTCALL CARD32
fbFetchPixel_a4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);

    pixel |= pixel << 4;
    return pixel << 24;
}

static FASTCALL CARD32
fbFetchPixel_r1g2b1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);
    CARD32  r,g,b;

    r = ((pixel & 0x8) * 0xff) << 13;
    g = ((pixel & 0x6) * 0x55) << 7;
    b = ((pixel & 0x1) * 0xff);
    return 0xff000000|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_b1g2r1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);
    CARD32  r,g,b;

    b = ((pixel & 0x8) * 0xff) >> 3;
    g = ((pixel & 0x6) * 0x55) << 7;
    r = ((pixel & 0x1) * 0xff) << 16;
    return 0xff000000|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_a1r1g1b1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);
    CARD32  a,r,g,b;

    a = ((pixel & 0x8) * 0xff) << 21;
    r = ((pixel & 0x4) * 0xff) << 14;
    g = ((pixel & 0x2) * 0xff) << 7;
    b = ((pixel & 0x1) * 0xff);
    return a|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_a1b1g1r1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);
    CARD32  a,r,g,b;

    a = ((pixel & 0x8) * 0xff) << 21;
    r = ((pixel & 0x4) * 0xff) >> 3;
    g = ((pixel & 0x2) * 0xff) << 7;
    b = ((pixel & 0x1) * 0xff) << 16;
    return a|r|g|b;
}

static FASTCALL CARD32
fbFetchPixel_c4 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = Fetch4(bits, offset);

    return indexed->rgba[pixel];
}


static FASTCALL CARD32
fbFetchPixel_a1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32  pixel = ((CARD32 *)bits)[offset >> 5];
    CARD32  a;
#if BITMAP_BIT_ORDER == MSBFirst
    a = pixel >> (0x1f - (offset & 0x1f));
#else
    a = pixel >> (offset & 0x1f);
#endif
    a = a & 1;
    a |= a << 1;
    a |= a << 2;
    a |= a << 4;
    return a << 24;
}

static FASTCALL CARD32
fbFetchPixel_g1 (const FbBits *bits, int offset, miIndexedPtr indexed)
{
    CARD32 pixel = ((CARD32 *)bits)[offset >> 5];
    CARD32 a;
#if BITMAP_BIT_ORDER == MSBFirst
    a = pixel >> (0x1f - (offset & 0x1f));
#else
    a = pixel >> (offset & 0x1f);
#endif
    a = a & 1;
    return indexed->rgba[a];
}

static fetchPixelProc fetchPixelProcForPicture (PicturePtr pict)
{
    switch(pict->format) {
    case PICT_a8r8g8b8: return fbFetchPixel_a8r8g8b8;
    case PICT_x8r8g8b8: return fbFetchPixel_x8r8g8b8;
    case PICT_a8b8g8r8: return fbFetchPixel_a8b8g8r8;
    case PICT_x8b8g8r8: return fbFetchPixel_x8b8g8r8;

        /* 24bpp formats */
    case PICT_r8g8b8: return fbFetchPixel_r8g8b8;
    case PICT_b8g8r8: return fbFetchPixel_b8g8r8;

        /* 16bpp formats */
    case PICT_r5g6b5: return fbFetchPixel_r5g6b5;
    case PICT_b5g6r5: return fbFetchPixel_b5g6r5;

    case PICT_a1r5g5b5: return fbFetchPixel_a1r5g5b5;
    case PICT_x1r5g5b5: return fbFetchPixel_x1r5g5b5;
    case PICT_a1b5g5r5: return fbFetchPixel_a1b5g5r5;
    case PICT_x1b5g5r5: return fbFetchPixel_x1b5g5r5;
    case PICT_a4r4g4b4: return fbFetchPixel_a4r4g4b4;
    case PICT_x4r4g4b4: return fbFetchPixel_x4r4g4b4;
    case PICT_a4b4g4r4: return fbFetchPixel_a4b4g4r4;
    case PICT_x4b4g4r4: return fbFetchPixel_x4b4g4r4;

        /* 8bpp formats */
    case PICT_a8: return  fbFetchPixel_a8;
    case PICT_r3g3b2: return fbFetchPixel_r3g3b2;
    case PICT_b2g3r3: return fbFetchPixel_b2g3r3;
    case PICT_a2r2g2b2: return fbFetchPixel_a2r2g2b2;
    case PICT_a2b2g2r2: return fbFetchPixel_a2b2g2r2;
    case PICT_c8: return  fbFetchPixel_c8;
    case PICT_g8: return  fbFetchPixel_c8;
    case PICT_x4a4: return fbFetchPixel_x4a4;

        /* 4bpp formats */
    case PICT_a4: return  fbFetchPixel_a4;
    case PICT_r1g2b1: return fbFetchPixel_r1g2b1;
    case PICT_b1g2r1: return fbFetchPixel_b1g2r1;
    case PICT_a1r1g1b1: return fbFetchPixel_a1r1g1b1;
    case PICT_a1b1g1r1: return fbFetchPixel_a1b1g1r1;
    case PICT_c4: return  fbFetchPixel_c4;
    case PICT_g4: return  fbFetchPixel_c4;

        /* 1bpp formats */
    case PICT_a1: return  fbFetchPixel_a1;
    case PICT_g1: return  fbFetchPixel_g1;
    }

    return NULL;
}



/*
 * All the store functions
 */

typedef FASTCALL void (*storeProc) (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed);

#define Splita(v)	CARD32	a = ((v) >> 24), r = ((v) >> 16) & 0xff, g = ((v) >> 8) & 0xff, b = (v) & 0xff
#define Split(v)	CARD32	r = ((v) >> 16) & 0xff, g = ((v) >> 8) & 0xff, b = (v) & 0xff

static FASTCALL void
fbStore_a8r8g8b8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    MEMCPY_WRAPPED(((CARD32 *)bits) + x, values, width*sizeof(CARD32));
}

static FASTCALL void
fbStore_x8r8g8b8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD32 *pixel = (CARD32 *)bits + x;
    for (i = 0; i < width; ++i)
        WRITE(pixel++, READ(values + i) & 0xffffff);
}

static FASTCALL void
fbStore_a8b8g8r8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD32 *pixel = (CARD32 *)bits + x;
    for (i = 0; i < width; ++i)
        WRITE(pixel++, (READ(values + i) & 0xff00ff00) | ((READ(values + i) >> 16) & 0xff) | ((READ(values + i) & 0xff) << 16));
}

static FASTCALL void
fbStore_x8b8g8r8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD32 *pixel = (CARD32 *)bits + x;
    for (i = 0; i < width; ++i)
        WRITE(pixel++, (READ(values + i) & 0x0000ff00) | ((READ(values + i) >> 16) & 0xff) | ((READ(values + i) & 0xff) << 16));
}

static FASTCALL void
fbStore_r8g8b8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8 *pixel = ((CARD8 *) bits) + 3*x;
    for (i = 0; i < width; ++i) {
        Store24(pixel, READ(values + i));
        pixel += 3;
    }
}

static FASTCALL void
fbStore_b8g8r8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8 *pixel = ((CARD8 *) bits) + 3*x;
    for (i = 0; i < width; ++i) {
        CARD32 val = READ(values + i);
#if IMAGE_BYTE_ORDER == MSBFirst
        WRITE(pixel++, Blue(val));
        WRITE(pixel++, Green(val));
        WRITE(pixel++, Red(val));
#else
        WRITE(pixel++, Red(val));
        WRITE(pixel++, Green(val));
        WRITE(pixel++, Blue(val));
#endif
    }
}

static FASTCALL void
fbStore_r5g6b5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16 *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(values + i);
        WRITE(pixel++, ((s >> 3) & 0x001f) |
	      ((s >> 5) & 0x07e0) |
	      ((s >> 8) & 0xf800));
    }
}

static FASTCALL void
fbStore_b5g6r5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((b << 8) & 0xf800) |
	      ((g << 3) & 0x07e0) |
	      ((r >> 3)         ));
    }
}

static FASTCALL void
fbStore_a1r5g5b5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Splita(READ(values + i));
        WRITE(pixel++, ((a << 8) & 0x8000) |
	      ((r << 7) & 0x7c00) |
	      ((g << 2) & 0x03e0) |
	      ((b >> 3)         ));
    }
}

static FASTCALL void
fbStore_x1r5g5b5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((r << 7) & 0x7c00) |
	      ((g << 2) & 0x03e0) |
	      ((b >> 3)         ));
    }
}

static FASTCALL void
fbStore_a1b5g5r5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Splita(READ(values + i));
        WRITE(pixel++, ((a << 8) & 0x8000) |
	      ((b << 7) & 0x7c00) |
	      ((g << 2) & 0x03e0) |
	      ((r >> 3)         ));
    }
}

static FASTCALL void
fbStore_x1b5g5r5 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((b << 7) & 0x7c00) |
	      ((g << 2) & 0x03e0) |
	      ((r >> 3)         ));
    }
}

static FASTCALL void
fbStore_a4r4g4b4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Splita(READ(values + i));
        WRITE(pixel++, ((a << 8) & 0xf000) |
	      ((r << 4) & 0x0f00) |
	      ((g     ) & 0x00f0) |
	      ((b >> 4)         ));
    }
}

static FASTCALL void
fbStore_x4r4g4b4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((r << 4) & 0x0f00) |
	      ((g     ) & 0x00f0) |
	      ((b >> 4)         ));
    }
}

static FASTCALL void
fbStore_a4b4g4r4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Splita(READ(values + i));
        WRITE(pixel++, ((a << 8) & 0xf000) |
	      ((b << 4) & 0x0f00) |
	      ((g     ) & 0x00f0) |
	      ((r >> 4)         ));
    }
}

static FASTCALL void
fbStore_x4b4g4r4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD16  *pixel = ((CARD16 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((b << 4) & 0x0f00) |
	      ((g     ) & 0x00f0) |
	      ((r >> 4)         ));
    }
}

static FASTCALL void
fbStore_a8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        WRITE(pixel++, READ(values + i) >> 24);
    }
}

static FASTCALL void
fbStore_r3g3b2 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((r     ) & 0xe0) |
	      ((g >> 3) & 0x1c) |
	      ((b >> 6)       ));
    }
}

static FASTCALL void
fbStore_b2g3r3 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Split(READ(values + i));
        WRITE(pixel++, ((b     ) & 0xe0) |
	      ((g >> 3) & 0x1c) |
	      ((r >> 6)       ));
    }
}

static FASTCALL void
fbStore_a2r2g2b2 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        Splita(READ(values + i));
        WRITE(pixel++, ((a     ) & 0xc0) |
	      ((r >> 2) & 0x30) |
	      ((g >> 4) & 0x0c) |
	      ((b >> 6)       ));
    }
}

static FASTCALL void
fbStore_c8 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        WRITE(pixel++, miIndexToEnt24(indexed,READ(values + i)));
    }
}

static FASTCALL void
fbStore_x4a4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    CARD8   *pixel = ((CARD8 *) bits) + x;
    for (i = 0; i < width; ++i) {
        WRITE(pixel++, READ(values + i) >> 28);
    }
}

#define Store8(l,o,v)  (((CARD8 *) l)[(o) >> 3] = (v))
#if IMAGE_BYTE_ORDER == MSBFirst
#define Store4(l,o,v)  Store8(l,o,((o) & 4 ?			\
				   (Fetch8(l,o) & 0xf0) | (v) :		\
				   (Fetch8(l,o) & 0x0f) | ((v) << 4)))
#else
#define Store4(l,o,v)  Store8(l,o,((o) & 4 ?			       \
				   (Fetch8(l,o) & 0x0f) | ((v) << 4) : \
				   (Fetch8(l,o) & 0xf0) | (v)))
#endif

static FASTCALL void
fbStore_a4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        Store4(bits, i + x, READ(values + i)>>28);
    }
}

static FASTCALL void
fbStore_r1g2b1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  pixel;

        Split(READ(values + i));
        pixel = (((r >> 4) & 0x8) |
                 ((g >> 5) & 0x6) |
                 ((b >> 7)      ));
        Store4(bits, i + x, pixel);
    }
}

static FASTCALL void
fbStore_b1g2r1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  pixel;

        Split(READ(values + i));
        pixel = (((b >> 4) & 0x8) |
                 ((g >> 5) & 0x6) |
                 ((r >> 7)      ));
        Store4(bits, i + x, pixel);
    }
}

static FASTCALL void
fbStore_a1r1g1b1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  pixel;
        Splita(READ(values + i));
        pixel = (((a >> 4) & 0x8) |
                 ((r >> 5) & 0x4) |
                 ((g >> 6) & 0x2) |
                 ((b >> 7)      ));
        Store4(bits, i + x, pixel);
    }
}

static FASTCALL void
fbStore_a1b1g1r1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  pixel;
        Splita(READ(values + i));
        pixel = (((a >> 4) & 0x8) |
                 ((b >> 5) & 0x4) |
                 ((g >> 6) & 0x2) |
                 ((r >> 7)      ));
        Store4(bits, i + x, pixel);
    }
}

static FASTCALL void
fbStore_c4 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  pixel;

        pixel = miIndexToEnt24(indexed, READ(values + i));
        Store4(bits, i + x, pixel);
    }
}

static FASTCALL void
fbStore_a1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  *pixel = ((CARD32 *) bits) + ((i+x) >> 5);
        CARD32  mask = FbStipMask((i+x) & 0x1f, 1);

        CARD32 v = READ(values + i) & 0x80000000 ? mask : 0;
        WRITE(pixel, (READ(pixel) & ~mask) | v);
    }
}

static FASTCALL void
fbStore_g1 (FbBits *bits, const CARD32 *values, int x, int width, miIndexedPtr indexed)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  *pixel = ((CARD32 *) bits) + ((i+x) >> 5);
        CARD32  mask = FbStipMask((i+x) & 0x1f, 1);

        CARD32 v = miIndexToEntY24(indexed,READ(values + i)) ? mask : 0;
        WRITE(pixel, (READ(pixel) & ~mask) | v);
    }
}


static storeProc storeProcForPicture (PicturePtr pict)
{
    switch(pict->format) {
    case PICT_a8r8g8b8: return fbStore_a8r8g8b8;
    case PICT_x8r8g8b8: return fbStore_x8r8g8b8;
    case PICT_a8b8g8r8: return fbStore_a8b8g8r8;
    case PICT_x8b8g8r8: return fbStore_x8b8g8r8;

        /* 24bpp formats */
    case PICT_r8g8b8: return fbStore_r8g8b8;
    case PICT_b8g8r8: return fbStore_b8g8r8;

        /* 16bpp formats */
    case PICT_r5g6b5: return fbStore_r5g6b5;
    case PICT_b5g6r5: return fbStore_b5g6r5;

    case PICT_a1r5g5b5: return fbStore_a1r5g5b5;
    case PICT_x1r5g5b5: return fbStore_x1r5g5b5;
    case PICT_a1b5g5r5: return fbStore_a1b5g5r5;
    case PICT_x1b5g5r5: return fbStore_x1b5g5r5;
    case PICT_a4r4g4b4: return fbStore_a4r4g4b4;
    case PICT_x4r4g4b4: return fbStore_x4r4g4b4;
    case PICT_a4b4g4r4: return fbStore_a4b4g4r4;
    case PICT_x4b4g4r4: return fbStore_x4b4g4r4;

        /* 8bpp formats */
    case PICT_a8: return  fbStore_a8;
    case PICT_r3g3b2: return fbStore_r3g3b2;
    case PICT_b2g3r3: return fbStore_b2g3r3;
    case PICT_a2r2g2b2: return fbStore_a2r2g2b2;
    case PICT_c8: return  fbStore_c8;
    case PICT_g8: return  fbStore_c8;
    case PICT_x4a4: return fbStore_x4a4;

        /* 4bpp formats */
    case PICT_a4: return  fbStore_a4;
    case PICT_r1g2b1: return fbStore_r1g2b1;
    case PICT_b1g2r1: return fbStore_b1g2r1;
    case PICT_a1r1g1b1: return fbStore_a1r1g1b1;
    case PICT_a1b1g1r1: return fbStore_a1b1g1r1;
    case PICT_c4: return  fbStore_c4;
    case PICT_g4: return  fbStore_c4;

        /* 1bpp formats */
    case PICT_a1: return  fbStore_a1;
    case PICT_g1: return  fbStore_g1;
    default:
        return NULL;
    }
}


/*
 * Combine src and mask
 */
static FASTCALL void
fbCombineMaskU (CARD32 *src, const CARD32 *mask, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 a = READ(mask + i) >> 24;
        CARD32 s = READ(src + i);
        FbByteMul(s, a);
        WRITE(src + i, s);
    }
}

/*
 * All of the composing functions
 */

static FASTCALL void
fbCombineClear (CARD32 *dest, const CARD32 *src, int width)
{
    MEMSET_WRAPPED(dest, 0, width*sizeof(CARD32));
}

static FASTCALL void
fbCombineSrcU (CARD32 *dest, const CARD32 *src, int width)
{
    MEMCPY_WRAPPED(dest, src, width*sizeof(CARD32));
}


static FASTCALL void
fbCombineOverU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 ia = Alpha(~s);

        FbByteMulAdd(d, ia, s);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineOverReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 ia = Alpha(~READ(dest + i));
        FbByteMulAdd(s, ia, d);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineInU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 a = Alpha(READ(dest + i));
        FbByteMul(s, a);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineInReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD32 a = Alpha(READ(src + i));
        FbByteMul(d, a);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineOutU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 a = Alpha(~READ(dest + i));
        FbByteMul(s, a);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineOutReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD32 a = Alpha(~READ(src + i));
        FbByteMul(d, a);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineAtopU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 dest_a = Alpha(d);
        CARD32 src_ia = Alpha(~s);

        FbByteAddMul(s, dest_a, d, src_ia);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineAtopReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 src_a = Alpha(s);
        CARD32 dest_ia = Alpha(~d);

        FbByteAddMul(s, dest_ia, d, src_a);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineXorU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 src_ia = Alpha(~s);
        CARD32 dest_ia = Alpha(~d);

        FbByteAddMul(s, dest_ia, d, src_ia);
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineAddU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        FbByteAdd(d, s);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineSaturateU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD16  sa, da;

        sa = s >> 24;
        da = ~d >> 24;
        if (sa > da)
        {
            sa = FbIntDiv(da, sa);
            FbByteMul(s, sa);
        }
        FbByteAdd(d, s);
        WRITE(dest + i, d);
    }
}

/*
 * All of the disjoint composing functions

 The four entries in the first column indicate what source contributions
 come from each of the four areas of the picture -- areas covered by neither
 A nor B, areas covered only by A, areas covered only by B and finally
 areas covered by both A and B.

 Disjoint			Conjoint
 Fa		Fb		Fa		Fb
 (0,0,0,0)	0		0		0		0
 (0,A,0,A)	1		0		1		0
 (0,0,B,B)	0		1		0		1
 (0,A,B,A)	1		min((1-a)/b,1)	1		max(1-a/b,0)
 (0,A,B,B)	min((1-b)/a,1)	1		max(1-b/a,0)	1
 (0,0,0,A)	max(1-(1-b)/a,0) 0		min(1,b/a)	0
 (0,0,0,B)	0		max(1-(1-a)/b,0) 0		min(a/b,1)
 (0,A,0,0)	min(1,(1-b)/a)	0		max(1-b/a,0)	0
 (0,0,B,0)	0		min(1,(1-a)/b)	0		max(1-a/b,0)
 (0,0,B,A)	max(1-(1-b)/a,0) min(1,(1-a)/b)	 min(1,b/a)	max(1-a/b,0)
 (0,A,0,B)	min(1,(1-b)/a)	max(1-(1-a)/b,0) max(1-b/a,0)	min(1,a/b)
 (0,A,B,0)	min(1,(1-b)/a)	min(1,(1-a)/b)	max(1-b/a,0)	max(1-a/b,0)

*/

#define CombineAOut 1
#define CombineAIn  2
#define CombineBOut 4
#define CombineBIn  8

#define CombineClear	0
#define CombineA	(CombineAOut|CombineAIn)
#define CombineB	(CombineBOut|CombineBIn)
#define CombineAOver	(CombineAOut|CombineBOut|CombineAIn)
#define CombineBOver	(CombineAOut|CombineBOut|CombineBIn)
#define CombineAAtop	(CombineBOut|CombineAIn)
#define CombineBAtop	(CombineAOut|CombineBIn)
#define CombineXor	(CombineAOut|CombineBOut)

/* portion covered by a but not b */
static INLINE CARD8
fbCombineDisjointOutPart (CARD8 a, CARD8 b)
{
    /* min (1, (1-b) / a) */

    b = ~b;		    /* 1 - b */
    if (b >= a)		    /* 1 - b >= a -> (1-b)/a >= 1 */
	return 0xff;	    /* 1 */
    return FbIntDiv(b,a);   /* (1-b) / a */
}

/* portion covered by both a and b */
static INLINE CARD8
fbCombineDisjointInPart (CARD8 a, CARD8 b)
{
    /* max (1-(1-b)/a,0) */
    /*  = - min ((1-b)/a - 1, 0) */
    /*  = 1 - min (1, (1-b)/a) */

    b = ~b;		    /* 1 - b */
    if (b >= a)		    /* 1 - b >= a -> (1-b)/a >= 1 */
	return 0;	    /* 1 - 1 */
    return ~FbIntDiv(b,a);  /* 1 - (1-b) / a */
}

static FASTCALL void
fbCombineDisjointGeneralU (CARD32 *dest, const CARD32 *src, int width, CARD8 combine)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32 m,n,o,p;
        CARD16 Fa, Fb, t, u, v;
        CARD8 sa = s >> 24;
        CARD8 da = d >> 24;

        switch (combine & CombineA) {
        default:
            Fa = 0;
            break;
        case CombineAOut:
            Fa = fbCombineDisjointOutPart (sa, da);
            break;
        case CombineAIn:
            Fa = fbCombineDisjointInPart (sa, da);
            break;
        case CombineA:
            Fa = 0xff;
            break;
        }

        switch (combine & CombineB) {
        default:
            Fb = 0;
            break;
        case CombineBOut:
            Fb = fbCombineDisjointOutPart (da, sa);
            break;
        case CombineBIn:
            Fb = fbCombineDisjointInPart (da, sa);
            break;
        case CombineB:
            Fb = 0xff;
            break;
        }
        m = FbGen (s,d,0,Fa,Fb,t, u, v);
        n = FbGen (s,d,8,Fa,Fb,t, u, v);
        o = FbGen (s,d,16,Fa,Fb,t, u, v);
        p = FbGen (s,d,24,Fa,Fb,t, u, v);
        s = m|n|o|p;
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineDisjointOverU (CARD32 *dest, const CARD32 *src, int width)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  s = READ(src + i);
        CARD16  a = s >> 24;

        if (a != 0x00)
        {
            if (a != 0xff)
            {
                CARD32 d = READ(dest + i);
                a = fbCombineDisjointOutPart (d >> 24, a);
                FbByteMulAdd(d, a, s);
                s = d;
            }
            WRITE(dest + i, s);
        }
    }
}

static FASTCALL void
fbCombineDisjointInU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineAIn);
}

static FASTCALL void
fbCombineDisjointInReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineBIn);
}

static FASTCALL void
fbCombineDisjointOutU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineAOut);
}

static FASTCALL void
fbCombineDisjointOutReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineBOut);
}

static FASTCALL void
fbCombineDisjointAtopU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineAAtop);
}

static FASTCALL void
fbCombineDisjointAtopReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineBAtop);
}

static FASTCALL void
fbCombineDisjointXorU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineDisjointGeneralU (dest, src, width, CombineXor);
}

/* portion covered by a but not b */
static INLINE CARD8
fbCombineConjointOutPart (CARD8 a, CARD8 b)
{
    /* max (1-b/a,0) */
    /* = 1-min(b/a,1) */

    /* min (1, (1-b) / a) */

    if (b >= a)		    /* b >= a -> b/a >= 1 */
	return 0x00;	    /* 0 */
    return ~FbIntDiv(b,a);   /* 1 - b/a */
}

/* portion covered by both a and b */
static INLINE CARD8
fbCombineConjointInPart (CARD8 a, CARD8 b)
{
    /* min (1,b/a) */

    if (b >= a)		    /* b >= a -> b/a >= 1 */
	return 0xff;	    /* 1 */
    return FbIntDiv(b,a);   /* b/a */
}

static FASTCALL void
fbCombineConjointGeneralU (CARD32 *dest, const CARD32 *src, int width, CARD8 combine)
{
    int i;
    for (i = 0; i < width; ++i) {
        CARD32  s = READ(src + i);
        CARD32 d = READ(dest + i);
        CARD32  m,n,o,p;
        CARD16  Fa, Fb, t, u, v;
        CARD8 sa = s >> 24;
        CARD8 da = d >> 24;

        switch (combine & CombineA) {
        default:
            Fa = 0;
            break;
        case CombineAOut:
            Fa = fbCombineConjointOutPart (sa, da);
            break;
        case CombineAIn:
            Fa = fbCombineConjointInPart (sa, da);
            break;
        case CombineA:
            Fa = 0xff;
            break;
        }

        switch (combine & CombineB) {
        default:
            Fb = 0;
            break;
        case CombineBOut:
            Fb = fbCombineConjointOutPart (da, sa);
            break;
        case CombineBIn:
            Fb = fbCombineConjointInPart (da, sa);
            break;
        case CombineB:
            Fb = 0xff;
            break;
        }
        m = FbGen (s,d,0,Fa,Fb,t, u, v);
        n = FbGen (s,d,8,Fa,Fb,t, u, v);
        o = FbGen (s,d,16,Fa,Fb,t, u, v);
        p = FbGen (s,d,24,Fa,Fb,t, u, v);
        s = m|n|o|p;
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineConjointOverU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineAOver);
}


static FASTCALL void
fbCombineConjointOverReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineBOver);
}


static FASTCALL void
fbCombineConjointInU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineAIn);
}


static FASTCALL void
fbCombineConjointInReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineBIn);
}

static FASTCALL void
fbCombineConjointOutU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineAOut);
}

static FASTCALL void
fbCombineConjointOutReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineBOut);
}

static FASTCALL void
fbCombineConjointAtopU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineAAtop);
}

static FASTCALL void
fbCombineConjointAtopReverseU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineBAtop);
}

static FASTCALL void
fbCombineConjointXorU (CARD32 *dest, const CARD32 *src, int width)
{
    fbCombineConjointGeneralU (dest, src, width, CombineXor);
}

static CombineFuncU fbCombineFuncU[] = {
    fbCombineClear,
    fbCombineSrcU,
    NULL, /* CombineDst */
    fbCombineOverU,
    fbCombineOverReverseU,
    fbCombineInU,
    fbCombineInReverseU,
    fbCombineOutU,
    fbCombineOutReverseU,
    fbCombineAtopU,
    fbCombineAtopReverseU,
    fbCombineXorU,
    fbCombineAddU,
    fbCombineSaturateU,
    NULL,
    NULL,
    fbCombineClear,
    fbCombineSrcU,
    NULL, /* CombineDst */
    fbCombineDisjointOverU,
    fbCombineSaturateU, /* DisjointOverReverse */
    fbCombineDisjointInU,
    fbCombineDisjointInReverseU,
    fbCombineDisjointOutU,
    fbCombineDisjointOutReverseU,
    fbCombineDisjointAtopU,
    fbCombineDisjointAtopReverseU,
    fbCombineDisjointXorU,
    NULL,
    NULL,
    NULL,
    NULL,
    fbCombineClear,
    fbCombineSrcU,
    NULL, /* CombineDst */
    fbCombineConjointOverU,
    fbCombineConjointOverReverseU,
    fbCombineConjointInU,
    fbCombineConjointInReverseU,
    fbCombineConjointOutU,
    fbCombineConjointOutReverseU,
    fbCombineConjointAtopU,
    fbCombineConjointAtopReverseU,
    fbCombineConjointXorU,
};

static INLINE void
fbCombineMaskC (CARD32 *src, CARD32 *mask)
{
    CARD32 a = *mask;

    CARD32	x;
    CARD16	xa;

    if (!a)
    {
	WRITE(src, 0);
	return;
    }

    x = READ(src);
    if (a == 0xffffffff)
    {
	x = x >> 24;
	x |= x << 8;
	x |= x << 16;
	WRITE(mask, x);
	return;
    }

    xa = x >> 24;
    FbByteMulC(x, a);
    WRITE(src, x);
    FbByteMul(a, xa);
    WRITE(mask, a);
}

static INLINE void
fbCombineMaskValueC (CARD32 *src, const CARD32 *mask)
{
    CARD32 a = READ(mask);
    CARD32	x;

    if (!a)
    {
	WRITE(src, 0);
	return;
    }

    if (a == 0xffffffff)
	return;

    x = READ(src);
    FbByteMulC(x, a);
    WRITE(src,x);
}

static INLINE void
fbCombineMaskAlphaC (const CARD32 *src, CARD32 *mask)
{
    CARD32 a = READ(mask);
    CARD32	x;

    if (!a)
	return;

    x = READ(src) >> 24;
    if (x == 0xff)
	return;
    if (a == 0xffffffff)
    {
	x = x >> 24;
	x |= x << 8;
	x |= x << 16;
	WRITE(mask, x);
	return;
    }

    FbByteMul(a, x);
    WRITE(mask, a);
}

static FASTCALL void
fbCombineClearC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    MEMSET_WRAPPED(dest, 0, width*sizeof(CARD32));
}

static FASTCALL void
fbCombineSrcC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
	CARD32 s = READ(src + i);
	CARD32 m = READ(mask + i);

	fbCombineMaskValueC (&s, &m);

	WRITE(dest, s);
    }
}

static FASTCALL void
fbCombineOverC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
	CARD32 s = READ(src + i);
	CARD32 m = READ(mask + i);
	CARD32 a;

	fbCombineMaskC (&s, &m);

	a = ~m;
        if (a != 0xffffffff)
        {
            if (a)
            {
                CARD32 d = READ(dest + i);
                FbByteMulAddC(d, a, s);
                s = d;
            }
            WRITE(dest + i, s);
        }
    }
}

static FASTCALL void
fbCombineOverReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD32 a = ~d >> 24;

        if (a)
        {
            CARD32 s = READ(src + i);
	    CARD32 m = READ(mask + i);

	    fbCombineMaskValueC (&s, &m);

            if (a != 0xff)
            {
                FbByteMulAdd(s, a, d);
            }
            WRITE(dest + i, s);
        }
    }
}

static FASTCALL void
fbCombineInC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD16 a = d >> 24;
        CARD32 s = 0;
        if (a)
        {
	    CARD32 m = READ(mask + i);

	    s = READ(src + i);
	    fbCombineMaskValueC (&s, &m);
            if (a != 0xff)
            {
                FbByteMul(s, a);
            }
        }
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineInReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 m = READ(mask + i);
        CARD32 a;

	fbCombineMaskAlphaC (&s, &m);

	a = m;
        if (a != 0xffffffff)
        {
            CARD32 d = 0;
            if (a)
            {
                d = READ(dest + i);
                FbByteMulC(d, a);
            }
            WRITE(dest + i, d); 
        }
    }
}

static FASTCALL void
fbCombineOutC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD16 a = ~d >> 24;
        CARD32 s = 0;
        if (a)
        {
	    CARD32 m = READ(mask + i);

	    s = READ(src + i);
	    fbCombineMaskValueC (&s, &m);

            if (a != 0xff)
            {
                FbByteMul(s, a);
            }
        }
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineOutReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
	CARD32 s = READ(src + i);
	CARD32 m = READ(mask + i);
	CARD32 a;

	fbCombineMaskAlphaC (&s, &m);

        a = ~m;
        if (a != 0xffffffff)
        {
            CARD32 d = 0;
            if (a)
            {
                d = READ(dest + i);
                FbByteMulC(d, a);
            }
            WRITE(dest + i, d);
        }
    }
}

static FASTCALL void
fbCombineAtopC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD32 s = READ(src + i);
        CARD32 m = READ(mask + i);
        CARD32 ad;
        CARD16 as = d >> 24;

	fbCombineMaskC (&s, &m);

        ad = ~m;

        FbByteAddMulC(d, ad, s, as);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineAtopReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {

        CARD32 d = READ(dest + i);
        CARD32 s = READ(src + i);
        CARD32 m = READ(mask + i);
        CARD32 ad;
        CARD16 as = ~d >> 24;

	fbCombineMaskC (&s, &m);

	ad = m;

        FbByteAddMulC(d, ad, s, as);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineXorC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 d = READ(dest + i);
        CARD32 s = READ(src + i);
        CARD32 m = READ(mask + i);
        CARD32 ad;
        CARD16 as = ~d >> 24;

	fbCombineMaskC (&s, &m);

	ad = ~m;

        FbByteAddMulC(d, ad, s, as);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineAddC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32 s = READ(src + i);
        CARD32 m = READ(mask + i);
        CARD32 d = READ(dest + i);

	fbCombineMaskValueC (&s, &m);

        FbByteAdd(d, s);
        WRITE(dest + i, d);
    }
}

static FASTCALL void
fbCombineSaturateC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32  s, d;
        CARD16  sa, sr, sg, sb, da;
        CARD16  t, u, v;
        CARD32  m,n,o,p;

        d = READ(dest + i);
        s = READ(src + i);
	m = READ(mask + i);

	fbCombineMaskC (&s, &m);

        sa = (m >> 24);
        sr = (m >> 16) & 0xff;
        sg = (m >>  8) & 0xff;
        sb = (m      ) & 0xff;
        da = ~d >> 24;

        if (sb <= da)
            m = FbAdd(s,d,0,t);
        else
            m = FbGen (s, d, 0, (da << 8) / sb, 0xff, t, u, v);

        if (sg <= da)
            n = FbAdd(s,d,8,t);
        else
            n = FbGen (s, d, 8, (da << 8) / sg, 0xff, t, u, v);

        if (sr <= da)
            o = FbAdd(s,d,16,t);
        else
            o = FbGen (s, d, 16, (da << 8) / sr, 0xff, t, u, v);

        if (sa <= da)
            p = FbAdd(s,d,24,t);
        else
            p = FbGen (s, d, 24, (da << 8) / sa, 0xff, t, u, v);

        WRITE(dest + i, m|n|o|p);
    }
}

static FASTCALL void
fbCombineDisjointGeneralC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width, CARD8 combine)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32  s, d;
        CARD32  m,n,o,p;
        CARD32  Fa, Fb;
        CARD16  t, u, v;
        CARD32  sa;
        CARD8   da;

        s = READ(src + i);
        m = READ(mask + i);
        d = READ(dest + i);
        da = d >> 24;

	fbCombineMaskC (&s, &m);

	sa = m;

        switch (combine & CombineA) {
        default:
            Fa = 0;
            break;
        case CombineAOut:
            m = fbCombineDisjointOutPart ((CARD8) (sa >> 0), da);
            n = fbCombineDisjointOutPart ((CARD8) (sa >> 8), da) << 8;
            o = fbCombineDisjointOutPart ((CARD8) (sa >> 16), da) << 16;
            p = fbCombineDisjointOutPart ((CARD8) (sa >> 24), da) << 24;
            Fa = m|n|o|p;
            break;
        case CombineAIn:
            m = fbCombineDisjointInPart ((CARD8) (sa >> 0), da);
            n = fbCombineDisjointInPart ((CARD8) (sa >> 8), da) << 8;
            o = fbCombineDisjointInPart ((CARD8) (sa >> 16), da) << 16;
            p = fbCombineDisjointInPart ((CARD8) (sa >> 24), da) << 24;
            Fa = m|n|o|p;
            break;
        case CombineA:
            Fa = 0xffffffff;
            break;
        }

        switch (combine & CombineB) {
        default:
            Fb = 0;
            break;
        case CombineBOut:
            m = fbCombineDisjointOutPart (da, (CARD8) (sa >> 0));
            n = fbCombineDisjointOutPart (da, (CARD8) (sa >> 8)) << 8;
            o = fbCombineDisjointOutPart (da, (CARD8) (sa >> 16)) << 16;
            p = fbCombineDisjointOutPart (da, (CARD8) (sa >> 24)) << 24;
            Fb = m|n|o|p;
            break;
        case CombineBIn:
            m = fbCombineDisjointInPart (da, (CARD8) (sa >> 0));
            n = fbCombineDisjointInPart (da, (CARD8) (sa >> 8)) << 8;
            o = fbCombineDisjointInPart (da, (CARD8) (sa >> 16)) << 16;
            p = fbCombineDisjointInPart (da, (CARD8) (sa >> 24)) << 24;
            Fb = m|n|o|p;
            break;
        case CombineB:
            Fb = 0xffffffff;
            break;
        }
        m = FbGen (s,d,0,FbGet8(Fa,0),FbGet8(Fb,0),t, u, v);
        n = FbGen (s,d,8,FbGet8(Fa,8),FbGet8(Fb,8),t, u, v);
        o = FbGen (s,d,16,FbGet8(Fa,16),FbGet8(Fb,16),t, u, v);
        p = FbGen (s,d,24,FbGet8(Fa,24),FbGet8(Fb,24),t, u, v);
        s = m|n|o|p;
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineDisjointOverC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineAOver);
}

static FASTCALL void
fbCombineDisjointInC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineAIn);
}

static FASTCALL void
fbCombineDisjointInReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineBIn);
}

static FASTCALL void
fbCombineDisjointOutC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineAOut);
}

static FASTCALL void
fbCombineDisjointOutReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineBOut);
}

static FASTCALL void
fbCombineDisjointAtopC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineAAtop);
}

static FASTCALL void
fbCombineDisjointAtopReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineBAtop);
}

static FASTCALL void
fbCombineDisjointXorC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineDisjointGeneralC (dest, src, mask, width, CombineXor);
}

static FASTCALL void
fbCombineConjointGeneralC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width, CARD8 combine)
{
    int i;

    for (i = 0; i < width; ++i) {
        CARD32  s, d;
        CARD32  m,n,o,p;
        CARD32  Fa, Fb;
        CARD16  t, u, v;
        CARD32  sa;
        CARD8   da;

        s = READ(src + i);
        m = READ(mask + i);
        d = READ(dest + i);
        da = d >> 24;

	fbCombineMaskC (&s, &m);

        sa = m;

        switch (combine & CombineA) {
        default:
            Fa = 0;
            break;
        case CombineAOut:
            m = fbCombineConjointOutPart ((CARD8) (sa >> 0), da);
            n = fbCombineConjointOutPart ((CARD8) (sa >> 8), da) << 8;
            o = fbCombineConjointOutPart ((CARD8) (sa >> 16), da) << 16;
            p = fbCombineConjointOutPart ((CARD8) (sa >> 24), da) << 24;
            Fa = m|n|o|p;
            break;
        case CombineAIn:
            m = fbCombineConjointInPart ((CARD8) (sa >> 0), da);
            n = fbCombineConjointInPart ((CARD8) (sa >> 8), da) << 8;
            o = fbCombineConjointInPart ((CARD8) (sa >> 16), da) << 16;
            p = fbCombineConjointInPart ((CARD8) (sa >> 24), da) << 24;
            Fa = m|n|o|p;
            break;
        case CombineA:
            Fa = 0xffffffff;
            break;
        }

        switch (combine & CombineB) {
        default:
            Fb = 0;
            break;
        case CombineBOut:
            m = fbCombineConjointOutPart (da, (CARD8) (sa >> 0));
            n = fbCombineConjointOutPart (da, (CARD8) (sa >> 8)) << 8;
            o = fbCombineConjointOutPart (da, (CARD8) (sa >> 16)) << 16;
            p = fbCombineConjointOutPart (da, (CARD8) (sa >> 24)) << 24;
            Fb = m|n|o|p;
            break;
        case CombineBIn:
            m = fbCombineConjointInPart (da, (CARD8) (sa >> 0));
            n = fbCombineConjointInPart (da, (CARD8) (sa >> 8)) << 8;
            o = fbCombineConjointInPart (da, (CARD8) (sa >> 16)) << 16;
            p = fbCombineConjointInPart (da, (CARD8) (sa >> 24)) << 24;
            Fb = m|n|o|p;
            break;
        case CombineB:
            Fb = 0xffffffff;
            break;
        }
        m = FbGen (s,d,0,FbGet8(Fa,0),FbGet8(Fb,0),t, u, v);
        n = FbGen (s,d,8,FbGet8(Fa,8),FbGet8(Fb,8),t, u, v);
        o = FbGen (s,d,16,FbGet8(Fa,16),FbGet8(Fb,16),t, u, v);
        p = FbGen (s,d,24,FbGet8(Fa,24),FbGet8(Fb,24),t, u, v);
        s = m|n|o|p;
        WRITE(dest + i, s);
    }
}

static FASTCALL void
fbCombineConjointOverC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineAOver);
}

static FASTCALL void
fbCombineConjointOverReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineBOver);
}

static FASTCALL void
fbCombineConjointInC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineAIn);
}

static FASTCALL void
fbCombineConjointInReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineBIn);
}

static FASTCALL void
fbCombineConjointOutC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineAOut);
}

static FASTCALL void
fbCombineConjointOutReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineBOut);
}

static FASTCALL void
fbCombineConjointAtopC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineAAtop);
}

static FASTCALL void
fbCombineConjointAtopReverseC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineBAtop);
}

static FASTCALL void
fbCombineConjointXorC (CARD32 *dest, CARD32 *src, CARD32 *mask, int width)
{
    fbCombineConjointGeneralC (dest, src, mask, width, CombineXor);
}

static CombineFuncC fbCombineFuncC[] = {
    fbCombineClearC,
    fbCombineSrcC,
    NULL, /* Dest */
    fbCombineOverC,
    fbCombineOverReverseC,
    fbCombineInC,
    fbCombineInReverseC,
    fbCombineOutC,
    fbCombineOutReverseC,
    fbCombineAtopC,
    fbCombineAtopReverseC,
    fbCombineXorC,
    fbCombineAddC,
    fbCombineSaturateC,
    NULL,
    NULL,
    fbCombineClearC,	    /* 0x10 */
    fbCombineSrcC,
    NULL, /* Dest */
    fbCombineDisjointOverC,
    fbCombineSaturateC, /* DisjointOverReverse */
    fbCombineDisjointInC,
    fbCombineDisjointInReverseC,
    fbCombineDisjointOutC,
    fbCombineDisjointOutReverseC,
    fbCombineDisjointAtopC,
    fbCombineDisjointAtopReverseC,
    fbCombineDisjointXorC,  /* 0x1b */
    NULL,
    NULL,
    NULL,
    NULL,
    fbCombineClearC,
    fbCombineSrcC,
    NULL, /* Dest */
    fbCombineConjointOverC,
    fbCombineConjointOverReverseC,
    fbCombineConjointInC,
    fbCombineConjointInReverseC,
    fbCombineConjointOutC,
    fbCombineConjointOutReverseC,
    fbCombineConjointAtopC,
    fbCombineConjointAtopReverseC,
    fbCombineConjointXorC,
};


FbComposeFunctions composeFunctions = {
    fbCombineFuncU,
    fbCombineFuncC,
    fbCombineMaskU
};


static void fbFetchSolid(PicturePtr pict, int x, int y, int width, CARD32 *buffer, CARD32 *mask, CARD32 maskBits)
{
    FbBits *bits;
    FbStride stride;
    int bpp;
    int xoff, yoff;
    CARD32 color;
    CARD32 *end;
    fetchPixelProc fetch = fetchPixelProcForPicture(pict);
    miIndexedPtr indexed = (miIndexedPtr) pict->pFormat->index.devPrivate;

    fbGetDrawable (pict->pDrawable, bits, stride, bpp, xoff, yoff);
    bits += yoff*stride + (xoff*bpp >> FB_SHIFT);

    color = fetch(bits, 0, indexed);

    end = buffer + width;
    while (buffer < end)
        WRITE(buffer++, color);
    fbFinishAccess (pict->pDrawable);
}

static void fbFetch(PicturePtr pict, int x, int y, int width, CARD32 *buffer, CARD32 *mask, CARD32 maskBits)
{
    FbBits *bits;
    FbStride stride;
    int bpp;
    int xoff, yoff;
    fetchProc fetch = fetchProcForPicture(pict);
    miIndexedPtr indexed = (miIndexedPtr) pict->pFormat->index.devPrivate;

    fbGetDrawable (pict->pDrawable, bits, stride, bpp, xoff, yoff);
    x += xoff;
    y += yoff;

    bits += y*stride;

    fetch(bits, x, width, buffer, indexed);
    fbFinishAccess (pict->pDrawable);
}

#define MOD(a,b) ((a) < 0 ? ((b) - ((-(a) - 1) % (b))) - 1 : (a) % (b))
#define DIV(a,b) ((((a) < 0) == ((b) < 0)) ? (a) / (b) :		\
		  ((a) - (b) + 1 - (((b) < 0) << 1)) / (b))

typedef struct
{
    CARD32        left_ag;
    CARD32        left_rb;
    CARD32        right_ag;
    CARD32        right_rb;
    int32_t       left_x;
    int32_t       right_x;
    int32_t       stepper;
    
    PictGradientStopPtr      stops;
    int                      num_stops;
    unsigned int             spread;

    int		  need_reset;
} GradientWalker;

static void
_gradient_walker_init (GradientWalker  *walker,
		       SourcePictPtr    pGradient,
		       unsigned int     spread)
{
    walker->num_stops = pGradient->gradient.nstops;
    walker->stops     = pGradient->gradient.stops;
    walker->left_x    = 0;
    walker->right_x   = 0x10000;
    walker->stepper   = 0;
    walker->left_ag   = 0;
    walker->left_rb   = 0;
    walker->right_ag  = 0;
    walker->right_rb  = 0;
    walker->spread    = spread;

    walker->need_reset = TRUE;
}

static void
_gradient_walker_reset (GradientWalker  *walker,
                        xFixed_32_32     pos)
{
    int32_t                  x, left_x, right_x;
    xRenderColor          *left_c, *right_c;
    int                      n, count = walker->num_stops;
    PictGradientStopPtr      stops = walker->stops;
    
    static const xRenderColor   transparent_black = { 0, 0, 0, 0 };
    
    switch (walker->spread)
    {
    case RepeatNormal:
	x = (int32_t)pos & 0xFFFF;
	for (n = 0; n < count; n++)
	    if (x < stops[n].x)
		break;
	if (n == 0) {
	    left_x =  stops[count-1].x - 0x10000;
	    left_c = &stops[count-1].color;
	} else {
	    left_x =  stops[n-1].x;
	    left_c = &stops[n-1].color;
	}
	
	if (n == count) {
	    right_x =  stops[0].x + 0x10000;
	    right_c = &stops[0].color;
	} else {
	    right_x =  stops[n].x;
	    right_c = &stops[n].color;
	}
	left_x  += (pos - x);
	right_x += (pos - x);
	break;
	
    case RepeatPad:
	for (n = 0; n < count; n++)
	    if (pos < stops[n].x)
		break;
	
	if (n == 0) {
	    left_x =  INT_MIN;
	    left_c = &stops[0].color;
	} else {
	    left_x =  stops[n-1].x;
	    left_c = &stops[n-1].color;
	}
	
	if (n == count) {
	    right_x =  INT_MAX;
	    right_c = &stops[n-1].color;
	} else {
	    right_x =  stops[n].x;
	    right_c = &stops[n].color;
	}
	break;
	
    case RepeatReflect:
	x = (int32_t)pos & 0xFFFF;
	if ((int32_t)pos & 0x10000)
	    x = 0x10000 - x;
	for (n = 0; n < count; n++)
	    if (x < stops[n].x)
		break;
	
	if (n == 0) {
	    left_x =  -stops[0].x;
	    left_c = &stops[0].color;
	} else {
	    left_x =  stops[n-1].x;
	    left_c = &stops[n-1].color;
	}
	
	if (n == count) {
	    right_x = 0x20000 - stops[n-1].x;
	    right_c = &stops[n-1].color;
	} else {
	    right_x =  stops[n].x;
	    right_c = &stops[n].color;
	}
	
	if ((int32_t)pos & 0x10000) {
	    xRenderColor  *tmp_c;
	    int32_t          tmp_x;
	    
	    tmp_x   = 0x10000 - right_x;
	    right_x = 0x10000 - left_x;
	    left_x  = tmp_x;
	    
	    tmp_c   = right_c;
	    right_c = left_c;
	    left_c  = tmp_c;

	    x = 0x10000 - x;
	}
	left_x  += (pos - x);
	right_x += (pos - x);
	break;
	
    default:  /* RepeatNone */
	for (n = 0; n < count; n++)
	    if (pos < stops[n].x)
		break;
	
	if (n == 0)
	{
	    left_x  =  INT_MIN;
	    right_x =  stops[0].x;
	    left_c  = right_c = (xRenderColor*) &transparent_black;
	}
	else if (n == count)
	{
	    left_x  = stops[n-1].x;
	    right_x = INT_MAX;
	    left_c  = right_c = (xRenderColor*) &transparent_black;
	}
	else
	{
	    left_x  =  stops[n-1].x;
	    right_x =  stops[n].x;
	    left_c  = &stops[n-1].color;
	    right_c = &stops[n].color;
	}
    }
    
    walker->left_x   = left_x;
    walker->right_x  = right_x;
    walker->left_ag  = ((left_c->alpha >> 8) << 16)   | (left_c->green >> 8);
    walker->left_rb  = ((left_c->red & 0xff00) << 8)  | (left_c->blue >> 8);
    walker->right_ag = ((right_c->alpha >> 8) << 16)  | (right_c->green >> 8);
    walker->right_rb = ((right_c->red & 0xff00) << 8) | (right_c->blue >> 8);
    
    if ( walker->left_x == walker->right_x                ||
	 ( walker->left_ag == walker->right_ag &&
	   walker->left_rb == walker->right_rb )   )
    {
	walker->stepper = 0;
    }
    else
    {
	int32_t width = right_x - left_x;
	walker->stepper = ((1 << 24) + width/2)/width;
    }

    walker->need_reset = FALSE;
}

#define  GRADIENT_WALKER_NEED_RESET(w,x)				\
    ( (w)->need_reset || (x) < (w)->left_x || (x) >= (w)->right_x)


/* the following assumes that GRADIENT_WALKER_NEED_RESET(w,x) is FALSE */
static CARD32
_gradient_walker_pixel (GradientWalker  *walker,
                        xFixed_32_32     x)
{
    int  dist, idist;
    CARD32  t1, t2, a, color;
    
    if (GRADIENT_WALKER_NEED_RESET (walker, x))
        _gradient_walker_reset (walker, x);
    
    dist  = ((int)(x - walker->left_x)*walker->stepper) >> 16;
    idist = 256 - dist;
    
    /* combined INTERPOLATE and premultiply */
    t1 = walker->left_rb*idist + walker->right_rb*dist;
    t1 = (t1 >> 8) & 0xff00ff;
    
    t2  = walker->left_ag*idist + walker->right_ag*dist;
    t2 &= 0xff00ff00;
    
    color = t2 & 0xff000000;
    a     = t2 >> 24;
    
    t1  = t1*a + 0x800080;
    t1  = (t1 + ((t1 >> 8) & 0xff00ff)) >> 8;
    
    t2  = (t2 >> 8)*a + 0x800080;
    t2  = (t2 + ((t2 >> 8) & 0xff00ff));
    
    return (color | (t1 & 0xff00ff) | (t2 & 0xff00));
}

static void fbFetchSourcePict(PicturePtr pict, int x, int y, int width, CARD32 *buffer, CARD32 *mask, CARD32 maskBits)
{
     SourcePictPtr   pGradient = pict->pSourcePict;
     GradientWalker  walker;
     CARD32         *end = buffer + width;
 
     _gradient_walker_init (&walker, pGradient, pict->repeat);
    
    if (pGradient->type == SourcePictTypeSolidFill) {
        register CARD32 color = pGradient->solidFill.color;
        while (buffer < end) {
            WRITE(buffer++, color);
        }
    } else if (pGradient->type == SourcePictTypeLinear) {
        PictVector v, unit;
        xFixed_32_32 l;
        xFixed_48_16 dx, dy, a, b, off;
	
        /* reference point is the center of the pixel */
        v.vector[0] = IntToxFixed(x) + xFixed1/2;
        v.vector[1] = IntToxFixed(y) + xFixed1/2;
        v.vector[2] = xFixed1;
        if (pict->transform) {
            if (!PictureTransformPoint3d (pict->transform, &v))
                return;
            unit.vector[0] = pict->transform->matrix[0][0];
            unit.vector[1] = pict->transform->matrix[1][0];
            unit.vector[2] = pict->transform->matrix[2][0];
        } else {
            unit.vector[0] = xFixed1;
            unit.vector[1] = 0;
            unit.vector[2] = 0;
        }

        dx = pGradient->linear.p2.x - pGradient->linear.p1.x;
        dy = pGradient->linear.p2.y - pGradient->linear.p1.y;
        l = dx*dx + dy*dy;
        if (l != 0) {
            a = (dx << 32) / l;
            b = (dy << 32) / l;
            off = (-a*pGradient->linear.p1.x - b*pGradient->linear.p1.y)>>16;
        }
        if (l == 0  || (unit.vector[2] == 0 && v.vector[2] == xFixed1)) {
            xFixed_48_16 inc, t;
            /* affine transformation only */
            if (l == 0) {
                t = 0;
                inc = 0;
            } else {
                t = ((a*v.vector[0] + b*v.vector[1]) >> 16) + off;
                inc = (a * unit.vector[0] + b * unit.vector[1]) >> 16;
            }

	    if (pGradient->linear.class == SourcePictClassVertical)
	    {
		register CARD32 color;

		color = _gradient_walker_pixel( &walker, t );
		while (buffer < end)
		    WRITE(buffer++, color);
	    }
	    else
	    {
                if (!mask) {
                    while (buffer < end)
                    {
                        WRITE(buffer, _gradient_walker_pixel (&walker, t));
                        buffer += 1;
                        t      += inc;
                    }
                } else {
                    while (buffer < end) {
                        if (*mask++ & maskBits)
                        {
                            WRITE(buffer, _gradient_walker_pixel (&walker, t));
                        }
                        buffer += 1;
                        t      += inc;
                    }
                }
	    }
	}
	else /* projective transformation */
	{
	    xFixed_48_16 t;

	    if (pGradient->linear.class == SourcePictClassVertical)
	    {
		register CARD32 color;

		if (v.vector[2] == 0)
		{
		    t = 0;
		}
		else
		{
		    xFixed_48_16 x, y;

		    x = ((xFixed_48_16) v.vector[0] << 16) / v.vector[2];
		    y = ((xFixed_48_16) v.vector[1] << 16) / v.vector[2];
		    t = ((a * x + b * y) >> 16) + off;
		}

 		color = _gradient_walker_pixel( &walker, t );
		while (buffer < end)
		    WRITE(buffer++, color);
	    }
	    else
	    {
		while (buffer < end)
		{
		    if (!mask || *mask++ & maskBits)
		    {
			if (v.vector[2] == 0) {
			    t = 0;
			} else {
			    xFixed_48_16 x, y;
			    x = ((xFixed_48_16)v.vector[0] << 16) / v.vector[2];
			    y = ((xFixed_48_16)v.vector[1] << 16) / v.vector[2];
			    t = ((a*x + b*y) >> 16) + off;
			}
			WRITE(buffer, _gradient_walker_pixel (&walker, t));
		    }
		    ++buffer;
		    v.vector[0] += unit.vector[0];
		    v.vector[1] += unit.vector[1];
		    v.vector[2] += unit.vector[2];
		}
            }
        }
    } else {

/*
 * In the radial gradient problem we are given two circles (c₁,r₁) and
 * (c₂,r₂) that define the gradient itself. Then, for any point p, we
 * must compute the value(s) of t within [0.0, 1.0] representing the
 * circle(s) that would color the point.
 *
 * There are potentially two values of t since the point p can be
 * colored by both sides of the circle, (which happens whenever one
 * circle is not entirely contained within the other).
 *
 * If we solve for a value of t that is outside of [0.0, 1.0] then we
 * use the extend mode (NONE, REPEAT, REFLECT, or PAD) to map to a
 * value within [0.0, 1.0].
 *
 * Here is an illustration of the problem:
 *
 *              p₂
 *           p  •
 *           •   ╲
 *        ·       ╲r₂
 *  p₁ ·           ╲
 *  •              θ╲
 *   ╲             ╌╌•
 *    ╲r₁        ·   c₂
 *    θ╲    ·
 *    ╌╌•
 *      c₁
 *
 * Given (c₁,r₁), (c₂,r₂) and p, we must find an angle θ such that two
 * points p₁ and p₂ on the two circles are collinear with p. Then, the
 * desired value of t is the ratio of the length of p₁p to the length
 * of p₁p₂.
 *
 * So, we have six unknown values: (p₁x, p₁y), (p₂x, p₂y), θ and t.
 * We can also write six equations that constrain the problem:
 *
 * Point p₁ is a distance r₁ from c₁ at an angle of θ:
 *
 *	1. p₁x = c₁x + r₁·cos θ
 *	2. p₁y = c₁y + r₁·sin θ
 *
 * Point p₂ is a distance r₂ from c₂ at an angle of θ:
 *
 *	3. p₂x = c₂x + r2·cos θ
 *	4. p₂y = c₂y + r2·sin θ
 *
 * Point p lies at a fraction t along the line segment p₁p₂:
 *
 *	5. px = t·p₂x + (1-t)·p₁x
 *	6. py = t·p₂y + (1-t)·p₁y
 *
 * To solve, first subtitute 1-4 into 5 and 6:
 *
 * px = t·(c₂x + r₂·cos θ) + (1-t)·(c₁x + r₁·cos θ)
 * py = t·(c₂y + r₂·sin θ) + (1-t)·(c₁y + r₁·sin θ)
 *
 * Then solve each for cos θ and sin θ expressed as a function of t:
 *
 * cos θ = (-(c₂x - c₁x)·t + (px - c₁x)) / ((r₂-r₁)·t + r₁)
 * sin θ = (-(c₂y - c₁y)·t + (py - c₁y)) / ((r₂-r₁)·t + r₁)
 *
 * To simplify this a bit, we define new variables for several of the
 * common terms as shown below:
 *
 *              p₂
 *           p  •
 *           •   ╲
 *        ·  ┆    ╲r₂
 *  p₁ ·     ┆     ╲
 *  •     pdy┆      ╲
 *   ╲       ┆       •c₂
 *    ╲r₁    ┆   ·   ┆
 *     ╲    ·┆       ┆cdy
 *      •╌╌╌╌┴╌╌╌╌╌╌╌┘
 *    c₁  pdx   cdx
 *
 * cdx = (c₂x - c₁x)
 * cdy = (c₂y - c₁y)
 *  dr =  r₂-r₁
 * pdx =  px - c₁x
 * pdy =  py - c₁y
 *
 * Note that cdx, cdy, and dr do not depend on point p at all, so can
 * be pre-computed for the entire gradient. The simplifed equations
 * are now:
 *
 * cos θ = (-cdx·t + pdx) / (dr·t + r₁)
 * sin θ = (-cdy·t + pdy) / (dr·t + r₁)
 *
 * Finally, to get a single function of t and eliminate the last
 * unknown θ, we use the identity sin²θ + cos²θ = 1. First, square
 * each equation, (we knew a quadratic was coming since it must be
 * possible to obtain two solutions in some cases):
 *
 * cos²θ = (cdx²t² - 2·cdx·pdx·t + pdx²) / (dr²·t² + 2·r₁·dr·t + r₁²)
 * sin²θ = (cdy²t² - 2·cdy·pdy·t + pdy²) / (dr²·t² + 2·r₁·dr·t + r₁²)
 *
 * Then add both together, set the result equal to 1, and express as a
 * standard quadratic equation in t of the form At² + Bt + C = 0
 *
 * (cdx² + cdy² - dr²)·t² - 2·(cdx·pdx + cdy·pdy + r₁·dr)·t + (pdx² + pdy² - r₁²) = 0
 *
 * In other words:
 *
 * A = cdx² + cdy² - dr²
 * B = -2·(pdx·cdx + pdy·cdy + r₁·dr)
 * C = pdx² + pdy² - r₁²
 *
 * And again, notice that A does not depend on p, so can be
 * precomputed. From here we just use the quadratic formula to solve
 * for t:
 *
 * t = (-2·B ± ⎷(B² - 4·A·C)) / 2·A
 */
        /* radial or conical */
        Bool affine = TRUE;
        double cx = 1.;
        double cy = 0.;
        double cz = 0.;
	double rx = x + 0.5;
	double ry = y + 0.5;
        double rz = 1.;

        if (pict->transform) {
            PictVector v;
            /* reference point is the center of the pixel */
            v.vector[0] = IntToxFixed(x) + xFixed1/2;
            v.vector[1] = IntToxFixed(y) + xFixed1/2;
            v.vector[2] = xFixed1;
            if (!PictureTransformPoint3d (pict->transform, &v))
                return;

            cx = pict->transform->matrix[0][0]/65536.;
            cy = pict->transform->matrix[1][0]/65536.;
            cz = pict->transform->matrix[2][0]/65536.;
            rx = v.vector[0]/65536.;
            ry = v.vector[1]/65536.;
            rz = v.vector[2]/65536.;
            affine = pict->transform->matrix[2][0] == 0 && v.vector[2] == xFixed1;
        }

        if (pGradient->type == SourcePictTypeRadial) {
	    PictRadialGradient *radial;
	    radial = &pGradient->radial;
            if (affine) {
                while (buffer < end) {
		    if (!mask || *mask++ & maskBits)
		    {
			double pdx, pdy;
			double B, C;
			double det;
			double c1x = radial->c1.x / 65536.0;
			double c1y = radial->c1.y / 65536.0;
			double r1  = radial->c1.radius / 65536.0;
                        xFixed_48_16 t;

			pdx = rx - c1x;
			pdy = ry - c1y;

			B = -2 * (  pdx * radial->cdx
				  + pdy * radial->cdy
				  + r1 * radial->dr);
			C = (pdx * pdx + pdy * pdy - r1 * r1);

                        det = (B * B) - (4 * radial->A * C);
			if (det < 0.0)
			    det = 0.0;

			if (radial->A < 0)
			    t = (xFixed_48_16) ((- B - sqrt(det)) / (2.0 * radial->A) * 65536);
			else
			    t = (xFixed_48_16) ((- B + sqrt(det)) / (2.0 * radial->A) * 65536);
			
 			WRITE(buffer, _gradient_walker_pixel (&walker, t));
		    }
		    ++buffer;
		    
                    rx += cx;
                    ry += cy;
                }
            } else {
		/* projective */
                while (buffer < end) {
		    if (!mask || *mask++ & maskBits)
		    {
			double pdx, pdy;
			double B, C;
			double det;
			double c1x = radial->c1.x / 65536.0;
			double c1y = radial->c1.y / 65536.0;
			double r1  = radial->c1.radius / 65536.0;
                        xFixed_48_16 t;
			double x, y;

			if (rz != 0) {
			    x = rx/rz;
			    y = ry/rz;
			} else {
			    x = y = 0.;
			}
			
			pdx = x - c1x;
			pdy = y - c1y;

			B = -2 * (  pdx * radial->cdx
				  + pdy * radial->cdy
				  + r1 * radial->dr);
			C = (pdx * pdx + pdy * pdy - r1 * r1);

                        det = (B * B) - (4 * radial->A * C);
			if (det < 0.0)
			    det = 0.0;

			if (radial->A < 0)
			    t = (xFixed_48_16) ((- B - sqrt(det)) / (2.0 * radial->A) * 65536);
			else
			    t = (xFixed_48_16) ((- B + sqrt(det)) / (2.0 * radial->A) * 65536);
			
 			WRITE(buffer, _gradient_walker_pixel (&walker, t));
		    }
		    ++buffer;
		    
                    rx += cx;
                    ry += cy;
		    rz += cz;
                }
            }
        } else /* SourcePictTypeConical */ {
            double a = pGradient->conical.angle/(180.*65536);
            if (affine) {
                rx -= pGradient->conical.center.x/65536.;
                ry -= pGradient->conical.center.y/65536.;

                while (buffer < end) {
		    double angle;

                    if (!mask || *mask++ & maskBits)
		    {
                        xFixed_48_16   t;
			
                        angle = atan2(ry, rx) + a;
			t     = (xFixed_48_16) (angle * (65536. / (2*M_PI)));
			
			WRITE(buffer, _gradient_walker_pixel (&walker, t));
		    }

                    ++buffer;
                    rx += cx;
                    ry += cy;
                }
            } else {
                while (buffer < end) {
                    double x, y;
                    double angle;
		    
                    if (!mask || *mask++ & maskBits)
                    {
			xFixed_48_16  t;
			
			if (rz != 0) {
			    x = rx/rz;
			    y = ry/rz;
			} else {
			    x = y = 0.;
			}
			x -= pGradient->conical.center.x/65536.;
			y -= pGradient->conical.center.y/65536.;
			angle = atan2(y, x) + a;
			t     = (xFixed_48_16) (angle * (65536. / (2*M_PI)));
			
			WRITE(buffer, _gradient_walker_pixel (&walker, t));
		    }
		    
                    ++buffer;
                    rx += cx;
                    ry += cy;
                    rz += cz;
                }
            }
        }
    }
}

static void fbFetchTransformed(PicturePtr pict, int x, int y, int width, CARD32 *buffer, CARD32 *mask, CARD32 maskBits)
{
    FbBits     *bits;
    FbStride    stride;
    int         bpp;
    int         xoff, yoff, dx, dy;
    fetchPixelProc   fetch;
    PictVector	v;
    PictVector  unit;
    int         i;
    BoxRec	box;
    miIndexedPtr indexed = (miIndexedPtr) pict->pFormat->index.devPrivate;
    Bool affine = TRUE;

    fetch = fetchPixelProcForPicture(pict);

    fbGetDrawable(pict->pDrawable, bits, stride, bpp, xoff, yoff);
    x += xoff;
    y += yoff;

    dx = pict->pDrawable->x;
    dy = pict->pDrawable->y;

    /* reference point is the center of the pixel */
    v.vector[0] = IntToxFixed(x - dx) + xFixed1 / 2;
    v.vector[1] = IntToxFixed(y - dy) + xFixed1 / 2;
    v.vector[2] = xFixed1;

    /* when using convolution filters one might get here without a transform */
    if (pict->transform) {
        if (!PictureTransformPoint3d (pict->transform, &v)) {
            fbFinishAccess (pict->pDrawable);
            return;
        }
        unit.vector[0] = pict->transform->matrix[0][0];
        unit.vector[1] = pict->transform->matrix[1][0];
        unit.vector[2] = pict->transform->matrix[2][0];
        affine = v.vector[2] == xFixed1 && unit.vector[2] == 0;
    } else {
        unit.vector[0] = xFixed1;
        unit.vector[1] = 0;
        unit.vector[2] = 0;
    }

    if (pict->filter == PictFilterNearest)
    {
        if (pict->repeatType == RepeatNormal) {
            if (REGION_NUM_RECTS(pict->pCompositeClip) == 1) {
                for (i = 0; i < width; ++i) {
		    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    if (!affine) {
				y = MOD(DIV(v.vector[1],v.vector[2]), pict->pDrawable->height);
				x = MOD(DIV(v.vector[0],v.vector[2]), pict->pDrawable->width);
			    } else {
				y = MOD(v.vector[1]>>16, pict->pDrawable->height);
				x = MOD(v.vector[0]>>16, pict->pDrawable->width);
			    }
			    WRITE(buffer + i, fetch(bits + (y + dy)*stride, x + dx, indexed));
			}
		    }

                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            } else {
                for (i = 0; i < width; ++i) {
		    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    if (!affine) {
				y = MOD(DIV(v.vector[1],v.vector[2]), pict->pDrawable->height);
				x = MOD(DIV(v.vector[0],v.vector[2]), pict->pDrawable->width);
			    } else {
				y = MOD(v.vector[1]>>16, pict->pDrawable->height);
				x = MOD(v.vector[0]>>16, pict->pDrawable->width);
			    }
			    if (POINT_IN_REGION (0, pict->pCompositeClip, x, y, &box))
				WRITE(buffer + i, fetch(bits + (y + dy)*stride, x + dx, indexed));
			    else
				WRITE(buffer + i, 0);
			}
		    }

                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            }
        } else {
            if (REGION_NUM_RECTS(pict->pCompositeClip) == 1) {
                box = pict->pCompositeClip->extents;
                for (i = 0; i < width; ++i) {
		    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    if (!affine) {
				y = DIV(v.vector[1],v.vector[2]);
				x = DIV(v.vector[0],v.vector[2]);
			    } else {
				y = v.vector[1]>>16;
				x = v.vector[0]>>16;
			    }
			    WRITE(buffer + i, ((x < box.x1-dx) | (x >= box.x2-dx) | (y < box.y1-dy) | (y >= box.y2-dy)) ?
				  0 : fetch(bits + (y + dy)*stride, x + dx, indexed));
			}
		    }
                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            } else {
                for (i = 0; i < width; ++i) {
                    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    if (!affine) {
				y = DIV(v.vector[1],v.vector[2]);
				x = DIV(v.vector[0],v.vector[2]);
			    } else {
				y = v.vector[1]>>16;
				x = v.vector[0]>>16;
			    }
			    if (POINT_IN_REGION (0, pict->pCompositeClip, x + dx, y + dy, &box))
				WRITE(buffer + i, fetch(bits + (y + dy)*stride, x + dx, indexed));
			    else
				WRITE(buffer + i, 0);
			}
		    }
                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            }
        }
    } else if (pict->filter == PictFilterBilinear) {
        /* adjust vector for maximum contribution at 0.5, 0.5 of each texel. */
        v.vector[0] -= v.vector[2] / 2;
        v.vector[1] -= v.vector[2] / 2;
        unit.vector[0] -= unit.vector[2] / 2;
        unit.vector[1] -= unit.vector[2] / 2;

        if (pict->repeatType == RepeatNormal) {
            if (REGION_NUM_RECTS(pict->pCompositeClip) == 1) {
                for (i = 0; i < width; ++i) {
                    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    int x1, x2, y1, y2, distx, idistx, disty, idisty;
			    FbBits *b;
			    CARD32 tl, tr, bl, br, r;
			    CARD32 ft, fb;

			    if (!affine) {
				xFixed_48_16 div;
				div = ((xFixed_48_16)v.vector[0] << 16)/v.vector[2];
				x1 = div >> 16;
				distx = ((xFixed)div >> 8) & 0xff;
				div = ((xFixed_48_16)v.vector[1] << 16)/v.vector[2];
				y1 = div >> 16;
				disty = ((xFixed)div >> 8) & 0xff;
			    } else {
				x1 = v.vector[0] >> 16;
				distx = (v.vector[0] >> 8) & 0xff;
				y1 = v.vector[1] >> 16;
				disty = (v.vector[1] >> 8) & 0xff;
			    }
			    x2 = x1 + 1;
			    y2 = y1 + 1;

			    idistx = 256 - distx;
			    idisty = 256 - disty;

			    x1 = MOD (x1, pict->pDrawable->width);
			    x2 = MOD (x2, pict->pDrawable->width);
			    y1 = MOD (y1, pict->pDrawable->height);
			    y2 = MOD (y2, pict->pDrawable->height);

			    b = bits + (y1 + dy)*stride;

			    tl = fetch(b, x1 + dx, indexed);
			    tr = fetch(b, x2 + dx, indexed);
			    b = bits + (y2 + dy)*stride;
			    bl = fetch(b, x1 + dx, indexed);
			    br = fetch(b, x2 + dx, indexed);

			    ft = FbGet8(tl,0) * idistx + FbGet8(tr,0) * distx;
			    fb = FbGet8(bl,0) * idistx + FbGet8(br,0) * distx;
			    r = (((ft * idisty + fb * disty) >> 16) & 0xff);
			    ft = FbGet8(tl,8) * idistx + FbGet8(tr,8) * distx;
			    fb = FbGet8(bl,8) * idistx + FbGet8(br,8) * distx;
			    r |= (((ft * idisty + fb * disty) >> 8) & 0xff00);
			    ft = FbGet8(tl,16) * idistx + FbGet8(tr,16) * distx;
			    fb = FbGet8(bl,16) * idistx + FbGet8(br,16) * distx;
			    r |= (((ft * idisty + fb * disty)) & 0xff0000);
			    ft = FbGet8(tl,24) * idistx + FbGet8(tr,24) * distx;
			    fb = FbGet8(bl,24) * idistx + FbGet8(br,24) * distx;
			    r |= (((ft * idisty + fb * disty) << 8) & 0xff000000);
			    WRITE(buffer + i, r);
			}
		    }
                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            } else {
                for (i = 0; i < width; ++i) {
		    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    int x1, x2, y1, y2, distx, idistx, disty, idisty;
			    FbBits *b;
			    CARD32 tl, tr, bl, br, r;
			    CARD32 ft, fb;

			    if (!affine) {
				xFixed_48_16 div;
				div = ((xFixed_48_16)v.vector[0] << 16)/v.vector[2];
				x1 = div >> 16;
				distx = ((xFixed)div >> 8) & 0xff;
				div = ((xFixed_48_16)v.vector[1] << 16)/v.vector[2];
				y1 = div >> 16;
				disty = ((xFixed)div >> 8) & 0xff;
			    } else {
				x1 = v.vector[0] >> 16;
				distx = (v.vector[0] >> 8) & 0xff;
				y1 = v.vector[1] >> 16;
				disty = (v.vector[1] >> 8) & 0xff;
			    }
			    x2 = x1 + 1;
			    y2 = y1 + 1;

			    idistx = 256 - distx;
			    idisty = 256 - disty;

			    x1 = MOD (x1, pict->pDrawable->width);
			    x2 = MOD (x2, pict->pDrawable->width);
			    y1 = MOD (y1, pict->pDrawable->height);
			    y2 = MOD (y2, pict->pDrawable->height);

			    b = bits + (y1 + dy)*stride;

			    tl = POINT_IN_REGION(0, pict->pCompositeClip, x1 + dx, y1 + dy, &box)
				? fetch(b, x1 + dx, indexed) : 0;
			    tr = POINT_IN_REGION(0, pict->pCompositeClip, x2 + dx, y1 + dy, &box)
				? fetch(b, x2 + dx, indexed) : 0;
			    b = bits + (y2 + dy)*stride;
			    bl = POINT_IN_REGION(0, pict->pCompositeClip, x1 + dx, y2 + dy, &box)
				? fetch(b, x1 + dx, indexed) : 0;
			    br = POINT_IN_REGION(0, pict->pCompositeClip, x2 + dx, y2 + dy, &box)
				? fetch(b, x2 + dx, indexed) : 0;

			    ft = FbGet8(tl,0) * idistx + FbGet8(tr,0) * distx;
			    fb = FbGet8(bl,0) * idistx + FbGet8(br,0) * distx;
			    r = (((ft * idisty + fb * disty) >> 16) & 0xff);
			    ft = FbGet8(tl,8) * idistx + FbGet8(tr,8) * distx;
			    fb = FbGet8(bl,8) * idistx + FbGet8(br,8) * distx;
			    r |= (((ft * idisty + fb * disty) >> 8) & 0xff00);
			    ft = FbGet8(tl,16) * idistx + FbGet8(tr,16) * distx;
			    fb = FbGet8(bl,16) * idistx + FbGet8(br,16) * distx;
			    r |= (((ft * idisty + fb * disty)) & 0xff0000);
			    ft = FbGet8(tl,24) * idistx + FbGet8(tr,24) * distx;
			    fb = FbGet8(bl,24) * idistx + FbGet8(br,24) * distx;
			    r |= (((ft * idisty + fb * disty) << 8) & 0xff000000);
			    WRITE(buffer + i, r);
			}
		    }

                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            }
        } else {
            if (REGION_NUM_RECTS(pict->pCompositeClip) == 1) {
                box = pict->pCompositeClip->extents;
                for (i = 0; i < width; ++i) {
		    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    int x1, x2, y1, y2, distx, idistx, disty, idisty, x_off;
			    FbBits *b;
			    CARD32 tl, tr, bl, br, r;
			    Bool x1_out, x2_out, y1_out, y2_out;
			    CARD32 ft, fb;

			    if (!affine) {
				xFixed_48_16 div;
				div = ((xFixed_48_16)v.vector[0] << 16)/v.vector[2];
				x1 = div >> 16;
				distx = ((xFixed)div >> 8) & 0xff;
				div = ((xFixed_48_16)v.vector[1] << 16)/v.vector[2];
				y1 = div >> 16;
				disty = ((xFixed)div >> 8) & 0xff;
			    } else {
				x1 = v.vector[0] >> 16;
				distx = (v.vector[0] >> 8) & 0xff;
				y1 = v.vector[1] >> 16;
				disty = (v.vector[1] >> 8) & 0xff;
			    }
			    x2 = x1 + 1;
			    y2 = y1 + 1;

			    idistx = 256 - distx;
			    idisty = 256 - disty;

			    b = bits + (y1 + dy)*stride;
			    x_off = x1 + dx;

			    x1_out = (x1 < box.x1-dx) | (x1 >= box.x2-dx);
			    x2_out = (x2 < box.x1-dx) | (x2 >= box.x2-dx);
			    y1_out = (y1 < box.y1-dy) | (y1 >= box.y2-dy);
			    y2_out = (y2 < box.y1-dy) | (y2 >= box.y2-dy);

			    tl = x1_out|y1_out ? 0 : fetch(b, x_off, indexed);
			    tr = x2_out|y1_out ? 0 : fetch(b, x_off + 1, indexed);
			    b += stride;
			    bl = x1_out|y2_out ? 0 : fetch(b, x_off, indexed);
			    br = x2_out|y2_out ? 0 : fetch(b, x_off + 1, indexed);

			    ft = FbGet8(tl,0) * idistx + FbGet8(tr,0) * distx;
			    fb = FbGet8(bl,0) * idistx + FbGet8(br,0) * distx;
			    r = (((ft * idisty + fb * disty) >> 16) & 0xff);
			    ft = FbGet8(tl,8) * idistx + FbGet8(tr,8) * distx;
			    fb = FbGet8(bl,8) * idistx + FbGet8(br,8) * distx;
			    r |= (((ft * idisty + fb * disty) >> 8) & 0xff00);
			    ft = FbGet8(tl,16) * idistx + FbGet8(tr,16) * distx;
			    fb = FbGet8(bl,16) * idistx + FbGet8(br,16) * distx;
			    r |= (((ft * idisty + fb * disty)) & 0xff0000);
			    ft = FbGet8(tl,24) * idistx + FbGet8(tr,24) * distx;
			    fb = FbGet8(bl,24) * idistx + FbGet8(br,24) * distx;
			    r |= (((ft * idisty + fb * disty) << 8) & 0xff000000);
			    WRITE(buffer + i, r);
			}
		    }

                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            } else {
                for (i = 0; i < width; ++i) {
                    if (!mask || mask[i] & maskBits)
		    {
			if (!v.vector[2]) {
			    WRITE(buffer + i, 0);
			} else {
			    int x1, x2, y1, y2, distx, idistx, disty, idisty, x_off;
			    FbBits *b;
			    CARD32 tl, tr, bl, br, r;
			    CARD32 ft, fb;

			    if (!affine) {
				xFixed_48_16 div;
				div = ((xFixed_48_16)v.vector[0] << 16)/v.vector[2];
				x1 = div >> 16;
				distx = ((xFixed)div >> 8) & 0xff;
				div = ((xFixed_48_16)v.vector[1] << 16)/v.vector[2];
				y1 = div >> 16;
				disty = ((xFixed)div >> 8) & 0xff;
			    } else {
				x1 = v.vector[0] >> 16;
				distx = (v.vector[0] >> 8) & 0xff;
				y1 = v.vector[1] >> 16;
				disty = (v.vector[1] >> 8) & 0xff;
			    }
			    x2 = x1 + 1;
			    y2 = y1 + 1;

			    idistx = 256 - distx;
			    idisty = 256 - disty;

			    b = bits + (y1 + dy)*stride;
			    x_off = x1 + dx;

			    tl = POINT_IN_REGION(0, pict->pCompositeClip, x1 + dx, y1 + dy, &box)
				? fetch(b, x_off, indexed) : 0;
			    tr = POINT_IN_REGION(0, pict->pCompositeClip, x2 + dx, y1 + dy, &box)
				? fetch(b, x_off + 1, indexed) : 0;
			    b += stride;
			    bl = POINT_IN_REGION(0, pict->pCompositeClip, x1 + dx, y2 + dy, &box)
				? fetch(b, x_off, indexed) : 0;
			    br = POINT_IN_REGION(0, pict->pCompositeClip, x2 + dx, y2 + dy, &box)
				? fetch(b, x_off + 1, indexed) : 0;

			    ft = FbGet8(tl,0) * idistx + FbGet8(tr,0) * distx;
			    fb = FbGet8(bl,0) * idistx + FbGet8(br,0) * distx;
			    r = (((ft * idisty + fb * disty) >> 16) & 0xff);
			    ft = FbGet8(tl,8) * idistx + FbGet8(tr,8) * distx;
			    fb = FbGet8(bl,8) * idistx + FbGet8(br,8) * distx;
			    r |= (((ft * idisty + fb * disty) >> 8) & 0xff00);
			    ft = FbGet8(tl,16) * idistx + FbGet8(tr,16) * distx;
			    fb = FbGet8(bl,16) * idistx + FbGet8(br,16) * distx;
			    r |= (((ft * idisty + fb * disty)) & 0xff0000);
			    ft = FbGet8(tl,24) * idistx + FbGet8(tr,24) * distx;
			    fb = FbGet8(bl,24) * idistx + FbGet8(br,24) * distx;
			    r |= (((ft * idisty + fb * disty) << 8) & 0xff000000);
			    WRITE(buffer + i, r);
			}
		    }

                    v.vector[0] += unit.vector[0];
                    v.vector[1] += unit.vector[1];
                    v.vector[2] += unit.vector[2];
                }
            }
        }
    } else if (pict->filter == PictFilterConvolution) {
        xFixed *params = pict->filter_params;
        INT32 cwidth = xFixedToInt(params[0]);
        INT32 cheight = xFixedToInt(params[1]);
        int xoff = (params[0] - xFixed1) >> 1;
	int yoff = (params[1] - xFixed1) >> 1;
        params += 2;
        for (i = 0; i < width; ++i) {
	    if (!mask || mask[i] & maskBits)
	    {
		if (!v.vector[2]) {
		    WRITE(buffer + i, 0);
		} else {
		    int x1, x2, y1, y2, x, y;
		    INT32 srtot, sgtot, sbtot, satot;
		    xFixed *p = params;

		    if (!affine) {
			xFixed_48_16 tmp;
			tmp = ((xFixed_48_16)v.vector[0] << 16)/v.vector[2] - xoff;
			x1 = xFixedToInt(tmp);
			tmp = ((xFixed_48_16)v.vector[1] << 16)/v.vector[2] - yoff;
			y1 = xFixedToInt(tmp);
		    } else {
			x1 = xFixedToInt(v.vector[0] - xoff);
			y1 = xFixedToInt(v.vector[1] - yoff);
		    }
		    x2 = x1 + cwidth;
		    y2 = y1 + cheight;

		    srtot = sgtot = sbtot = satot = 0;

		    for (y = y1; y < y2; y++) {
			int ty = (pict->repeatType == RepeatNormal) ? MOD (y, pict->pDrawable->height) : y;
			for (x = x1; x < x2; x++) {
			    if (*p) {
				int tx = (pict->repeatType == RepeatNormal) ? MOD (x, pict->pDrawable->width) : x;
				if (POINT_IN_REGION (0, pict->pCompositeClip, tx + dx, ty + dy, &box)) {
				    FbBits *b = bits + (ty + dy)*stride;
				    CARD32 c = fetch(b, tx + dx, indexed);

				    srtot += Red(c) * *p;
				    sgtot += Green(c) * *p;
				    sbtot += Blue(c) * *p;
				    satot += Alpha(c) * *p;
				}
			    }
			    p++;
			}
		    }

		    satot >>= 16;
		    srtot >>= 16;
		    sgtot >>= 16;
		    sbtot >>= 16;

		    if (satot < 0) satot = 0; else if (satot > 0xff) satot = 0xff;
		    if (srtot < 0) srtot = 0; else if (srtot > 0xff) srtot = 0xff;
		    if (sgtot < 0) sgtot = 0; else if (sgtot > 0xff) sgtot = 0xff;
		    if (sbtot < 0) sbtot = 0; else if (sbtot > 0xff) sbtot = 0xff;

		    WRITE(buffer + i, ((satot << 24) |
				       (srtot << 16) |
				       (sgtot <<  8) |
				       (sbtot       )));
		}
	    }
            v.vector[0] += unit.vector[0];
            v.vector[1] += unit.vector[1];
            v.vector[2] += unit.vector[2];
        }
    }

    fbFinishAccess (pict->pDrawable);
}


static void fbFetchExternalAlpha(PicturePtr pict, int x, int y, int width, CARD32 *buffer, CARD32 *mask, CARD32 maskBits)
{
    int i;
    CARD32 _alpha_buffer[SCANLINE_BUFFER_LENGTH];
    CARD32 *alpha_buffer = _alpha_buffer;

    if (!pict->alphaMap) {
        fbFetchTransformed(pict, x, y, width, buffer, mask, maskBits);
	return;
    }
    if (width > SCANLINE_BUFFER_LENGTH)
        alpha_buffer = (CARD32 *) malloc(width*sizeof(CARD32));

    fbFetchTransformed(pict, x, y, width, buffer, mask, maskBits);
    fbFetchTransformed(pict->alphaMap, x - pict->alphaOrigin.x,
		       y - pict->alphaOrigin.y, width, alpha_buffer,
		       mask, maskBits);
    for (i = 0; i < width; ++i) {
        if (!mask || mask[i] & maskBits)
	{
	    int a = alpha_buffer[i]>>24;
	    WRITE(buffer + i, (a << 24)
		  | (div_255(Red(READ(buffer + i)) * a) << 16)
		  | (div_255(Green(READ(buffer + i)) * a) << 8)
		  | (div_255(Blue(READ(buffer + i)) * a)));
	}
    }

    if (alpha_buffer != _alpha_buffer)
        free(alpha_buffer);
}

static void fbStore(PicturePtr pict, int x, int y, int width, CARD32 *buffer)
{
    FbBits *bits;
    FbStride stride;
    int bpp;
    int xoff, yoff;
    storeProc store = storeProcForPicture(pict);
    miIndexedPtr indexed = (miIndexedPtr) pict->pFormat->index.devPrivate;

    fbGetDrawable (pict->pDrawable, bits, stride, bpp, xoff, yoff);
    x += xoff;
    y += yoff;

    bits += y*stride;
    store(bits, buffer, x, width, indexed);
    fbFinishAccess (pict->pDrawable);
}

static void fbStoreExternalAlpha(PicturePtr pict, int x, int y, int width, CARD32 *buffer)
{
    FbBits *bits, *alpha_bits;
    FbStride stride, astride;
    int bpp, abpp;
    int xoff, yoff;
    int ax, ay;
    storeProc store;
    storeProc astore;
    miIndexedPtr indexed = (miIndexedPtr) pict->pFormat->index.devPrivate;
    miIndexedPtr aindexed;

    if (!pict->alphaMap) {
        fbStore(pict, x, y, width, buffer);
	return;
    }

    store = storeProcForPicture(pict);
    astore = storeProcForPicture(pict->alphaMap);
    aindexed = (miIndexedPtr) pict->alphaMap->pFormat->index.devPrivate;

    ax = x;
    ay = y;

    fbGetDrawable (pict->pDrawable, bits, stride, bpp, xoff, yoff);
    x += xoff;
    y += yoff;
    fbGetDrawable (pict->alphaMap->pDrawable, alpha_bits, astride, abpp, xoff, yoff);
    ax += xoff;
    ay += yoff;

    bits       += y*stride;
    alpha_bits += (ay - pict->alphaOrigin.y)*astride;


    store(bits, buffer, x, width, indexed);
    astore(alpha_bits, buffer, ax - pict->alphaOrigin.x, width, aindexed);

    fbFinishAccess (pict->alphaMap->pDrawable);
    fbFinishAccess (pict->pDrawable);
}

typedef void (*scanStoreProc)(PicturePtr , int , int , int , CARD32 *);
typedef void (*scanFetchProc)(PicturePtr , int , int , int , CARD32 * , CARD32 *, CARD32);

void
fbCompositeRect (const FbComposeData *data, CARD32 *scanline_buffer)
{
    CARD32 *src_buffer = scanline_buffer;
    CARD32 *dest_buffer = src_buffer + data->width;
    int i;
    scanStoreProc store;
    scanFetchProc fetchSrc = NULL, fetchMask = NULL, fetchDest = NULL;
    unsigned int srcClass = SourcePictClassUnknown;
    unsigned int maskClass = SourcePictClassUnknown;
    FbBits *bits;
    FbStride stride;
    int xoff, yoff;
    
    if (data->op == PictOpClear)
        fetchSrc = NULL;
    else if (!data->src->pDrawable) {
        if (data->src->pSourcePict)
	{
            fetchSrc = fbFetchSourcePict;
	    srcClass = SourcePictureClassify (data->src,
					      data->xSrc, data->ySrc,
					      data->width, data->height);
	}
    } else if (data->src->alphaMap)
        fetchSrc = fbFetchExternalAlpha;
    else if (data->src->repeatType == RepeatNormal &&
             data->src->pDrawable->width == 1 && data->src->pDrawable->height == 1)
    {
        fetchSrc = fbFetchSolid;
	srcClass = SourcePictClassHorizontal;
    }
    else if (!data->src->transform && data->src->filter != PictFilterConvolution)
        fetchSrc = fbFetch;
    else
        fetchSrc = fbFetchTransformed;
    
    if (data->mask && data->op != PictOpClear) {
        if (!data->mask->pDrawable) {
            if (data->mask->pSourcePict)
                fetchMask = fbFetchSourcePict;
        } else if (data->mask->alphaMap)
	{
            fetchMask = fbFetchExternalAlpha;
	    maskClass = SourcePictureClassify (data->mask,
					       data->xMask, data->yMask,
					       data->width, data->height);
	}
        else if (data->mask->repeatType == RepeatNormal
                 && data->mask->pDrawable->width == 1 && data->mask->pDrawable->height == 1)
	{
	    fetchMask = fbFetchSolid;
	    maskClass = SourcePictClassHorizontal;
	}
        else if (!data->mask->transform && data->mask->filter != PictFilterConvolution)
            fetchMask = fbFetch;
        else
            fetchMask = fbFetchTransformed;
    } else {
        fetchMask = NULL;
    }
    
    if (data->dest->alphaMap)
    {
	fetchDest = fbFetchExternalAlpha;
	store = fbStoreExternalAlpha;
	
	if (data->op == PictOpClear || data->op == PictOpSrc)
	    fetchDest = NULL;
    }
    else
    {
	fetchDest = fbFetch;
	store = fbStore;
	
	switch (data->op) {
	case PictOpClear:
	case PictOpSrc:
	    fetchDest = NULL;
	    /* fall-through */
	case PictOpAdd:
	case PictOpOver:
	    switch (data->dest->format) {
	    case PICT_a8r8g8b8:
	    case PICT_x8r8g8b8:
		store = NULL;
		break;
	    default:
		break;
	    }
	    break;
	}
    }
    
    if (!store)
    {
	int bpp;
	
	fbGetDrawable (data->dest->pDrawable, bits, stride, bpp, xoff, yoff);
    }
    else
    {
	bits = NULL;
	stride = 0;
	xoff = yoff = 0;
    }
    
    if (fetchSrc		   &&
	fetchMask		   &&
	data->mask		   &&
	data->mask->componentAlpha &&
	PICT_FORMAT_RGB (data->mask->format))
    {
	CARD32 *mask_buffer = dest_buffer + data->width;
	CombineFuncC compose = composeFunctions.combineC[data->op];
	if (!compose)
	    return;
	
	for (i = 0; i < data->height; ++i) {
	    /* fill first half of scanline with source */
	    if (fetchSrc)
	    {
		if (fetchMask)
		{
		    /* fetch mask before source so that fetching of
		       source can be optimized */
		    fetchMask (data->mask, data->xMask, data->yMask + i,
			       data->width, mask_buffer, 0, 0);
		    
		    if (maskClass == SourcePictClassHorizontal)
			fetchMask = NULL;
		}
		
		if (srcClass == SourcePictClassHorizontal)
		{
		    fetchSrc (data->src, data->xSrc, data->ySrc + i,
			      data->width, src_buffer, 0, 0);
		    fetchSrc = NULL;
		}
		else
		{
		    fetchSrc (data->src, data->xSrc, data->ySrc + i,
			      data->width, src_buffer, mask_buffer,
			      0xffffffff);
		}
	    }
	    else if (fetchMask)
	    {
		fetchMask (data->mask, data->xMask, data->yMask + i,
			   data->width, mask_buffer, 0, 0);
	    }
	    
	    if (store)
	    {
		/* fill dest into second half of scanline */
		if (fetchDest)
		    fetchDest (data->dest, data->xDest, data->yDest + i,
			       data->width, dest_buffer, 0, 0);
		
		/* blend */
		compose (dest_buffer, src_buffer, mask_buffer, data->width);
		
		/* write back */
		store (data->dest, data->xDest, data->yDest + i, data->width,
		       dest_buffer);
	    }
	    else
	    {
		/* blend */
		compose (bits + (data->yDest + i+ yoff) * stride +
			 data->xDest + xoff,
			 src_buffer, mask_buffer, data->width);
	    }
	}
    }
    else
    {
	CARD32 *src_mask_buffer = 0, *mask_buffer = 0;
	CombineFuncU compose = composeFunctions.combineU[data->op];
	if (!compose)
	    return;
	
	if (fetchMask)
	    mask_buffer = dest_buffer + data->width;
	
	for (i = 0; i < data->height; ++i) {
	    /* fill first half of scanline with source */
	    if (fetchSrc)
	    {
		if (fetchMask)
		{
		    /* fetch mask before source so that fetching of
		       source can be optimized */
		    fetchMask (data->mask, data->xMask, data->yMask + i,
			       data->width, mask_buffer, 0, 0);
		    
		    if (maskClass == SourcePictClassHorizontal)
			fetchMask = NULL;
		}
		
		if (srcClass == SourcePictClassHorizontal)
		{
		    fetchSrc (data->src, data->xSrc, data->ySrc + i,
			      data->width, src_buffer, 0, 0);
		    
		    if (mask_buffer)
		    {
			fbCombineInU (mask_buffer, src_buffer, data->width);
			src_mask_buffer = mask_buffer;
		    }
		    else
			src_mask_buffer = src_buffer;
		    
		    fetchSrc = NULL;
		}
		else
		{
		    fetchSrc (data->src, data->xSrc, data->ySrc + i,
			      data->width, src_buffer, mask_buffer,
			      0xff000000);
		    
		    if (mask_buffer)
			composeFunctions.combineMaskU (src_buffer,
						       mask_buffer,
						       data->width);
		    
		    src_mask_buffer = src_buffer;
		}
	    }
	    else if (fetchMask)
	    {
		fetchMask (data->mask, data->xMask, data->yMask + i,
			   data->width, mask_buffer, 0, 0);
		
		fbCombineInU (mask_buffer, src_buffer, data->width);
		
		src_mask_buffer = mask_buffer;
	    }
	    
	    if (store)
	    {
		/* fill dest into second half of scanline */
		if (fetchDest)
		    fetchDest (data->dest, data->xDest, data->yDest + i,
			       data->width, dest_buffer, 0, 0);
		
		/* blend */
		compose (dest_buffer, src_mask_buffer, data->width);
		
		/* write back */
		store (data->dest, data->xDest, data->yDest + i, data->width,
		       dest_buffer);
	    }
	    else
	    {
		/* blend */
		compose (bits + (data->yDest + i+ yoff) * stride +
			 data->xDest + xoff,
			 src_mask_buffer, data->width);
	    }
	}
    }

    if (!store)
	fbFinishAccess (data->dest->pDrawable);
}

void
fbCompositeGeneral (CARD8	op,
		    PicturePtr	pSrc,
		    PicturePtr	pMask,
		    PicturePtr	pDst,
		    INT16	xSrc,
		    INT16	ySrc,
		    INT16	xMask,
		    INT16	yMask,
		    INT16	xDst,
		    INT16	yDst,
		    CARD16	width,
		    CARD16	height)
{
    return fbComposite (op, pSrc, pMask, pDst,
			xSrc, ySrc, xMask, yMask, xDst, yDst,
			width, height);
}

#endif
