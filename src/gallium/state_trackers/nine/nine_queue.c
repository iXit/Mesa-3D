/*
 * Copyright 2016 Patrick Rudolph <siro@das-labor.org>
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

#include "nine_queue.h"
#include "os/os_thread.h"
#include "util/macros.h"
#include "nine_helpers.h"

#define NINE_QUEUES (32)
#define NINE_QUEUES_MASK (NINE_QUEUES - 1)

#define NINE_QUEUE_SIZE (4352)

#define DBG_CHANNEL DBG_DEVICE

/*
 * Single producer - single consumer pool queue
 *
 * Producer:
 *  Allocates slices of memory on a queue. The queue doesn't know about how many
 *  slices of memory are already allocated, and it doesn't know about it's size.
 *  The queue is pushed to the consumer by two events:
 *   * an allocation would overflow the current queue
 *   * by caller request
 *  The producer is blocked if all NINE_QUEUES queues are full.
 *
 * Consumer:
 *  Returns a pointer to memory on a queue. The queue doesn't know about how many
 *  slices of memory are still allocated, and it doesn't know about it's size.
 *  The consumer is blocked if all NINE_QUEUES are empty.
 *
 * Constrains:
 *  The caller has to provide correct memory slice size for allocation and deallocation.
 *  Assuming that only fixed size elements are passed, the sizeof operator could be used
 *  to provide this value.
 *
 * A pool of NINE_QUEUES queues are allocated, each with a size of NINE_QUEUE_SIZE.
 */

struct nine_queue {
    unsigned head;
    unsigned tail;
    void *mem_pool;
    BOOL full;
};

struct nine_queue_pool {
    struct nine_queue pool[NINE_QUEUES];
    unsigned head;
    unsigned tail;
    pipe_condvar event_pop;
    pipe_condvar event_push;
    pipe_mutex mutex_pop;
    pipe_mutex mutex_push;
};

/* Consumer functions: */
static void
nine_queue_free(struct nine_queue* ctx)
{
    ctx->head = ctx->tail = ctx->full = 0;
}

/* Gets a pointer to the next memory slice.
 * Blocks if none in queue. */
void *
nine_queue_get(struct nine_queue_pool* ctx)
{
    struct nine_queue *queue = &ctx->pool[ctx->tail];

    /* wait for queue full */
    pipe_mutex_lock(ctx->mutex_push);
    while (!queue->full)
    {
        pipe_condvar_wait(ctx->event_push, ctx->mutex_push);
    }
    pipe_mutex_unlock(ctx->mutex_push);

    return queue->mem_pool + queue->tail;
}

/* Frees a slice of memory with size @space.
 * Doesn't block.
 * Signals producer on fully processed queue. */
void
nine_queue_pop(struct nine_queue_pool* ctx, unsigned space)
{
    struct nine_queue *queue = &ctx->pool[ctx->tail];

    queue->tail += space;

    if (queue->tail == queue->head) {
        pipe_mutex_lock(ctx->mutex_pop);
        nine_queue_free(queue);
        pipe_condvar_signal(ctx->event_pop);
        pipe_mutex_unlock(ctx->mutex_pop);

        ctx->tail = (ctx->tail + 1) & NINE_QUEUES_MASK;
    }
}

/* Producer functions: */

/* Flush queue and push it to consumer thread. */
static void
nine_queue_submit(struct nine_queue_pool* ctx)
{
    struct nine_queue *queue = &ctx->pool[ctx->head];

    /* signal waiting worker */
    pipe_mutex_lock(ctx->mutex_push);
    queue->full = 1;
    pipe_condvar_signal(ctx->event_push);
    pipe_mutex_unlock(ctx->mutex_push);

    ctx->head = (ctx->head + 1) & NINE_QUEUES_MASK;
}

/* Gets a a pointer to slice of memory with size @space.
 * Does block if queue is full.
 * Returns NULL on @space > NINE_QUEUE_SIZE. */
void *
nine_queue_alloc(struct nine_queue_pool* ctx, unsigned space)
{
    struct nine_queue *queue = &ctx->pool[ctx->head];

    if (space > NINE_QUEUE_SIZE)
        return NULL;

    /* wait for queue empty */
    pipe_mutex_lock(ctx->mutex_pop);
    while (queue->full)
    {
        pipe_condvar_wait(ctx->event_pop, ctx->mutex_pop);
    }
    pipe_mutex_unlock(ctx->mutex_pop);

    if (queue->head + space > NINE_QUEUE_SIZE) {

        nine_queue_submit(ctx);

        queue = &ctx->pool[ctx->head];

        /* wait for queue empty */
        pipe_mutex_lock(ctx->mutex_pop);
        while (queue->full)
        {
            pipe_condvar_wait(ctx->event_pop, ctx->mutex_pop);
        }
        pipe_mutex_unlock(ctx->mutex_pop);
    }

    return queue->mem_pool + queue->head;
}

/* Pushes a slice of memory with size @space.
 * Doesn't block.
 * Flushes the queue on @flush . */
void
nine_queue_push(struct nine_queue_pool* ctx, unsigned space, bool flush)
{
    struct nine_queue *queue = &ctx->pool[ctx->head];
    queue->head += space;

    if (flush)
        nine_queue_submit(ctx);
}

struct nine_queue_pool*
nine_queue_create(void)
{
    unsigned i;
    struct nine_queue_pool *ctx;

    ctx = CALLOC_STRUCT(nine_queue_pool);
    if (!ctx)
        goto failed;

    for (i = 0; i < NINE_QUEUES; i++) {
        ctx->pool[i].mem_pool = MALLOC(NINE_QUEUE_SIZE);
        if (!ctx->pool[i].mem_pool)
            goto failed;
    }

    pipe_condvar_init(ctx->event_pop);
    pipe_mutex_init(ctx->mutex_pop);

    pipe_condvar_init(ctx->event_push);
    pipe_mutex_init(ctx->mutex_push);

    return ctx;
failed:
    if (ctx) {
        for (i = 0; i < NINE_QUEUES; i++) {
            if (ctx->pool[i].mem_pool)
                FREE(ctx->pool[i].mem_pool);
        }
        FREE(ctx);
    }
    return NULL;
}

void
nine_queue_delete(struct nine_queue_pool *ctx)
{
    unsigned i;
    pipe_mutex_destroy(ctx->mutex_pop);
    pipe_mutex_destroy(ctx->mutex_push);

    for (i = 0; i < NINE_QUEUES; i++)
        FREE(ctx->pool[i].mem_pool);

    FREE(ctx);
}
