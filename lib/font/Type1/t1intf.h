/* $Xorg: t1intf.h,v 1.3 2000/08/17 19:46:33 cpqbld Exp $ */
/* Copyright International Business Machines,Corp. 1991
 * All Rights Reserved
 *
 * License to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is
 * hereby granted, provided that the above copyright notice
 * appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation,
 * and that the name of IBM not be used in advertising or
 * publicity pertaining to distribution of the software without
 * specific, written prior permission.
 *
 * IBM PROVIDES THIS SOFTWARE "AS IS", WITHOUT ANY WARRANTIES
 * OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING, BUT NOT
 * LIMITED TO ANY IMPLIED WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS.  THE ENTIRE RISK AS TO THE QUALITY AND
 * PERFORMANCE OF THE SOFTWARE, INCLUDING ANY DUTY TO SUPPORT
 * OR MAINTAIN, BELONGS TO THE LICENSEE.  SHOULD ANY PORTION OF
 * THE SOFTWARE PROVE DEFECTIVE, THE LICENSEE (NOT IBM) ASSUMES
 * THE ENTIRE COST OF ALL SERVICING, REPAIR AND CORRECTION.  IN
 * NO EVENT SHALL IBM BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */
/* $XFree86: xc/lib/font/Type1/t1intf.h,v 1.6 2001/01/17 19:43:23 dawes Exp $ */

struct type1font {
       CharInfoPtr  pDefault;
       CharInfoRec  glyphs[256];
};

/*
 * Function prototypes
 */
/* t1funcs.c */
extern int Type1OpenScalable ( FontPathElementPtr fpe, FontPtr *ppFont, 
			       int flags, FontEntryPtr entry, char *fileName,
			       FontScalablePtr vals, fsBitmapFormat format, 
			       fsBitmapFormatMask fmask,
			       FontPtr non_cachable_font );
extern void Type1CloseFont ( FontPtr pFont );
extern int Type1ReturnCodeToXReturnCode ( int rc );

/* t1info.c */
extern int Type1GetInfoScalable ( FontPathElementPtr fpe, FontInfoPtr pInfo, 
				  FontEntryPtr entry, FontNamePtr fontName, 
				  char *fileName, FontScalablePtr Vals );
extern void T1FillFontInfo ( FontPtr pFont, FontScalablePtr Vals, 
			     char *Filename, char *Fontname, long sWidth );
extern void Type1InitStdProps ( void );
