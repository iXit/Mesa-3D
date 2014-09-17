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

#include "swapchain9.h"
#include "surface9.h"
#include "device9.h"

#include "nine_helpers.h"
#include "nine_pipe.h"
#include "nine_dump.h"

#include "util/u_inlines.h"
#include "util/u_surface.h"
#include "hud/hud_context.h"
#include "state_tracker/drm_driver.h"

#define DBG_CHANNEL DBG_SWAPCHAIN

HRESULT
NineSwapChain9_ctor( struct NineSwapChain9 *This,
                     struct NineUnknownParams *pParams,
                     BOOL implicit,
                     ID3DPresent *pPresent,
                     struct d3dadapter9_context *pCTX,
                     HWND hFocusWindow )
{
    D3DPRESENT_PARAMETERS params;
    HRESULT hr;

    DBG("This=%p pDevice=%p pPresent=%p pCTX=%p hFocusWindow=%p\n",
        This, pParams->device, pPresent, pCTX, hFocusWindow);

    hr = NineUnknown_ctor(&This->base, pParams);
    if (FAILED(hr))
        return hr;

    This->screen = NineDevice9_GetScreen(This->base.device);
    This->pipe = NineDevice9_GetPipe(This->base.device);
    This->cso = NineDevice9_GetCSO(This->base.device);
    This->implicit = implicit;
    This->actx = pCTX;
    This->present = pPresent;
    ID3DPresent_AddRef(pPresent);
    hr = ID3DPresent_GetPresentParameters(This->present, &params);
    if (FAILED(hr))
        return hr;
    if (!params.hDeviceWindow)
        params.hDeviceWindow = hFocusWindow;

    This->rendering_done = FALSE;
    return NineSwapChain9_Resize(This, &params);
}

HRESULT
NineSwapChain9_Resize( struct NineSwapChain9 *This,
                       D3DPRESENT_PARAMETERS *pParams )
{
    struct NineDevice9 *pDevice = This->base.device;
    struct NineSurface9 **bufs;
    D3DSURFACE_DESC desc;
    HRESULT hr;
    struct pipe_resource *resource, tmplt;
    enum pipe_format pf;
    struct winsys_handle whandle;
    BOOL has_present_buffers = FALSE;
    int stride, dmaBufFd, depth;
    unsigned i, oldBufferCount, newBufferCount;

    DBG("This=%p pParams=%p\n", This, pParams);
    user_assert(pParams != NULL, E_POINTER);

    DBG("pParams(%p):\n"
        "BackBufferWidth: %u\n"
        "BackBufferHeight: %u\n"
        "BackBufferFormat: %s\n"
        "BackBufferCount: %u\n"
        "MultiSampleType: %u\n"
        "MultiSampleQuality: %u\n"
        "SwapEffect: %u\n"
        "hDeviceWindow: %p\n"
        "Windowed: %i\n"
        "EnableAutoDepthStencil: %i\n"
        "AutoDepthStencilFormat: %s\n"
        "Flags: %s\n"
        "FullScreen_RefreshRateInHz: %u\n"
        "PresentationInterval: %x\n", pParams,
        pParams->BackBufferWidth, pParams->BackBufferHeight,
        d3dformat_to_string(pParams->BackBufferFormat),
        pParams->BackBufferCount,
        pParams->MultiSampleType, pParams->MultiSampleQuality,
        pParams->SwapEffect, pParams->hDeviceWindow, pParams->Windowed,
        pParams->EnableAutoDepthStencil,
        d3dformat_to_string(pParams->AutoDepthStencilFormat),
        nine_D3DPRESENTFLAG_to_str(pParams->Flags),
        pParams->FullScreen_RefreshRateInHz,
        pParams->PresentationInterval);

    if (pParams->BackBufferFormat == D3DFMT_UNKNOWN)
        pParams->BackBufferFormat = This->params.BackBufferFormat;
    if (pParams->EnableAutoDepthStencil &&
        This->params.EnableAutoDepthStencil &&
        pParams->AutoDepthStencilFormat == D3DFMT_UNKNOWN)
        pParams->AutoDepthStencilFormat = This->params.AutoDepthStencilFormat;
    /* NULL means focus window.
    if (!pParams->hDeviceWindow && This->params.hDeviceWindow)
        pParams->hDeviceWindow = This->params.hDeviceWindow;
    */
    if (pParams->BackBufferCount == 0)
        pParams->BackBufferCount = 1; /* ref MSDN */

    oldBufferCount = This->params.BackBufferCount ? This->params.BackBufferCount + (This->params.SwapEffect != D3DSWAPEFFECT_COPY) : 0;
    newBufferCount = pParams->BackBufferCount + (This->params.SwapEffect != D3DSWAPEFFECT_COPY);

    if (pParams->BackBufferWidth == 0 || pParams->BackBufferHeight == 0) {
        int width, height;
        if (!pParams->Windowed)
            return D3DERR_INVALIDCALL;
        if (FAILED(ID3DPresent_GetWindowSize(This->present, NULL, &width, &height))) {
            width = This->params.BackBufferWidth;
            height = This->params.BackBufferHeight;
        }
        if (!pParams->BackBufferWidth)
            pParams->BackBufferWidth = width;
        if (!pParams->BackBufferHeight)
            pParams->BackBufferHeight = height;
    }

    pf = d3d9_to_pipe_format(pParams->BackBufferFormat);
    if (This->actx->linear_framebuffer ||
        (pf != PIPE_FORMAT_B8G8R8X8_UNORM &&
        pf != PIPE_FORMAT_B8G8R8A8_UNORM) ||
        pParams->SwapEffect != D3DSWAPEFFECT_DISCARD) {
        has_present_buffers = TRUE;
    }
    depth = 24; /* TODO handle also 16 bit */

    tmplt.target = PIPE_TEXTURE_2D;
    tmplt.width0 = pParams->BackBufferWidth;
    tmplt.height0 = pParams->BackBufferHeight;
    tmplt.depth0 = 1;
    tmplt.nr_samples = pParams->MultiSampleType;
    tmplt.last_level = 0;
    tmplt.array_size = 1;
    tmplt.usage = PIPE_USAGE_DEFAULT;
    tmplt.flags = 0;

    desc.Type = D3DRTYPE_SURFACE;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSampleType = pParams->MultiSampleType;
    desc.MultiSampleQuality = 0;
    desc.Width = pParams->BackBufferWidth;
    desc.Height = pParams->BackBufferHeight;

    for (i = 0; i < oldBufferCount; i++) {
        ID3DPresent_DestroyBuffer(This->present, This->present_handles[i]);
        This->present_handles[i] = NULL;
        if (This->present_buffers)
            pipe_resource_reference((struct pipe_resource **)(This->present_buffers + i * sizeof(struct pipe_resource *)), NULL);
    }

    if (!has_present_buffers && This->present_buffers) {
        FREE(This->present_buffers);
        This->present_buffers = NULL;
    }
    if (newBufferCount != oldBufferCount) {
        for (i = newBufferCount; i < oldBufferCount;
             ++i)
            NineUnknown_Detach(NineUnknown(This->buffers[i]));

        bufs = REALLOC(This->buffers,
                       oldBufferCount * sizeof(This->buffers[0]),
                       newBufferCount * sizeof(This->buffers[0]));
        if (!bufs)
            return E_OUTOFMEMORY;
        This->buffers = bufs;
        if (has_present_buffers) {
            This->present_buffers = REALLOC(This->present_buffers,
                                            This->present_buffers == NULL ? 0 : oldBufferCount * sizeof(struct pipe_resource *),
                                            newBufferCount * sizeof(struct pipe_resource *));
            memset(This->present_buffers, 0, newBufferCount * sizeof(struct pipe_resource *));
        }
        This->present_handles = REALLOC(This->present_handles,
                                        oldBufferCount * sizeof(D3DWindowBuffer *),
                                        newBufferCount * sizeof(D3DWindowBuffer *));
        for (i = oldBufferCount; i < newBufferCount; ++i) {
            This->buffers[i] = NULL;
            This->present_handles[i] = NULL;
        }
    }

    for (i = 0; i < newBufferCount; ++i) {
        tmplt.format = d3d9_to_pipe_format(pParams->BackBufferFormat);
        tmplt.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_TRANSFER_READ |
                     PIPE_BIND_TRANSFER_WRITE | PIPE_BIND_RENDER_TARGET;
        if (!has_present_buffers)
            tmplt.bind |= PIPE_BIND_SHARED | PIPE_BIND_SCANOUT;
        resource = This->screen->resource_create(This->screen, &tmplt);
        if (!resource) {
            DBG("Failed to create pipe_resource.\n");
            return D3DERR_OUTOFVIDEOMEMORY;
        }
        if (pParams->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER)
            resource->flags |= NINE_RESOURCE_FLAG_LOCKABLE;
        if (This->buffers[i]) {
            NineSurface9_SetResourceResize(This->buffers[i], resource);
            if (has_present_buffers)
                pipe_resource_reference(&resource, NULL);
        } else {
            desc.Format = pParams->BackBufferFormat;
            desc.Usage = D3DUSAGE_RENDERTARGET;
            hr = NineSurface9_new(pDevice, NineUnknown(This), resource, 0,
                                  0, 0, &desc, &This->buffers[i]);
            if (has_present_buffers)
                pipe_resource_reference(&resource, NULL);
            if (FAILED(hr)) {
                DBG("Failed to create RT surface.\n");
                return hr;
            }
            This->buffers[i]->base.base.forward = FALSE;
        }
        if (has_present_buffers) {
            tmplt.format = PIPE_FORMAT_B8G8R8X8_UNORM;
            tmplt.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT;
            if (This->actx->linear_framebuffer)
                tmplt.bind |= PIPE_BIND_LINEAR;
            if (pParams->SwapEffect != D3DSWAPEFFECT_DISCARD)
                tmplt.bind |= PIPE_BIND_RENDER_TARGET;
            resource = This->screen->resource_create(This->screen, &tmplt);
            pipe_resource_reference((struct pipe_resource **)(This->present_buffers + i * sizeof(struct pipe_resource *)), resource);
        }
        memset(&whandle, 0, sizeof(whandle));
        whandle.type = DRM_API_HANDLE_TYPE_FD;
        This->screen->resource_get_handle(This->screen, resource, &whandle);
        stride = whandle.stride;
        dmaBufFd = whandle.handle;
        ID3DPresent_NewBuffer(This->present,
                              dmaBufFd,
                              resource->width0,
                              resource->height0,
                              stride,
                              depth,
                              32,
                              &(This->present_handles[i])
                              );
        if (!has_present_buffers)
            pipe_resource_reference(&resource, NULL);
    }
    if (pParams->EnableAutoDepthStencil) {
        tmplt.format = d3d9_to_pipe_format(pParams->AutoDepthStencilFormat);
        tmplt.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_TRANSFER_READ |
                     PIPE_BIND_TRANSFER_WRITE | PIPE_BIND_DEPTH_STENCIL;

        resource = This->screen->resource_create(This->screen, &tmplt);
        if (!resource) {
            DBG("Failed to create pipe_resource for depth buffer.\n");
            return D3DERR_OUTOFVIDEOMEMORY;
        }
        if (This->zsbuf) {
            NineSurface9_SetResourceResize(This->zsbuf, resource);
            pipe_resource_reference(&resource, NULL);
        } else {
            /* XXX wine thinks the container of this should be the device */
            desc.Format = pParams->AutoDepthStencilFormat;
            desc.Usage = D3DUSAGE_DEPTHSTENCIL;
            hr = NineSurface9_new(pDevice, NineUnknown(pDevice), resource, 0,
                                  0, 0, &desc, &This->zsbuf);
            pipe_resource_reference(&resource, NULL);
            if (FAILED(hr)) {
                DBG("Failed to create ZS surface.\n");
                return hr;
            }
            This->zsbuf->base.base.forward = FALSE;
        }
    }

    This->params = *pParams;

    return D3D_OK;
}

void
NineSwapChain9_dtor( struct NineSwapChain9 *This )
{
    unsigned i;

    DBG("This=%p\n", This);

    if (This->buffers) {
        for (i = 0; i < This->params.BackBufferCount; i++) {
            NineUnknown_Destroy(NineUnknown(This->buffers[i]));
            ID3DPresent_DestroyBuffer(This->present, This->present_handles[i]);
            if (This->present_buffers)
                pipe_resource_reference((struct pipe_resource **)(This->present_buffers + i * sizeof(struct pipe_resource *)), NULL);
        }
        FREE(This->buffers);
        FREE(This->present_buffers);
    }
    if (This->zsbuf)
        NineUnknown_Destroy(NineUnknown(This->zsbuf));

    if (This->present)
        ID3DPresent_Release(This->present);

    NineUnknown_dtor(&This->base);
}

static void handle_draw_cursor_and_hud( struct NineSwapChain9 *This, struct pipe_resource *resource, int resource_level )
{
    struct NineDevice9 *device = This->base.device;
    struct pipe_blit_info blit;

     if (device->cursor.software && device->cursor.visible && device->cursor.w) {
        blit.src.resource = device->cursor.image;
        blit.src.level = 0;
        blit.src.format = device->cursor.image->format;
        blit.src.box.x = 0;
        blit.src.box.y = 0;
        blit.src.box.z = 0;
        blit.src.box.depth = 1;
        blit.src.box.width = device->cursor.w;
        blit.src.box.height = device->cursor.h;

        blit.dst.resource = resource;
        blit.dst.level = resource_level;
        blit.dst.format = resource->format;
        blit.dst.box.z = 0;
        blit.dst.box.depth = 1;

        blit.mask = PIPE_MASK_RGBA;
        blit.filter = PIPE_TEX_FILTER_NEAREST;
        blit.scissor_enable = FALSE;
        blit.alpha_blend = FALSE;

        ID3DPresent_GetCursorPos(This->present, &device->cursor.pos);

        /* NOTE: blit messes up when box.x + box.width < 0, fix driver */
        blit.dst.box.x = MAX2(device->cursor.pos.x, 0) - device->cursor.hotspot.x;
        blit.dst.box.y = MAX2(device->cursor.pos.y, 0) - device->cursor.hotspot.y;
        blit.dst.box.width = blit.src.box.width;
        blit.dst.box.height = blit.src.box.height;

        DBG("Blitting cursor(%ux%u) to (%i,%i).\n",
            blit.src.box.width, blit.src.box.height,
            blit.dst.box.x, blit.dst.box.y);

        blit.alpha_blend = TRUE;
        This->pipe->blit(This->pipe, &blit);
    }

    if (device->hud && resource) {
        hud_draw(device->hud, resource); /* XXX: no offset */
        /* HUD doesn't clobber stipple */
        NineDevice9_RestoreNonCSOState(device, ~0x2);
    }
}

static INLINE HRESULT
present( struct NineSwapChain9 *This,
         const RECT *pSourceRect,
         const RECT *pDestRect,
         HWND hDestWindowOverride,
         const RGNDATA *pDirtyRegion,
         DWORD dwFlags )
{
    struct pipe_resource *resource;
    HRESULT hr;
    struct pipe_blit_info blit;

    DBG(">>>\npresent: This=%p pSourceRect=%p pDestRect=%p "
        "pDirtyRegion=%p",
        This, pSourceRect, pDestRect, pDirtyRegion);
    if (pSourceRect)
        DBG("pSourceRect = (%u..%u)x(%u..%u)\n",
            pSourceRect->left, pSourceRect->right,
            pSourceRect->top, pSourceRect->bottom);
    if (pDestRect)
        DBG("pDestRect = (%u..%u)x(%u..%u)\n",
            pDestRect->left, pDestRect->right,
            pDestRect->top, pDestRect->bottom);

    /* TODO: in the case the source and destination rect have different size:
     * We need to allocate a new buffer, and do a blit to it to resize.
     * We can't use the present_buffer for that since when we created it,
     * we couldn't guess which size would have been needed.
     * If pDestRect or pSourceRect is null, we have to check the sizes
     * from the source size, and the destination window size.
     * In this case, either resize rngdata, or pass NULL instead
     */
    /* TODO: This->buffers[0]->level != 0 will always need a copy */

    if (This->rendering_done)
        goto bypass_rendering;

    resource = This->buffers[0]->base.resource;
    if (This->params.SwapEffect == D3DSWAPEFFECT_DISCARD)
        handle_draw_cursor_and_hud(This, resource, This->buffers[0]->level);

    if (This->present_buffers) {
        blit.src.resource = resource;
        blit.src.level = This->buffers[0]->level;
        blit.src.format = resource->format;
        blit.src.box.z = 0;
        blit.src.box.depth = 1;
        blit.src.box.x = 0;
        blit.src.box.y = 0;
        blit.src.box.width = resource->width0;
        blit.src.box.height = resource->height0;

        resource = This->present_buffers[0];

        blit.dst.resource = resource;
        blit.dst.level = 0;
        blit.dst.format = resource->format;
        blit.dst.box.z = 0;
        blit.dst.box.depth = 1;
        blit.dst.box.x = 0;
        blit.dst.box.y = 0;
        blit.dst.box.width = resource->width0;
        blit.dst.box.height = resource->height0;

        blit.mask = PIPE_MASK_RGBA;
        blit.filter = PIPE_TEX_FILTER_NEAREST;
        blit.scissor_enable = FALSE;
        blit.alpha_blend = FALSE;

        This->pipe->blit(This->pipe, &blit);
    }

    if (This->params.SwapEffect != D3DSWAPEFFECT_DISCARD)
        handle_draw_cursor_and_hud(This, resource, 0);


    This->pipe->flush(This->pipe, NULL, PIPE_FLUSH_END_OF_FRAME);

    /* TODO: to implement the behaviour of Present on Windows,
     * it seems we should wait the last buffer Presented has been
     * rendered. This should have the side effect of implementing throttling,
     * which decreases input lag (see dri2 state tracker for an alternative implementation).
     * If the flag D3DPRESENT_DONOTWAIT is set, we have to return D3DERR_WASSTILLDRAWING if we would have to wait.
     * Note that the following Present call can also return that, and we'll have to care about that.
    /* really present the frame */
    This->rendering_done = TRUE;
bypass_rendering:
    hr = ID3DPresent_PresentBuffer(This->present, This->present_handles[0], hDestWindowOverride, pSourceRect, pDestRect, pDirtyRegion, dwFlags);

    if (FAILED(hr)) { return hr; }

    This->rendering_done = FALSE;

    return D3D_OK;
}

HRESULT WINAPI
NineSwapChain9_Present( struct NineSwapChain9 *This,
                        const RECT *pSourceRect,
                        const RECT *pDestRect,
                        HWND hDestWindowOverride,
                        const RGNDATA *pDirtyRegion,
                        DWORD dwFlags )
{
    struct pipe_resource *res = NULL;
    D3DWindowBuffer *handle_temp;
    int i;
    HRESULT hr = present(This, pSourceRect, pDestRect,
                         hDestWindowOverride, pDirtyRegion, dwFlags);

    if (hr == D3DERR_WASSTILLDRAWING)
        return hr;

    switch (This->params.SwapEffect) {
        case D3DSWAPEFFECT_DISCARD:
        case D3DSWAPEFFECT_FLIP:
            /* rotate the queue */;
            pipe_resource_reference(&res, This->buffers[0]->base.resource);
            for (i = 1; i <= This->params.BackBufferCount; i++) {
                NineSurface9_SetResourceResize(This->buffers[i - 1],
                                               This->buffers[i]->base.resource);
            }
            NineSurface9_SetResourceResize(
                This->buffers[This->params.BackBufferCount], res);
            pipe_resource_reference(&res, NULL);

            if (This->present_buffers) {
                pipe_resource_reference(&res, This->present_buffers[0]);
                for (i = 1; i <= This->params.BackBufferCount; i++)
                    pipe_resource_reference((struct pipe_resource **)(This->present_buffers + (i-1) * sizeof(struct pipe_resource *)), This->present_buffers[i]);
                pipe_resource_reference((struct pipe_resource **)(This->present_buffers + This->params.BackBufferCount * sizeof(struct pipe_resource *)), res);
                pipe_resource_reference(&res, NULL);
            }

            handle_temp = This->present_handles[0];
            for (i = 1; i <= This->params.BackBufferCount; i++) {
                This->present_handles[i-1] = This->present_handles[i];
            }
            This->present_handles[This->params.BackBufferCount] = handle_temp;
            break;

        case D3DSWAPEFFECT_COPY:
            /* do nothing */
            break;

        case D3DSWAPEFFECT_OVERLAY:
            /* XXX not implemented */
            break;

        case D3DSWAPEFFECT_FLIPEX:
            /* XXX not implemented */
            break;
    }
    /* TODO: here wait This->buffers[0] is released */
    This->base.device->state.changed.group |= NINE_STATE_FB;
    nine_update_state(This->base.device, NINE_STATE_FB);

    return hr;
}

HRESULT WINAPI
NineSwapChain9_GetFrontBufferData( struct NineSwapChain9 *This,
                                   IDirect3DSurface9 *pDestSurface )
{
    /* TODO: GetFrontBuffer() and copy the contents */
    STUB(D3DERR_INVALIDCALL);
}

HRESULT WINAPI
NineSwapChain9_GetBackBuffer( struct NineSwapChain9 *This,
                              UINT iBackBuffer,
                              D3DBACKBUFFER_TYPE Type,
                              IDirect3DSurface9 **ppBackBuffer )
{
    (void)user_error(Type == D3DBACKBUFFER_TYPE_MONO);
    user_assert(iBackBuffer < This->params.BackBufferCount, D3DERR_INVALIDCALL);
    user_assert(ppBackBuffer != NULL, E_POINTER);

    NineUnknown_AddRef(NineUnknown(This->buffers[iBackBuffer]));
    *ppBackBuffer = (IDirect3DSurface9 *)This->buffers[iBackBuffer];
    return D3D_OK;
}

HRESULT WINAPI
NineSwapChain9_GetRasterStatus( struct NineSwapChain9 *This,
                                D3DRASTER_STATUS *pRasterStatus )
{
    user_assert(pRasterStatus != NULL, E_POINTER);
    return ID3DPresent_GetRasterStatus(This->present, pRasterStatus);
}

HRESULT WINAPI
NineSwapChain9_GetDisplayMode( struct NineSwapChain9 *This,
                               D3DDISPLAYMODE *pMode )
{
    D3DDISPLAYMODEEX mode;
    D3DDISPLAYROTATION rot;
    HRESULT hr;

    user_assert(pMode != NULL, E_POINTER);

    hr = ID3DPresent_GetDisplayMode(This->present, &mode, &rot);
    if (SUCCEEDED(hr)) {
        pMode->Width = mode.Width;
        pMode->Height = mode.Height;
        pMode->RefreshRate = mode.RefreshRate;
        pMode->Format = mode.Format;
    }
    return hr;
}

HRESULT WINAPI
NineSwapChain9_GetPresentParameters( struct NineSwapChain9 *This,
                                     D3DPRESENT_PARAMETERS *pPresentationParameters )
{
    user_assert(pPresentationParameters != NULL, E_POINTER);
    *pPresentationParameters = This->params;
    return D3D_OK;
}

IDirect3DSwapChain9Vtbl NineSwapChain9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineSwapChain9_Present,
    (void *)NineSwapChain9_GetFrontBufferData,
    (void *)NineSwapChain9_GetBackBuffer,
    (void *)NineSwapChain9_GetRasterStatus,
    (void *)NineSwapChain9_GetDisplayMode,
    (void *)NineUnknown_GetDevice, /* actually part of SwapChain9 iface */
    (void *)NineSwapChain9_GetPresentParameters
};

static const GUID *NineSwapChain9_IIDs[] = {
    &IID_IDirect3DSwapChain9,
    &IID_IUnknown,
    NULL
};

HRESULT
NineSwapChain9_new( struct NineDevice9 *pDevice,
                    BOOL implicit,
                    ID3DPresent *pPresent,
                    struct d3dadapter9_context *pCTX,
                    HWND hFocusWindow,
                    struct NineSwapChain9 **ppOut )
{
    NINE_DEVICE_CHILD_NEW(SwapChain9, ppOut, pDevice, /* args */
                          implicit, pPresent, pCTX, hFocusWindow);
}
