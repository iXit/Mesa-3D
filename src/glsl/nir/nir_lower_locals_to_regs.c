/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir.h"

struct locals_to_regs_state {
   void *mem_ctx;
   nir_function_impl *impl;

   /* A hash table mapping derefs to registers */
   struct hash_table *regs_table;
};

/* The following two functions implement a hash and equality check for
 * variable dreferences.  When the hash or equality function encounters an
 * array, it ignores the offset and whether it is direct or indirect
 * entirely.
 */
static uint32_t
hash_deref(const void *void_deref)
{
   const nir_deref *deref = void_deref;

   uint32_t hash;
   if (deref->child) {
      hash = hash_deref(deref->child);
   } else {
      hash = 2166136261ul;
   }

   switch (deref->deref_type) {
   case nir_deref_type_var:
      hash ^= _mesa_hash_pointer(nir_deref_as_var(deref)->var);
      break;
   case nir_deref_type_array: {
      hash ^= 268435183;
      break;
   }
   case nir_deref_type_struct:
      hash ^= nir_deref_as_struct(deref)->index;
      break;
   }

   return hash * 0x01000193;
}

static bool
derefs_equal(const void *void_a, const void *void_b)
{
   const nir_deref *a = void_a;
   const nir_deref *b = void_b;

   if (a->deref_type != b->deref_type)
      return false;

   switch (a->deref_type) {
   case nir_deref_type_var:
      if (nir_deref_as_var(a)->var != nir_deref_as_var(b)->var)
         return false;
      break;
   case nir_deref_type_array:
      /* Do nothing.  All array derefs are the same */
      break;
   case nir_deref_type_struct:
      if (nir_deref_as_struct(a)->index != nir_deref_as_struct(b)->index)
         return false;
      break;
   default:
      unreachable("Invalid dreference type");
   }

   assert((a->child == NULL) == (b->child == NULL));
   if (a->child)
      return derefs_equal(a->child, b->child);
   else
      return true;
}

static nir_register *
get_reg_for_deref(nir_deref_var *deref, struct locals_to_regs_state *state)
{
   uint32_t hash = hash_deref(deref);

   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(state->regs_table, hash, deref);
   if (entry)
      return entry->data;

   unsigned array_size = 1;
   nir_deref *tail = &deref->deref;
   while (tail->child) {
      if (tail->child->deref_type == nir_deref_type_array) {
         /* Multiply by the parent's type. */
         if (glsl_type_is_matrix(tail->type)) {
            array_size *= glsl_get_matrix_columns(tail->type);
         } else {
            assert(glsl_get_length(tail->type) > 0);
            array_size *= glsl_get_length(tail->type);
         }
      }
      tail = tail->child;
   }

   assert(glsl_type_is_vector(tail->type) || glsl_type_is_scalar(tail->type));

   nir_register *reg = nir_local_reg_create(state->impl);
   reg->num_components = glsl_get_vector_elements(tail->type);
   reg->num_array_elems = array_size > 1 ? array_size : 0;

   _mesa_hash_table_insert_with_hash(state->regs_table, hash, deref, reg);

   return reg;
}

static nir_src
get_deref_reg_src(nir_deref_var *deref, nir_instr *instr,
                  struct locals_to_regs_state *state)
{
   nir_src src;

   src.is_ssa = false;
   src.reg.reg = get_reg_for_deref(deref, state);
   src.reg.base_offset = 0;
   src.reg.indirect = NULL;

   nir_deref *tail = &deref->deref;
   while (tail->child != NULL) {
      const struct glsl_type *parent_type = tail->type;
      tail = tail->child;

      if (tail->deref_type != nir_deref_type_array)
         continue;

      nir_deref_array *deref_array = nir_deref_as_array(tail);

      src.reg.base_offset *= glsl_get_length(parent_type);
      src.reg.base_offset += deref_array->base_offset;

      if (src.reg.indirect) {
         nir_load_const_instr *load_const =
            nir_load_const_instr_create(state->mem_ctx, 1);
         load_const->value.u[0] = glsl_get_length(parent_type);
         nir_instr_insert_before(instr, &load_const->instr);

         nir_alu_instr *mul = nir_alu_instr_create(state->mem_ctx, nir_op_imul);
         mul->src[0].src = *src.reg.indirect;
         mul->src[1].src.is_ssa = true;
         mul->src[1].src.ssa = &load_const->def;
         mul->dest.write_mask = 1;
         mul->dest.dest.is_ssa = true;
         nir_ssa_def_init(&mul->instr, &mul->dest.dest.ssa, 1, NULL);
         nir_instr_insert_before(instr, &mul->instr);

         src.reg.indirect->is_ssa = true;
         src.reg.indirect->ssa = &mul->dest.dest.ssa;
      }

      if (deref_array->deref_array_type == nir_deref_array_type_indirect) {
         if (src.reg.indirect == NULL) {
            src.reg.indirect = ralloc(state->mem_ctx, nir_src);
            *src.reg.indirect = nir_src_copy(deref_array->indirect,
                                             state->mem_ctx);
         } else {
            nir_alu_instr *add = nir_alu_instr_create(state->mem_ctx,
                                                      nir_op_iadd);
            add->src[0].src = *src.reg.indirect;
            add->src[1].src = nir_src_copy(deref_array->indirect,
                                           state->mem_ctx);
            add->dest.write_mask = 1;
            add->dest.dest.is_ssa = true;
            nir_ssa_def_init(&add->instr, &add->dest.dest.ssa, 1, NULL);
            nir_instr_insert_before(instr, &add->instr);

            src.reg.indirect->is_ssa = true;
            src.reg.indirect->ssa = &add->dest.dest.ssa;
         }
      }
   }

   return src;
}

static bool
lower_locals_to_regs_block(nir_block *block, void *void_state)
{
   struct locals_to_regs_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      switch (intrin->intrinsic) {
      case nir_intrinsic_load_var: {
         if (intrin->variables[0]->var->data.mode != nir_var_local)
            continue;

         nir_alu_instr *mov = nir_alu_instr_create(state->mem_ctx, nir_op_imov);
         mov->src[0].src = get_deref_reg_src(intrin->variables[0],
                                             &intrin->instr, state);
         mov->dest.write_mask = (1 << intrin->num_components) - 1;
         if (intrin->dest.is_ssa) {
            mov->dest.dest.is_ssa = true;
            nir_ssa_def_init(&mov->instr, &mov->dest.dest.ssa,
                             intrin->num_components, NULL);

            nir_src new_src = {
               .is_ssa = true,
               .ssa = &mov->dest.dest.ssa,
            };

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa, new_src,
                                     state->mem_ctx);
         } else {
            mov->dest.dest = nir_dest_copy(intrin->dest, state->mem_ctx);
         }
         nir_instr_insert_before(&intrin->instr, &mov->instr);

         nir_instr_remove(&intrin->instr);
         break;
      }

      case nir_intrinsic_store_var: {
         if (intrin->variables[0]->var->data.mode != nir_var_local)
            continue;

         nir_src reg_src = get_deref_reg_src(intrin->variables[0],
                                             &intrin->instr, state);

         nir_alu_instr *mov = nir_alu_instr_create(state->mem_ctx, nir_op_imov);
         mov->src[0].src = nir_src_copy(intrin->src[0], state->mem_ctx);
         mov->dest.write_mask = (1 << intrin->num_components) - 1;
         mov->dest.dest.is_ssa = false;
         mov->dest.dest.reg.reg = reg_src.reg.reg;
         mov->dest.dest.reg.base_offset = reg_src.reg.base_offset;
         mov->dest.dest.reg.indirect = reg_src.reg.indirect;

         nir_instr_insert_before(&intrin->instr, &mov->instr);

         nir_instr_remove(&intrin->instr);
         break;
      }

      case nir_intrinsic_copy_var:
         unreachable("There should be no copies whatsoever at this point");
         break;

      default:
         continue;
      }
   }

   return true;
}

static void
nir_lower_locals_to_regs_impl(nir_function_impl *impl)
{
   struct locals_to_regs_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.impl = impl;
   state.regs_table = _mesa_hash_table_create(NULL, hash_deref, derefs_equal);

   nir_foreach_block(impl, lower_locals_to_regs_block, &state);

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);

   _mesa_hash_table_destroy(state.regs_table, NULL);
}

void
nir_lower_locals_to_regs(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_lower_locals_to_regs_impl(overload->impl);
   }
}
