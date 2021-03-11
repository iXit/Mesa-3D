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
        bool can_unsynchronized; /* Whether the upload can use nooverwrite */
        struct pipe_box valid_region; /* Region in the GPU buffer with valid content */
        struct pipe_box required_valid_region; /* Region that needs to be valid right now. */
        struct pipe_box filled_region; /* Region in the GPU buffer filled since last discard */
        unsigned frame_count_last_discard;
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

/* Try to remove b from a, supposed to include b */
static void u_box_try_remove_region_1d(struct pipe_box *dst,
                                       const struct pipe_box *a,
                                       const struct pipe_box *b)
{
    int x, width;
    if (a->x == b->x) {
        x = a->x + b->width;
        width = a->width - b->width;
    } else if ((a->x + a->width) == (b->x + b->width)) {
        x = a->x;
        width = a->width - b->width;
    } else {
        x = a->x;
        width = a->width;
    }
    dst->x = x;
    dst->width = width;
}

static inline void
NineBuffer9_Upload( struct NineBuffer9 *This )
{
    struct NineDevice9 *device = This->base.base.device;
    int start, upload_size;
    unsigned upload_flags = 0;
    struct pipe_box box_upload;

    assert(This->base.pool != D3DPOOL_DEFAULT && This->managed.dirty);

    if (This->base.pool == D3DPOOL_SYSTEMMEM && This->base.usage & D3DUSAGE_DYNAMIC) {
        struct pipe_box region_already_valid;
        struct pipe_box conflicting_region;
        struct pipe_box *valid_region = &This->managed.valid_region;
        struct pipe_box *required_valid_region = &This->managed.required_valid_region;
        struct pipe_box *filled_region = &This->managed.filled_region;
        /* Try to upload SYSTEMMEM DYNAMIC in an efficient fashion.
         * Unlike non-dynamic for which we upload the whole dirty region, try to
         * only upload the data needed for the draw. The draw call preparation
         * fills This->managed.required_valid_region for that */
        u_box_intersect_1d(&region_already_valid,
                           valid_region,
                           required_valid_region);
        /* If the required valid region is already valid, nothing to do */
        if (region_already_valid.x == required_valid_region->x &&
            region_already_valid.width == required_valid_region->width)
            return;
        /* (Try to) Remove valid areas from the region to upload */
        u_box_try_remove_region_1d(&box_upload,
                                   required_valid_region,
                                   &region_already_valid);
        assert(box_upload.width > 0);
        /* To maintain correctly the valid region, as we will do union later with
         * box_upload, we must ensure box_upload is consecutive with valid_region */
        if (box_upload.x > valid_region->x + valid_region->width && valid_region->width > 0) {
            box_upload.width = box_upload.x + box_upload.width - (valid_region->x + valid_region->width);
            box_upload.x = valid_region->x + valid_region->width;
        } else if (box_upload.x + box_upload.width < valid_region->x && valid_region->width > 0) {
            box_upload.width = valid_region->x - box_upload.x;
        }
        /* There is conflict if some areas, that are not valid but are filled for previous draw calls,
         * intersect with the region we plan to upload. Note by construction valid_region IS
         * included in filled_region, thus so is region_already_valid. */
        u_box_intersect_1d(&conflicting_region, &box_upload, filled_region);
        /* As box_upload could still contain region_already_valid, check the intersection
         * doesn't happen to be exactly region_already_valid (it cannot be smaller, see above) */
        if (This->managed.can_unsynchronized && (conflicting_region.width == 0 ||
            (conflicting_region.x == region_already_valid.x &&
             conflicting_region.width == region_already_valid.width))) {
            /* No conflicts. */
            upload_flags |= PIPE_MAP_UNSYNCHRONIZED;
        } else {
            /* We cannot use PIPE_MAP_UNSYNCHRONIZED. We must choose between no flag and DISCARD.
             * Criterias to discard:
             * . Most of the resource was filled (but some apps do allocate a big buffer
             * to only use a small part in a round fashion)
             * . The region to upload is very small compared to the filled region and
             * at the start of the buffer (hints at round usage starting again)
             * . The region to upload is very big compared to the required region
             * . We have not discarded yet this frame */
            if (filled_region->width > (This->size / 2) ||
                (10 * box_upload.width < filled_region->width &&
                 box_upload.x < (filled_region->x + filled_region->width)/2) ||
                box_upload.width > 2 * required_valid_region->width ||
                This->managed.frame_count_last_discard != device->frame_count) {
                /* Avoid DISCARDING too much by discarding only if most of the buffer
                 * has been used */
                DBG_FLAG(DBG_INDEXBUFFER|DBG_VERTEXBUFFER,
             "Uploading %p DISCARD: valid %d %d, filled %d %d, required %d %d, box_upload %d %d, required already_valid %d %d, conficting %d %d\n",
             This, valid_region->x, valid_region->width, filled_region->x, filled_region->width,
             required_valid_region->x, required_valid_region->width, box_upload.x, box_upload.width,
             region_already_valid.x, region_already_valid.width, conflicting_region.x, conflicting_region.width
                );
                upload_flags |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
                u_box_1d(0, 0, filled_region);
                u_box_1d(0, 0, valid_region);
                box_upload = This->managed.required_valid_region;
                This->managed.can_unsynchronized = true;
                This->managed.frame_count_last_discard = device->frame_count;
            } else {
                /* Once we use without UNSYNCHRONIZED, we cannot use it anymore */
                This->managed.can_unsynchronized = false;
            }
        }

        u_box_union_1d(filled_region,
                       filled_region,
                       &box_upload);
        u_box_union_1d(valid_region,
                       valid_region,
                       &box_upload);
        start = box_upload.x;
        upload_size = box_upload.width;
    } else {
        box_upload = This->managed.dirty_box;
        /* Align the upload with the cache line (for WC)*/
        start = (box_upload.x/64)*64;
        upload_size = MIN2(This->size, ((box_upload.x+box_upload.width+63)/64)*64) - start;
    }

    if (start == 0 && upload_size == This->size) {
        upload_flags |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
    }

    if (This->managed.pending_upload) {
        u_box_union_1d(&This->managed.upload_pending_regions,
                       &This->managed.upload_pending_regions,
                       &box_upload);
    } else {
        This->managed.upload_pending_regions = box_upload;
    }

    DBG_FLAG(DBG_INDEXBUFFER|DBG_VERTEXBUFFER,
             "Uploading %p, offset=%d, size=%d, Flags=0x%x\n",
             This, start, upload_size, upload_flags);
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
