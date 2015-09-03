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

#include "vertexbuffer9.h"
#include "device9.h"
#include "nine_helpers.h"
#include "nine_pipe.h"

#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "util/u_box.h"

#define DBG_CHANNEL DBG_VERTEXBUFFER

HRESULT
NineVertexBuffer9_ctor( struct NineVertexBuffer9 *This,
                        struct NineUnknownParams *pParams,
                        D3DVERTEXBUFFER_DESC *pDesc )
{
    struct pipe_resource *info = &This->base.info;
    HRESULT hr;

    DBG("This=%p Size=0x%x Usage=%x Pool=%u\n", This,
        pDesc->Size, pDesc->Usage, pDesc->Pool);

    user_assert(pDesc->Pool != D3DPOOL_SCRATCH, D3DERR_INVALIDCALL);

    This->flags = MALLOC(sizeof(DWORD));
    if (!This->flags)
        return E_OUTOFMEMORY;
    This->nmaps = 0;
    This->maxmaps = 1;

    This->pipe = pParams->device->pipe;

    info->screen = pParams->device->screen;
    info->target = PIPE_BUFFER;
    info->format = PIPE_FORMAT_R8_UNORM;
    info->width0 = pDesc->Size;
    info->flags = 0;

    info->bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_TRANSFER_WRITE;
    if (!(pDesc->Usage & D3DUSAGE_WRITEONLY))
        info->bind |= PIPE_BIND_TRANSFER_READ;

    info->usage = PIPE_USAGE_DEFAULT;
    if (pDesc->Usage & D3DUSAGE_DYNAMIC)
        info->usage = PIPE_USAGE_STREAM;
    if (pDesc->Pool == D3DPOOL_SYSTEMMEM)
        info->usage = PIPE_USAGE_STAGING;

    /* if (pDesc->Usage & D3DUSAGE_DONOTCLIP) { } */
    /* if (pDesc->Usage & D3DUSAGE_NONSECURE) { } */
    /* if (pDesc->Usage & D3DUSAGE_NPATCHES) { } */
    /* if (pDesc->Usage & D3DUSAGE_POINTS) { } */
    /* if (pDesc->Usage & D3DUSAGE_RTPATCHES) { } */
    if (pDesc->Usage & D3DUSAGE_SOFTWAREPROCESSING)
        DBG("Application asked for Software Vertex Processing, "
            "but this is unimplemented\n");
    /* if (pDesc->Usage & D3DUSAGE_TEXTAPI) { } */

    info->height0 = 1;
    info->depth0 = 1;
    info->array_size = 1;
    info->last_level = 0;
    info->nr_samples = 0;

    hr = NineResource9_ctor(&This->base, pParams, NULL, TRUE,
                            D3DRTYPE_VERTEXBUFFER, pDesc->Pool, pDesc->Usage);
    if (FAILED(hr))
        return hr;

    pDesc->Type = D3DRTYPE_VERTEXBUFFER;
    pDesc->Format = D3DFMT_VERTEXDATA;
    This->desc = *pDesc;

    This->data = align_malloc(pDesc->Size, 32);
    if (!This->data)
        return E_OUTOFMEMORY;

    u_box_1d(0, This->desc.Size, &This->box);

    return D3D_OK;
}

void
NineVertexBuffer9_dtor( struct NineVertexBuffer9 *This )
{
    if (This->flags) {
        while (This->nmaps) {
            NineVertexBuffer9_Unlock(This);
        }
        FREE(This->flags);
    }

    if (This->data)
        FREE(This->data);

    NineResource9_dtor(&This->base);
}

HRESULT WINAPI
NineVertexBuffer9_Lock( struct NineVertexBuffer9 *This,
                        UINT OffsetToLock,
                        UINT SizeToLock,
                        void **ppbData,
                        DWORD Flags )
{
    DBG("This=%p(pipe=%p) OffsetToLock=0x%x, SizeToLock=0x%x, Flags=0x%x\n",
        This, This->base.resource,
        OffsetToLock, SizeToLock, Flags);

    user_assert(OffsetToLock < This->desc.Size, D3DERR_INVALIDCALL);
    user_assert(ppbData, E_POINTER);
    user_assert(!(Flags & ~(D3DLOCK_DISCARD |
                            D3DLOCK_DONOTWAIT |
                            D3DLOCK_NO_DIRTY_UPDATE |
                            D3DLOCK_NOSYSLOCK |
                            D3DLOCK_READONLY |
                            D3DLOCK_NOOVERWRITE)), D3DERR_INVALIDCALL);

    if (This->nmaps == This->maxmaps) {
        struct map *newmaps =
            REALLOC(This->flags, sizeof(DWORD)*This->maxmaps,
                    sizeof(DWORD)*(This->maxmaps << 1));
        if (newmaps == NULL)
            return E_OUTOFMEMORY;

        This->maxmaps <<= 1;
        This->flags = newmaps;
    }

    if (SizeToLock == 0) {
        SizeToLock = This->desc.Size - OffsetToLock;
        user_warn(OffsetToLock != 0);
    }
    This->flags[This->nmaps] = Flags;

    This->nmaps++;
    *ppbData = ((unsigned char*)This->data) + OffsetToLock;

    return D3D_OK;
}

HRESULT WINAPI
NineVertexBuffer9_Unlock( struct NineVertexBuffer9 *This )
{
    DBG("This=%p\n", This);
    unsigned usage = PIPE_TRANSFER_WRITE;

    user_assert(This->nmaps > 0, D3DERR_INVALIDCALL);
    This->nmaps--;

    if (This->flags[This->nmaps] & D3DLOCK_READONLY)
        return D3D_OK;

    if (This->flags[This->nmaps] & D3DLOCK_DISCARD)
        usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;

    This->pipe->transfer_inline_write(This->pipe, This->base.resource, 0, usage, &This->box, This->data, 0, 0 );
    return D3D_OK;
}

HRESULT WINAPI
NineVertexBuffer9_GetDesc( struct NineVertexBuffer9 *This,
                           D3DVERTEXBUFFER_DESC *pDesc )
{
    user_assert(pDesc, E_POINTER);
    *pDesc = This->desc;
    return D3D_OK;
}

IDirect3DVertexBuffer9Vtbl NineVertexBuffer9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)NineResource9_SetPrivateData,
    (void *)NineResource9_GetPrivateData,
    (void *)NineResource9_FreePrivateData,
    (void *)NineResource9_SetPriority,
    (void *)NineResource9_GetPriority,
    (void *)NineResource9_PreLoad,
    (void *)NineResource9_GetType,
    (void *)NineVertexBuffer9_Lock,
    (void *)NineVertexBuffer9_Unlock,
    (void *)NineVertexBuffer9_GetDesc
};

static const GUID *NineVertexBuffer9_IIDs[] = {
    &IID_IDirect3DVertexBuffer9,
    &IID_IDirect3DResource9,
    &IID_IUnknown,
    NULL
};

HRESULT
NineVertexBuffer9_new( struct NineDevice9 *pDevice,
                       D3DVERTEXBUFFER_DESC *pDesc,
                       struct NineVertexBuffer9 **ppOut )
{
    NINE_DEVICE_CHILD_NEW(VertexBuffer9, ppOut, /* args */ pDevice, pDesc);
}
