/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2015 Patrick Rudolph <siro@das-labor.org>
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

#ifndef _NINE_BUFFER9_H_
#define _NINE_BUFFER9_H_

#include "device9.h"
#include "nine_buffer_upload.h"
#include "nine_state.h"
#include "resource9.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/list.h"
#include "util/u_box.h"

struct pipe_screen;
struct pipe_context;
struct pipe_transfer;

struct NineTransfer {
    struct pipe_transfer *transfer;
    bool is_pipe_secondary;
    struct nine_subbuffer *buf; /* NULL unless subbuffer are used */
    bool should_destroy_buf; /* If the subbuffer should be destroyed */
};

struct NineBuffer9
{
    struct NineResource9 base;

    /* G3D */
    struct NineTransfer *maps;
    int nlocks, nmaps, maxmaps;
    UINT size;

    int16_t bind_count; /* to Device9->state.stream */
    /* Whether only discard and nooverwrite were used so far
     * for this buffer. Allows some optimization. */
    boolean discard_nooverwrite_only;
    boolean need_sync_if_nooverwrite;
    struct nine_subbuffer *buf;

    /* Specific to managed buffers */
    struct {
        void *data;
        boolean dirty;
        struct pipe_box dirty_box; /* region in the resource to update */
        struct pipe_box upload_pending_regions; /* region with uploads pending */
        struct list_head list; /* for update_buffers */
        struct list_head list2; /* for managed_buffers */
        struct list_head list3; /* for attached_dynamic_systemmem_vertex_buffers */
        unsigned pending_upload; /* for uploads */
        /* SYSTEMMEM DYNAMIC */
        bool discard_nooverwrite;
        bool discard_nooverwrite_noalign;
        unsigned nooverwrite_compatible_min_x;
        struct pipe_box valid_region;
        struct pipe_box required_valid_region;
    } managed;
};
static inline struct NineBuffer9 *
NineBuffer9( void *data )
{
    return (struct NineBuffer9 *)data;
}

HRESULT
NineBuffer9_ctor( struct NineBuffer9 *This,
                        struct NineUnknownParams *pParams,
                        D3DRESOURCETYPE Type,
                        DWORD Usage,
                        UINT Size,
                        D3DPOOL Pool );

void
NineBuffer9_dtor( struct NineBuffer9 *This );

struct pipe_resource *
NineBuffer9_GetResource( struct NineBuffer9 *This, unsigned *offset );

HRESULT NINE_WINAPI
NineBuffer9_Lock( struct NineBuffer9 *This,
                        UINT OffsetToLock,
                        UINT SizeToLock,
                        void **ppbData,
                        DWORD Flags );

HRESULT NINE_WINAPI
NineBuffer9_Unlock( struct NineBuffer9 *This );


static inline void
NineBuffer9_Upload( struct NineBuffer9 *This )
{
    struct NineDevice9 *device = This->base.base.device;
    /* Align the upload with the cache line (for WC)*/
    int start = (This->managed.dirty_box.x/64)*64;
    int upload_size = MIN2(This->size, ((This->managed.dirty_box.x+This->managed.dirty_box.width+63)/64)*64) - start;
    unsigned upload_flags = 0;
    struct pipe_box box;

    assert(This->base.pool != D3DPOOL_DEFAULT && This->managed.dirty);

    if (This->base.pool == D3DPOOL_SYSTEMMEM && This->base.usage & D3DUSAGE_DYNAMIC) {
        /* D3DPOOL_SYSTEMMEM D3DUSAGE_DYNAMIC buffers tend to be updated frequently in a round fashion
         * with no overlap for each lock (except obviously when coming back at the start of the
         * buffer. For more efficient uploads, use DISCARD/NOOVERWRITE. */
        if (This->managed.dirty_box.x == 0) {
            upload_flags |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
            u_box_1d(0, 0, &This->managed.valid_region);
            /* As we have discarded the resource, previous required valid regions
             * can be flushed */
            u_box_intersect_1d(&This->managed.required_valid_region,
                               &This->managed.required_valid_region, &This->managed.dirty_box);
            This->managed.discard_nooverwrite = true;
            This->managed.discard_nooverwrite_noalign = false;
            This->managed.nooverwrite_compatible_min_x = This->managed.dirty_box.width;
        } else if (This->managed.discard_nooverwrite && This->managed.nooverwrite_compatible_min_x <= This->managed.dirty_box.x) {
            upload_flags |= PIPE_MAP_UNSYNCHRONIZED;
            This->managed.nooverwrite_compatible_min_x = This->managed.dirty_box.x+This->managed.dirty_box.width;
            if (This->managed.discard_nooverwrite_noalign) {
                start = This->managed.dirty_box.x;
                upload_size = This->managed.dirty_box.width;
            }
        } else {
            /* One use incompatible with DISCARD/NOOVERWRITE. Disable until next discard */
            This->managed.discard_nooverwrite = false;
        }
        u_box_union_1d(&This->managed.valid_region, &This->managed.valid_region, &This->managed.dirty_box);
        u_box_union_1d(&box, &This->managed.valid_region, &This->managed.required_valid_region);
        /* If some required regions are missing, upload them too. */
        if (box.x != This->managed.valid_region.x || box.width != This->managed.valid_region.width) {
            unsigned stop = start + upload_size;
            if (box.x != This->managed.valid_region.x)
                start = box.x;
            if ((box.x + box.width) < (This->managed.valid_region.x + This->managed.valid_region.width))
                stop = box.x + box.width;
            upload_size = stop - start;
            This->managed.valid_region = box;
        }
    } else if (start == 0 && upload_size == This->size) {
        upload_flags |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
    }

    if (This->managed.pending_upload) {
        u_box_union_1d(&This->managed.upload_pending_regions,
                       &This->managed.upload_pending_regions,
                       &This->managed.dirty_box);
    } else {
        This->managed.upload_pending_regions = This->managed.dirty_box;
    }
    nine_context_range_upload(device, &This->managed.pending_upload,
                              (struct NineUnknown *)This,
                              This->base.resource,
                              start,
                              upload_size,
                              upload_flags,
                              (char *)This->managed.data + start);
    This->managed.dirty = FALSE;
}

static void inline
NineBindBufferToDevice( struct NineDevice9 *device,
                        struct NineBuffer9 **slot,
                        struct NineBuffer9 *buf )
{
    struct NineBuffer9 *old = *slot;

    if (buf) {
        if ((buf->managed.dirty) && list_is_empty(&buf->managed.list))
            list_add(&buf->managed.list, &device->update_buffers);
        buf->bind_count++;
    }
    if (old) {
        old->bind_count--;
        if (!old->bind_count && old->managed.dirty)
            list_delinit(&old->managed.list);
    }

    nine_bind(slot, buf);
}

void
NineBuffer9_SetDirty( struct NineBuffer9 *This );

#define BASEBUF_REGISTER_UPDATE(b) { \
    if ((b)->managed.dirty && (b)->bind_count) \
        if (list_is_empty(&(b)->managed.list)) \
            list_add(&(b)->managed.list, &(b)->base.base.device->update_buffers); \
    }

#endif /* _NINE_BUFFER9_H_ */
