#ifndef UTIL_BOX_INLINES_H
#define UTIL_BOX_INLINES_H

#include "pipe/p_state.h"
#include "util/u_math.h"

static INLINE
void u_box_1d( unsigned x,
	       unsigned w,
	       struct pipe_box *box )
{
   box->x = x;
   box->y = 0;
   box->z = 0;
   box->width = w;
   box->height = 1;
   box->depth = 1;
}

static INLINE
void u_box_2d( unsigned x,
	       unsigned y,
	       unsigned w,
	       unsigned h,
	       struct pipe_box *box )
{
   box->x = x;
   box->y = y;
   box->z = 0;
   box->width = w;
   box->height = h;
   box->depth = 1;
}

static INLINE
void u_box_origin_2d( unsigned w,
		      unsigned h,
		      struct pipe_box *box )
{
   box->x = 0;
   box->y = 0;
   box->z = 0;
   box->width = w;
   box->height = h;
   box->depth = 1;
}

static INLINE
void u_box_2d_zslice( unsigned x,
		      unsigned y,
		      unsigned z,
		      unsigned w,
		      unsigned h,
		      struct pipe_box *box )
{
   box->x = x;
   box->y = y;
   box->z = z;
   box->width = w;
   box->height = h;
   box->depth = 1;
}

static INLINE
void u_box_3d( unsigned x,
	       unsigned y,
	       unsigned z,
	       unsigned w,
	       unsigned h,
	       unsigned d,
	       struct pipe_box *box )
{
   box->x = x;
   box->y = y;
   box->z = z;
   box->width = w;
   box->height = h;
   box->depth = d;
}

/* Returns whether @a is contained in or equal to @b. */
static INLINE
boolean u_box_contained(struct pipe_box *a, struct pipe_box *b)
{
   return
      a->x >= b->x && (a->x + a->width  <= b->x + b->width) &&
      a->y >= b->y && (a->y + a->height <= b->y + b->height) &&
      a->z >= b->z && (a->z + a->depth  <= b->z + b->depth);
}

/* Clips @box to width @w and height @h.
 * Returns -1 if the resulting box would be empty (then @box is left unchanged).
 * Otherwise, returns 1/2/0/3 if width/height/neither/both have been reduced.
 * Aliasing permitted.
 */
static INLINE
int u_box_clip_2d(struct pipe_box *dst,
                  struct pipe_box *box, int w, int h)
{
   int i, a[2], b[2], dim[2], res = 0;

   if (!box->width || !box->height)
      return -1;
   dim[0] = w;
   dim[1] = h;
   a[0] = box->x;
   a[1] = box->y;
   b[0] = box->x + box->width;
   b[1] = box->y + box->height;

   for (i = 0; i < 2; ++i) {
      if (b[i] < a[i]) {
         if (a[i] < 0 || b[i] >= dim[i])
            return -1;
         if (a[i] > dim[i]) { a[i] = dim[i]; res |= (1 << i); }
         if (b[i] < 0) { b[i] = 0; res |= (1 << i); }
      } else {
         if (b[i] < 0 || a[i] >= dim[i])
            return -1;
         if (a[i] < 0) { a[i] = 0; res |= (1 << i); }
         if (b[i] > dim[i]) { b[i] = dim[i]; res |= (1 << i); }
      }
   }

   if (res) {
      dst->x = a[0];
      dst->y = a[1];
      dst->width = b[0] - a[0];
      dst->height = b[1] - a[1];
   }
   return res;
}

static INLINE
int u_box_clip_3d(struct pipe_box *dst,
                  struct pipe_box *box, int w, int h, int d)
{
   int i, a[3], b[3], dim[3], res = 0;

   if (!box->width || !box->height)
      return -1;
   dim[0] = w;
   dim[1] = h;
   dim[2] = d;
   a[0] = box->x;
   a[1] = box->y;
   a[2] = box->z;
   b[0] = box->x + box->width;
   b[1] = box->y + box->height;
   b[2] = box->z + box->depth;

   for (i = 0; i < 2; ++i) {
      if (b[i] < a[i]) {
         if (a[i] < 0 || b[i] >= dim[i])
            return -1;
         if (a[i] > dim[i]) { a[i] = dim[i]; res |= (1 << i); }
         if (b[i] < 0) { b[i] = 0; res |= (1 << i); }
      } else {
         if (b[i] < 0 || a[i] >= dim[i])
            return -1;
         if (a[i] < 0) { a[i] = 0; res |= (1 << i); }
         if (b[i] > dim[i]) { b[i] = dim[i]; res |= (1 << i); }
      }
   }

   if (res) {
      dst->x = a[0];
      dst->y = a[1];
      dst->z = a[2];
      dst->width = b[0] - a[0];
      dst->height = b[1] - a[1];
      dst->depth = b[2] - a[2];
   }
   return res;
}

/* Return true if @a is contained in or equal to @b.
 */
static INLINE
boolean u_box_contained_2d(const struct pipe_box *a, const struct pipe_box *b)
{
   int a_x1 = a->x + a->width;
   int b_x1 = b->x + b->width;
   int a_y1 = a->y + a->height;
   int b_y1 = b->y + b->height;
   return
      a->x >= b->x && a_x1 <= b_x1 &&
      a->y >= b->y && a_y1 <= b_y1;
}

static INLINE
int64_t u_box_volume(const struct pipe_box *box)
{
   return (int64_t)box->width * box->height * box->depth;
}

/* Aliasing of @dst and @a permitted. */
static INLINE
void u_box_cover_2d(struct pipe_box *dst,
                    struct pipe_box *a, const struct pipe_box *b)
{
   int x1_a = a->x + a->width;
   int y1_a = a->y + a->height;
   int x1_b = b->x + b->width;
   int y1_b = b->y + b->height;

   dst->x = MIN2(a->x, b->x);
   dst->y = MIN2(a->y, b->y);

   dst->width = MAX2(x1_a, x1_b) - dst->x;
   dst->height = MAX2(y1_a, y1_b) - dst->y;
}

/* Aliasing of @dst and @a permitted. */
static INLINE
void u_box_cover(struct pipe_box *dst,
                 struct pipe_box *a, const struct pipe_box *b)
{
   int x1_a = a->x + a->width;
   int y1_a = a->y + a->height;
   int z1_a = a->z + a->depth;
   int x1_b = b->x + b->width;
   int y1_b = b->y + b->height;
   int z1_b = b->z + b->depth;

   dst->x = MIN2(a->x, b->x);
   dst->y = MIN2(a->y, b->y);
   dst->z = MIN2(a->z, b->z);

   dst->width = MAX2(x1_a, x1_b) - dst->x;
   dst->height = MAX2(y1_a, y1_b) - dst->y;
   dst->depth = MAX2(z1_a, z1_b) - dst->z;
}

static INLINE
boolean u_box_test_intersection_xy_only(const struct pipe_box *a,
                                        const struct pipe_box *b)
{
   int i;
   unsigned a_l[2], a_r[2], b_l[2], b_r[2];

   a_l[0] = MIN2(a->x, a->x + a->width);
   a_r[0] = MAX2(a->x, a->x + a->width);
   a_l[1] = MIN2(a->y, a->y + a->height);
   a_r[1] = MAX2(a->y, a->y + a->height);

   b_l[0] = MIN2(b->x, b->x + b->width);
   b_r[0] = MAX2(b->x, b->x + b->width);
   b_l[1] = MIN2(b->y, b->y + b->height);
   b_r[1] = MAX2(b->y, b->y + b->height);

   for (i = 0; i < 2; ++i) {
      if (a_l[i] > b_r[i] || a_r[i] < b_l[i])
         return FALSE;
   }
   return TRUE;
}

static INLINE
void u_box_minify(struct pipe_box *dst,
                  const struct pipe_box *src, unsigned l)
{
   dst->x = src->x >> l;
   dst->y = src->y >> l;
   dst->z = src->z >> l;
   dst->width = MAX2(src->width >> l, 1);
   dst->height = MAX2(src->height >> l, 1);
   dst->depth = MAX2(src->depth >> l, 1);
}

static INLINE
void u_box_minify_2d(struct pipe_box *dst,
                     const struct pipe_box *src, unsigned l)
{
   dst->x = src->x >> l;
   dst->y = src->y >> l;
   dst->width = MAX2(src->width >> l, 1);
   dst->height = MAX2(src->height >> l, 1);
}

#endif
