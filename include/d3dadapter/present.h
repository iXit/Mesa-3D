/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#ifndef _D3DADAPTER_PRESENT_H_
#define _D3DADAPTER_PRESENT_H_

#include <d3d9.h>

#ifndef D3DOK_WINDOW_OCCLUDED
#define D3DOK_WINDOW_OCCLUDED MAKE_D3DSTATUS(2531)
#endif /* D3DOK_WINDOW_OCCLUDED */

#ifndef __cplusplus
typedef struct ID3DPresent ID3DPresent;
typedef struct ID3DPresentGroup ID3DPresentGroup;
typedef struct ID3DAdapter9 ID3DAdapter9;
typedef struct D3DWindowBuffer D3DWindowBuffer;

/* Presentation backend for drivers to display their brilliant work */
typedef struct ID3DPresentVtbl
{
    /* TODO: check if there is already some way to give a version number to this interface,
     * in case something needs to be added some day */
    /* IUnknown */
    HRESULT (WINAPI *QueryInterface)(ID3DPresent *This, REFIID riid, void **ppvObject);
    ULONG (WINAPI *AddRef)(ID3DPresent *This);
    ULONG (WINAPI *Release)(ID3DPresent *This);

    /* ID3DPresent */
    /* This function initializes the screen and window provided at creation.
     * Hence why this should always be called as the one of first things a new
     * swap chain does */
    HRESULT (WINAPI *GetPresentParameters)(ID3DPresent *This, D3DPRESENT_PARAMETERS *pPresentationParameters);
    /* Make a buffer visible to the window system via dma-buf fd.
     * For better compatibility, it must be 32bpp and format ARGB/XRGB */
    HRESULT (WINAPI *NewBuffer)(ID3DPresent *This, int dmaBufFd, int width, int height, int stride, int depth, int bpp, D3DWindowBuffer **out);
    HRESULT (WINAPI *DestroyBuffer)(ID3DPresent *This, D3DWindowBuffer *buffer);
    /* After presenting a buffer to the window system, the buffer
     * may be used as is (no copy of the content) by the window system.
     * You must not use a non-released buffer, else the user may see undefined content. */
    HRESULT (WINAPI *IsBufferReleased)(ID3DPresent *This, D3DWindowBuffer *buffer, BOOL *bReleased);
    /* It is possible buffers are not released in order */
    HRESULT (WINAPI *WaitOneBufferReleased)(ID3DPresent *This);
    /* TODO: see how to handle the case the FrontBuffer has different size */
    HRESULT (WINAPI *FrontBufferCopy)(ID3DPresent *This, D3DWindowBuffer *buffer);
    /* It is possible to do partial copy, but impossible to do resizing, which must
     * be done by the client after checking the front buffer size */
    HRESULT (WINAPI *PresentBuffer)(ID3DPresent *This, D3DWindowBuffer *buffer, HWND hWndOverride, const RECT *pSourceRect, const RECT *pDestRect, const RGNDATA *pDirtyRegion, DWORD Flags);
    HRESULT (WINAPI *GetRasterStatus)(ID3DPresent *This, D3DRASTER_STATUS *pRasterStatus);
    HRESULT (WINAPI *GetDisplayMode)(ID3DPresent *This, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation);
    HRESULT (WINAPI *GetPresentStats)(ID3DPresent *This, D3DPRESENTSTATS *pStats);
    HRESULT (WINAPI *GetCursorPos)(ID3DPresent *This, POINT *pPoint);
    HRESULT (WINAPI *SetCursorPos)(ID3DPresent *This, POINT *pPoint);
    /* Cursor size is always 32x32. pBitmap and pHotspot can be NULL. */
    HRESULT (WINAPI *SetCursor)(ID3DPresent *This, void *pBitmap, POINT *pHotspot, BOOL bShow);
    HRESULT (WINAPI *SetGammaRamp)(ID3DPresent *This, const D3DGAMMARAMP *pRamp, HWND hWndOverride);
    HRESULT (WINAPI *GetWindowSize)(ID3DPresent *This,  HWND hWnd, int *width, int *height);
} ID3DPresentVtbl;

/* Side notes, to delete after implementing:
 *
 * The D3DWindowBuffer and the buffer we render to are going to be different in three cases:
 * . In DRI_PRIME case, you render to a tiled buffer, and you share a buffer with no tiling
 * . In the case some resizing is needed, because the rendering buffer is smaller than the destination on the front buffer
 * . If the format we use to render failed to be used by NewBuffer. In this case we'll create a D3DWindowBuffer of different format.
 * In all the cases, that means we'll have a copy from the buffer we render to the buffer shared with the Window system.
 * Basically every backbuffer in these cases are going to have an additional buffer attached. Most of the time, Mesa will
 * manipulate the buffer we render to, but before the call to Present, these
 * cases will have to handled (blit + flush)
 *
 * There's also an additional case: when we don't use the discard mode, the back buffer content is supposed not to change.
 * That means we can't draw a hud on it for example. We probably want to handle this case the same way than above.
 *
 * Mesa is going to use WaitOneBufferReleased and test which one was released,
 * However that's probably ok only in discard mode, since perhaps the other modes assure that the buffers are rotated in
 * a defined order.
 *
 * I removed ForceBufferRelease. The backend is doing to do that automatically when needed.
 * 
 * I removed GetFrontBufferHandle. After all we don't know when Mesa is actually going to read it
 * and the content may change. We have to prefer a copy here.
 * 
 * I removed WaitBufferReleased. After all we are probably not going to use it, and it can be
 * implemented with calls to WaitOneBufferReleased and IsBufferReleased.
 *
 * Nine currently doesn't do throttling. This must be implemented to prevent input lag.
 * An example is the dri2 state tracker : you must check some pipe defined values on that
 * to know how many buffers can be in the rendering pipeline before we should care about throttling.
 * We should respect this number because it is a compromise between performance and input lag.
 * When we have too many buffers in the pipeline, we have to wait one is finished rendering before calling
 * PresentBuffer.
 */

struct ID3DPresent
{
    ID3DPresentVtbl *lpVtbl;
};

/* IUnknown macros */
#define ID3DPresent_QueryInterface(p,a,b) (p)->lpVtbl->QueryInterface(p,a,b)
#define ID3DPresent_AddRef(p) (p)->lpVtbl->AddRef(p)
#define ID3DPresent_Release(p) (p)->lpVtbl->Release(p)
/* ID3DPresent macros */
#define ID3DPresent_GetPresentParameters(p,a) (p)->lpVtbl->GetPresentParameters(p,a)
/* TODO: fill macros */
#define ID3DPresent_NewBuffer(p,a,b,c,d,e,f,g) (p)->lpVtbl->NewBuffer(p,a,b,c,d,e,f,g)
#define ID3DPresent_DestroyBuffer(p,a) (p)->lpVtbl->DestroyBuffer(p,a)
#define ID3DPresent_PresentBuffer(p,a,b,c,d,e,f) (p)->lpVtbl->PresentBuffer(p,a,b,c,d,e,f)
#define ID3DPresent_GetRasterStatus(p,a) (p)->lpVtbl->GetRasterStatus(p,a)
#define ID3DPresent_GetDisplayMode(p,a,b) (p)->lpVtbl->GetDisplayMode(p,a,b)
#define ID3DPresent_GetPresentStats(p,a) (p)->lpVtbl->GetPresentStats(p,a)
#define ID3DPresent_GetCursorPos(p,a) (p)->lpVtbl->GetCursorPos(p,a)
#define ID3DPresent_SetCursorPos(p,a) (p)->lpVtbl->SetCursorPos(p,a)
#define ID3DPresent_SetCursor(p,a,b,c) (p)->lpVtbl->SetCursor(p,a,b,c)
#define ID3DPresent_SetGammaRamp(p,a,b) (p)->lpVtbl->SetGammaRamp(p,a,b)
#define ID3DPresent_GetWindowSize(p,a,b,c) (p)->lpVtbl->GetWindowSize(p,a,b,c)

typedef struct ID3DPresentGroupVtbl
{
    /* IUnknown */
    HRESULT (WINAPI *QueryInterface)(ID3DPresentGroup *This, REFIID riid, void **ppvObject);
    ULONG (WINAPI *AddRef)(ID3DPresentGroup *This);
    ULONG (WINAPI *Release)(ID3DPresentGroup *This);

    /* ID3DPresentGroup */
    /* When creating a device, it's relevant for the driver to know how many
     * implicit swap chains to create. It has to create one per monitor in a
     * multi-monitor setup */
    UINT (WINAPI *GetMultiheadCount)(ID3DPresentGroup *This);
    /* returns only the implicit present interfaces */
    HRESULT (WINAPI *GetPresent)(ID3DPresentGroup *This, UINT Index, ID3DPresent **ppPresent);
    /* used to create additional presentation interfaces along the way */
    HRESULT (WINAPI *CreateAdditionalPresent)(ID3DPresentGroup *This, D3DPRESENT_PARAMETERS *pPresentationParameters, ID3DPresent **ppPresent);
} ID3DPresentGroupVtbl;

struct ID3DPresentGroup
{
    ID3DPresentGroupVtbl *lpVtbl;
};

/* IUnknown macros */
#define ID3DPresentGroup_QueryInterface(p,a,b) (p)->lpVtbl->QueryInterface(p,a,b)
#define ID3DPresentGroup_AddRef(p) (p)->lpVtbl->AddRef(p)
#define ID3DPresentGroup_Release(p) (p)->lpVtbl->Release(p)
/* ID3DPresentGroup */
#define ID3DPresentGroup_GetMultiheadCount(p) (p)->lpVtbl->GetMultiheadCount(p)
#define ID3DPresentGroup_GetPresent(p,a,b) (p)->lpVtbl->GetPresent(p,a,b)
#define ID3DPresentGroup_CreateAdditionalPresent(p,a,b) (p)->lpVtbl->CreateAdditionalPresent(p,a,b)

#else /* __cplusplus */

struct ID3DPresent : public IUnknown
{
    HRESULT WINAPI GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters);
    HRESULT WINAPI GetRasterStatus(D3DRASTER_STATUS *pRasterStatus);
    HRESULT WINAPI GetDisplayMode(D3DDISPLAYMODEEX *pMode);
    HRESULT WINAPI GetPresentStats(D3DPRESENTSTATS *pStats);
};

struct ID3DPresentGroup : public IUnknown
{
    UINT WINAPI GetMultiheadCount();
    HRESULT WINAPI GetPresent(UINT Index, ID3DPresent **ppPresent);
    HRESULT WINAPI CreateAdditionalPresent(D3DPRESENT_PARAMETERS *pPresentationParameters, ID3DPresent **ppPresent);
};

#endif /* __cplusplus */

#endif /* _D3DADAPTER_PRESENT_H_ */
