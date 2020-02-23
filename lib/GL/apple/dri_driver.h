/* $XFree86: xc/lib/GL/apple/dri_driver.h,v 1.1 2003/06/30 01:45:10 torrey Exp $ */
/**************************************************************************

Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
Copyright (c) 2002 Apple Computer, Inc.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Kevin E. Martin <kevin@precisioninsight.com>
 *   Brian Paul <brian@precisioninsight.com>
 */

#ifndef _DRI_DRIVER_H_
#define _DRI_DRIVER_H_

#include "Xplugin.h"
#include "Xthreads.h"
#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>

#ifdef GLX_DIRECT_RENDERING

typedef struct __DRIdisplayPrivateRec  __DRIdisplayPrivate;
typedef struct __DRIscreenPrivateRec   __DRIscreenPrivate;
typedef struct __DRIvisualPrivateRec   __DRIvisualPrivate;
typedef struct __DRIcontextPrivateRec  __DRIcontextPrivate;
typedef struct __DRIdrawablePrivateRec __DRIdrawablePrivate;

#endif /* GLX_DIRECT_RENDERING */

#define DRI_MESA_VALIDATE_DRAWABLE_INFO(dpy,scrn,pDrawPriv)  \
    do {                                                     \
        if (*(pDrawPriv->pStamp) != pDrawPriv->lastStamp) {  \
            driMesaUpdateDrawableInfo(dpy,scrn,pDrawPriv);   \
        }                                                    \
    } while (0)


/**
 * Per-drawable private DRI driver information.
 */
struct __DRIdrawablePrivateRec {
    /*
    ** X's drawable ID associated with this private drawable.
    */
    GLXDrawable draw;

    /*
    ** Reference count for number of context's currently bound to this
    ** drawable.  Once the refcount reaches 0, the drawable can be
    ** destroyed.  This behavior will change with GLX 1.3.
    */
    int refcount;

    xp_surface_id surface_id;
    unsigned int uid;

    /*
    ** Pointer to contexts to which this drawable is currently bound.
    */
    __DRIcontextPrivate *driContextPriv;

    /*
    ** Pointer to screen on which this drawable was created.
    */
    __DRIscreenPrivate *driScreenPriv;

    /*
    ** Set when the drawable on the server is known to have gone away
    */
    unsigned int destroyed :1;
};


/**
 * Per-context private driver information.
 */
struct __DRIcontextPrivateRec {
    /*
    ** Other contexts bound to the same drawable.
    */
    __DRIcontextPrivate *next, *prev; 

    /*
    ** Kernel context handle used to access the device lock.
    */
    XID contextID;

    /*
    ** CGL context info
    */
    CGLContextObj ctx;
    CGLPixelFormatObj pixelFormat;

    /*
    ** Set when attached
    */
    xp_surface_id surface_id;
    xthread_t thread_id;

    /*
    ** This context's display pointer.
    */
    __DRInativeDisplay *display;

    /*
    ** Pointer to drawable currently bound to this context.
    */
    __DRIdrawablePrivate *driDrawablePriv;

    /*
    ** Pointer to screen on which this context was created.
    */
    __DRIscreenPrivate *driScreenPriv;

    /*
    ** wrapped CGL vectors
    */
    struct {
        void (*viewport)(GLIContext ctx, GLint x, GLint y,
                         GLsizei width, GLsizei height);
        void (*new_list)(GLIContext ctx, GLuint list, GLenum mode);
        void (*end_list)(GLIContext ctx);
    } disp;

    unsigned int pending_update :1;
    unsigned int pending_clear :1;
};


struct __DRIvisualPrivateRec {
    /*
    ** X's visual ID associated with this private visual.
    */
    VisualID vid;

    /*
    ** CGL object representing the visual
    */
    CGLPixelFormatObj pixel_format;
};


/**
 * Per-screen private driver information.
 */
struct __DRIscreenPrivateRec {
    /*
    ** Display for this screen
    */
    __DRInativeDisplay *display;

    /*
    ** Mutex for this screen
    */
    xmutex_t mutex;

    /*
    ** Current screen's number
    */
    int myNum;

    /**
     * ID used when the client sets the drawable lock.
     *
     * The X server uses this value to detect if the client has died while
     * holding the drawable lock.
     */
    int drawLockID;

    /**
     * \name Direct frame buffer access information 
     * Used for software fallbacks.
     */
    /*@{*/
    unsigned char *pFB;
    int fbSize;
    int fbOrigin;
    int fbStride;
    int fbWidth;
    int fbHeight;
    int fbBPP;
    /*@}*/

    /**
     * Dummy context to which drawables are bound when not bound to any
     * other context. 
     *
     * A dummy hHWContext is created for this context, and is used by the GL
     * core when a hardware lock is required but the drawable is not currently
     * bound (e.g., potentially during a SwapBuffers request).  The dummy
     * context is created when the first "real" context is created on this
     * screen.
     */
    __DRIcontextPrivate dummyContextPriv;

    /*
    ** Hash table to hold the drawable information for this screen.
    */
    void *drawHash;

    /**
     * GLX visuals / FBConfigs for this screen.  These are stored as a
     * linked list.
     * 
     * \note
     * This field is \b only used in conjunction with the old interfaces.  If
     * the new interfaces are used, this field will be set to \c NULL and will
     * not be dereferenced.
     */
    __GLcontextModes *modes;

    /**
     * Pointer back to the \c __DRIscreen that contains this structure.
     */
    __DRIscreen *psc;
};


extern void driMesaUpdateDrawableInfo(__DRInativeDisplay *dpy, int scrn,
                                      __DRIdrawablePrivate *pdp);


#endif /* _DRI_DRIVER_H_ */