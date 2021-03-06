/* A pass for lowering trees to RTL.
   Copyright (C) 2004 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "basic-block.h"
#include "function.h"
#include "expr.h"
#include "langhooks.h"
#include "tree-flow.h"
#include "timevar.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "except.h"
#include "flags.h"
#include "diagnostic.h"
#include "toplev.h"

/* Verify that there is exactly single jump instruction since last and attach
   REG_BR_PROB note specifying probability.
   ??? We really ought to pass the probability down to RTL expanders and let it
   re-distribute it when the conditional expands into multiple conditionals.
   This is however difficult to do.  */
static void
add_reg_br_prob_note (FILE *dump_file, rtx last, int probability)
{
  if (profile_status == PROFILE_ABSENT)
    return;
  for (last = NEXT_INSN (last); last && NEXT_INSN (last); last = NEXT_INSN (last))
    if (GET_CODE (last) == JUMP_INSN)
      {
	/* It is common to emit condjump-around-jump sequence when we don't know
	   how to reverse the conditional.  Special case this.  */
	if (!any_condjump_p (last)
	    || GET_CODE (NEXT_INSN (last)) != JUMP_INSN
	    || !simplejump_p (NEXT_INSN (last))
	    || GET_CODE (NEXT_INSN (NEXT_INSN (last))) != BARRIER
	    || GET_CODE (NEXT_INSN (NEXT_INSN (NEXT_INSN (last)))) != CODE_LABEL
	    || NEXT_INSN (NEXT_INSN (NEXT_INSN (NEXT_INSN (last)))))
	  goto failed;
	if (find_reg_note (last, REG_BR_PROB, 0))
	  abort ();
	REG_NOTES (last)
	  = gen_rtx_EXPR_LIST (REG_BR_PROB,
			       GEN_INT (REG_BR_PROB_BASE - probability),
			       REG_NOTES (last));
	return;
      }
  if (!last || GET_CODE (last) != JUMP_INSN || !any_condjump_p (last))
      goto failed;
  if (find_reg_note (last, REG_BR_PROB, 0))
    abort ();
  REG_NOTES (last)
    = gen_rtx_EXPR_LIST (REG_BR_PROB,
			 GEN_INT (probability), REG_NOTES (last));
  return;
failed:
  if (dump_file)
    fprintf (dump_file, "Failed to add probability note\n");
}


#ifndef LOCAL_ALIGNMENT
#define LOCAL_ALIGNMENT(TYPE, ALIGNMENT) ALIGNMENT
#endif

#ifndef STACK_ALIGNMENT_NEEDED
#define STACK_ALIGNMENT_NEEDED 1
#endif

#ifdef FRAME_GROWS_DOWNWARD
# undef FRAME_GROWS_DOWNWARD
# define FRAME_GROWS_DOWNWARD 1
#else
# define FRAME_GROWS_DOWNWARD 0
#endif


/* This structure holds data relevant to one variable that will be
   placed in a stack slot.  */
struct stack_var
{
  /* The Variable.  */
  tree decl;

  /* The offset of the variable.  During partitioning, this is the
     offset relative to the partition.  After partitioning, this
     is relative to the stack frame.  */
  HOST_WIDE_INT offset;

  /* Initially, the size of the variable.  Later, the size of the partition,
     if this variable becomes it's partition's representative.  */
  HOST_WIDE_INT size;

  /* The *byte* alignment required for this variable.  Or as, with the
     size, the alignment for this partition.  */
  unsigned int alignb;

  /* The partition representative.  */
  size_t representative;

  /* The next stack variable in the partition, or EOC.  */
  size_t next;
};

#define EOC  ((size_t)-1)

/* We have an array of such objects while deciding allocation.  */
static struct stack_var *stack_vars;
static size_t stack_vars_alloc;
static size_t stack_vars_num;

/* An array of indicies such that stack_vars[stack_vars_sorted[i]].size
   is non-decreasing.  */
static size_t *stack_vars_sorted;

/* We have an interference graph between such objects.  This graph
   is lower triangular.  */
static bool *stack_vars_conflict;
static size_t stack_vars_conflict_alloc;

/* The phase of the stack frame.  This is the known misalignment of
   virtual_stack_vars_rtx from PREFERRED_STACK_BOUNDARY.  That is,
   (frame_offset+frame_phase) % PREFERRED_STACK_BOUNDARY == 0.  */
static int frame_phase;


/* Discover the byte alignment to use for DECL.  Ignore alignment
   we can't do with expected alignment of the stack boundary.  */

static unsigned int
get_decl_align_unit (tree decl)
{
  unsigned int align;

  align = DECL_ALIGN (decl);
  align = LOCAL_ALIGNMENT (TREE_TYPE (decl), align);
  if (align > PREFERRED_STACK_BOUNDARY)
    align = PREFERRED_STACK_BOUNDARY;
  if (cfun->stack_alignment_needed < align)
    cfun->stack_alignment_needed = align;

  return align / BITS_PER_UNIT;
}

/* Allocate SIZE bytes at byte alignment ALIGN from the stack frame.
   Return the frame offset.  */

static HOST_WIDE_INT
alloc_stack_frame_space (HOST_WIDE_INT size, HOST_WIDE_INT align)
{
  HOST_WIDE_INT offset, new_frame_offset;

  new_frame_offset = frame_offset;
  if (FRAME_GROWS_DOWNWARD)
    {
      new_frame_offset -= size + frame_phase;
      new_frame_offset &= -align;
      new_frame_offset += frame_phase;
      offset = new_frame_offset;
    }
  else
    {
      new_frame_offset -= frame_phase;
      new_frame_offset += align - 1;
      new_frame_offset &= -align;
      new_frame_offset += frame_phase;
      offset = new_frame_offset;
      new_frame_offset += size;
    }
  frame_offset = new_frame_offset;

  return offset;
}

/* Accumulate DECL into STACK_VARS.  */

static void
add_stack_var (tree decl)
{
  if (stack_vars_num >= stack_vars_alloc)
    {
      if (stack_vars_alloc)
	stack_vars_alloc = stack_vars_alloc * 3 / 2;
      else
	stack_vars_alloc = 32;
      stack_vars
	= XRESIZEVEC (struct stack_var, stack_vars, stack_vars_alloc);
    }
  stack_vars[stack_vars_num].decl = decl;
  stack_vars[stack_vars_num].offset = 0;
  stack_vars[stack_vars_num].size = tree_low_cst (DECL_SIZE_UNIT (decl), 1);
  stack_vars[stack_vars_num].alignb = get_decl_align_unit (decl);

  /* All variables are initially in their own partition.  */
  stack_vars[stack_vars_num].representative = stack_vars_num;
  stack_vars[stack_vars_num].next = EOC;

  /* Ensure that this decl doesn't get put onto the list twice.  */
  SET_DECL_RTL (decl, pc_rtx);

  stack_vars_num++;
}

/* Compute the linear index of a lower-triangular coordinate (I, J).  */

static size_t
triangular_index (size_t i, size_t j)
{
  if (i < j)
    {
      size_t t;
      t = i, i = j, j = t;
    }
  return (i * (i + 1)) / 2 + j;
}

/* Ensure that STACK_VARS_CONFLICT is large enough for N objects.  */

static void
resize_stack_vars_conflict (size_t n)
{
  size_t size = triangular_index (n-1, n-1) + 1;

  if (size <= stack_vars_conflict_alloc)
    return;

  stack_vars_conflict = XRESIZEVEC (bool, stack_vars_conflict, size);
  memset (stack_vars_conflict + stack_vars_conflict_alloc, 0,
	  (size - stack_vars_conflict_alloc) * sizeof (bool));
  stack_vars_conflict_alloc = size;
}

/* Make the decls associated with luid's X and Y conflict.  */

static void
add_stack_var_conflict (size_t x, size_t y)
{
  size_t index = triangular_index (x, y);
  gcc_assert (index < stack_vars_conflict_alloc);
  stack_vars_conflict[index] = true;
}

/* Check whether the decls associated with luid's X and Y conflict.  */

static bool
stack_var_conflict_p (size_t x, size_t y)
{
  size_t index = triangular_index (x, y);
  gcc_assert (index < stack_vars_conflict_alloc);
  return stack_vars_conflict[index];
}
  
/* A subroutine of expand_used_vars.  If two variables X and Y have alias
   sets that do not conflict, then do add a conflict for these variables
   in the interference graph.  We also have to mind MEM_IN_STRUCT_P and
   MEM_SCALAR_P.  */

static void
add_alias_set_conflicts (void)
{
  size_t i, j, n = stack_vars_num;

  for (i = 0; i < n; ++i)
    {
      bool aggr_i = AGGREGATE_TYPE_P (TREE_TYPE (stack_vars[i].decl));
      HOST_WIDE_INT set_i = get_alias_set (stack_vars[i].decl);

      for (j = 0; j < i; ++j)
	{
	  bool aggr_j = AGGREGATE_TYPE_P (TREE_TYPE (stack_vars[j].decl));
	  HOST_WIDE_INT set_j = get_alias_set (stack_vars[j].decl);
	  if (aggr_i != aggr_j || !alias_sets_conflict_p (set_i, set_j))
	    add_stack_var_conflict (i, j);
	}
    }
}

/* A subroutine of partition_stack_vars.  A comparison function for qsort,
   sorting an array of indicies by the size of the object.  */

static int
stack_var_size_cmp (const void *a, const void *b)
{
  HOST_WIDE_INT sa = stack_vars[*(const size_t *)a].size;
  HOST_WIDE_INT sb = stack_vars[*(const size_t *)b].size;

  if (sa < sb)
    return -1;
  if (sa > sb)
    return 1;
  return 0;
}

/* A subroutine of partition_stack_vars.  The UNION portion of a UNION/FIND
   partitioning algorithm.  Partitions A and B are known to be non-conflicting.
   Merge them into a single partition A.

   At the same time, add OFFSET to all variables in partition B.  At the end
   of the partitioning process we've have a nice block easy to lay out within
   the stack frame.  */

static void
union_stack_vars (size_t a, size_t b, HOST_WIDE_INT offset)
{
  size_t i, last;

  /* Update each element of partition B with the given offset,
     and merge them into partition A.  */
  for (last = i = b; i != EOC; last = i, i = stack_vars[i].next)
    {
      stack_vars[i].offset += offset;
      stack_vars[i].representative = a;
    }
  stack_vars[last].next = stack_vars[a].next;
  stack_vars[a].next = b;

  /* Update the required alignment of partition A to account for B.  */
  if (stack_vars[a].alignb < stack_vars[b].alignb)
    stack_vars[a].alignb = stack_vars[b].alignb;

  /* Update the interference graph and merge the conflicts.  */
  for (last = stack_vars_num, i = 0; i < last; ++i)
    if (stack_var_conflict_p (b, i))
      add_stack_var_conflict (a, i);
}

/* A subroutine of expand_used_vars.  Binpack the variables into
   partitions constrained by the interference graph.  The overall
   algorithm used is as follows:

	Sort the objects by size.
	For each object A {
	  S = size(A)
	  O = 0
	  loop {
	    Look for the largest non-conflicting object B with size <= S.
	    UNION (A, B)
	    offset(B) = O
	    O += size(B)
	    S -= size(B)
	  }
	}
*/

static void
partition_stack_vars (void)
{
  size_t si, sj, n = stack_vars_num;

  stack_vars_sorted = XNEWVEC (size_t, stack_vars_num);
  for (si = 0; si < n; ++si)
    stack_vars_sorted[si] = si;

  if (n == 1)
    return;

  qsort (stack_vars_sorted, n, sizeof (size_t), stack_var_size_cmp);

  /* Special case: detect when all variables conflict, and thus we can't
     do anything during the partitioning loop.  It isn't uncommon (with
     C code at least) to declare all variables at the top of the function,
     and if we're not inlining, then all variables will be in the same scope.
     Take advantage of very fast libc routines for this scan.  */
  gcc_assert (sizeof(bool) == sizeof(char));
  if (memchr (stack_vars_conflict, false, stack_vars_conflict_alloc) == NULL)
    return;

  for (si = 0; si < n; ++si)
    {
      size_t i = stack_vars_sorted[si];
      HOST_WIDE_INT isize = stack_vars[i].size;
      HOST_WIDE_INT offset = 0;

      for (sj = si; sj-- > 0; )
	{
	  size_t j = stack_vars_sorted[sj];
	  HOST_WIDE_INT jsize = stack_vars[j].size;
	  unsigned int jalign = stack_vars[j].alignb;

	  /* Ignore objects that aren't partition representatives.  */
	  if (stack_vars[j].representative != j)
	    continue;

	  /* Ignore objects too large for the remaining space.  */
	  if (isize < jsize)
	    continue;

	  /* Ignore conflicting objects.  */
	  if (stack_var_conflict_p (i, j))
	    continue;

	  /* Refine the remaining space check to include alignment.  */
	  if (offset & (jalign - 1))
	    {
	      HOST_WIDE_INT toff = offset;
	      toff += jalign - 1;
	      toff &= -(HOST_WIDE_INT)jalign;
	      if (isize - (toff - offset) < jsize)
		continue;

	      isize -= toff - offset;
	      offset = toff;
	    }

	  /* UNION the objects, placing J at OFFSET.  */
	  union_stack_vars (i, j, offset);

	  isize -= jsize;
	  if (isize == 0)
	    break;
	}
    }
}

/* A debugging aid for expand_used_vars.  Dump the generated partitions.  */

static void
dump_stack_var_partition (void)
{
  size_t si, i, j, n = stack_vars_num;

  for (si = 0; si < n; ++si)
    {
      i = stack_vars_sorted[si];

      /* Skip variables that aren't partition representatives, for now.  */
      if (stack_vars[i].representative != i)
	continue;

      fprintf (dump_file, "Partition %lu: size " HOST_WIDE_INT_PRINT_DEC
	       " align %u\n", (unsigned long) i, stack_vars[i].size,
	       stack_vars[i].alignb);

      for (j = i; j != EOC; j = stack_vars[j].next)
	{
	  fputc ('\t', dump_file);
	  print_generic_expr (dump_file, stack_vars[j].decl, dump_flags);
	  fprintf (dump_file, ", offset " HOST_WIDE_INT_PRINT_DEC "\n",
		   stack_vars[i].offset);
	}
    }
}

/* Assign rtl to DECL at frame offset OFFSET.  */

static void
expand_one_stack_var_at (tree decl, HOST_WIDE_INT offset)
{
  HOST_WIDE_INT align;
  rtx x;
  
  /* If this fails, we've overflowed the stack frame.  Error nicely?  */
  gcc_assert (offset == trunc_int_for_mode (offset, Pmode));

  x = plus_constant (virtual_stack_vars_rtx, offset);
  x = gen_rtx_MEM (DECL_MODE (decl), x);

  /* Set alignment we actually gave this decl.  */
  offset -= frame_phase;
  align = offset & -offset;
  align *= BITS_PER_UNIT;
  if (align > STACK_BOUNDARY || align == 0)
    align = STACK_BOUNDARY;
  DECL_ALIGN (decl) = align;
  DECL_USER_ALIGN (decl) = 0;

  set_mem_attributes (x, decl, true);
  SET_DECL_RTL (decl, x);
}

/* A subroutine of expand_used_vars.  Give each partition representative
   a unique location within the stack frame.  Update each partition member
   with that location.  */

static void
expand_stack_vars (void)
{
  size_t si, i, j, n = stack_vars_num;

  for (si = 0; si < n; ++si)
    {
      HOST_WIDE_INT offset;

      i = stack_vars_sorted[si];

      /* Skip variables that aren't partition representatives, for now.  */
      if (stack_vars[i].representative != i)
	continue;

      offset = alloc_stack_frame_space (stack_vars[i].size,
					stack_vars[i].alignb);

      /* Create rtl for each variable based on their location within the
	 partition.  */
      for (j = i; j != EOC; j = stack_vars[j].next)
	expand_one_stack_var_at (stack_vars[j].decl,
				 stack_vars[j].offset + offset);
    }
}

/* A subroutine of expand_one_var.  Called to immediately assign rtl
   to a variable to be allocated in the stack frame.  */

static void
expand_one_stack_var (tree var)
{
  HOST_WIDE_INT size, offset, align;

  size = tree_low_cst (DECL_SIZE_UNIT (var), 1);
  align = get_decl_align_unit (var);
  offset = alloc_stack_frame_space (size, align);

  expand_one_stack_var_at (var, offset);
}

/* A subroutine of expand_one_var.  Called to assign rtl
   to a TREE_STATIC VAR_DECL.  */

static void
expand_one_static_var (tree var)
{
  /* If this is an inlined copy of a static local variable,
     look up the original.  */
  var = DECL_ORIGIN (var);

  /* If we've already processed this variable because of that, do nothing.  */
  if (TREE_ASM_WRITTEN (var))
    return;

  /* Give the front end a chance to do whatever.  In practice, this is
     resolving duplicate names for IMA in C.  */
  if (lang_hooks.expand_decl (var))
    return;

  /* Otherwise, just emit the variable.  */
  rest_of_decl_compilation (var, 0, 0);
}

/* A subroutine of expand_one_var.  Called to assign rtl to a VAR_DECL
   that will reside in a hard register.  */

static void
expand_one_hard_reg_var (tree var)
{
  rest_of_decl_compilation (var, 0, 0);
}

/* A subroutine of expand_one_var.  Called to assign rtl to a VAR_DECL
   that will reside in a pseudo register.  */

static void
expand_one_register_var (tree var)
{
  tree type = TREE_TYPE (var);
  int unsignedp = TYPE_UNSIGNED (type);
  enum machine_mode reg_mode
    = promote_mode (type, DECL_MODE (var), &unsignedp, 0);
  rtx x = gen_reg_rtx (reg_mode);

  SET_DECL_RTL (var, x);

  /* Note if the object is a user variable.  */
  if (!DECL_ARTIFICIAL (var))
    {
      mark_user_reg (x);

      /* Trust user variables which have a pointer type to really
	 be pointers.  Do not trust compiler generated temporaries
	 as our type system is totally busted as it relates to
	 pointer arithmetic which translates into lots of compiler
	 generated objects with pointer types, but which are not really
	 pointers.  */
      if (POINTER_TYPE_P (type))
	mark_reg_pointer (x, TYPE_ALIGN (TREE_TYPE (TREE_TYPE (var))));
    }
}

/* A subroutine of expand_one_var.  Called to assign rtl to a VAR_DECL that
   has some associated error, e.g. it's type is error-mark.  We just need
   to pick something that won't crash the rest of the compiler.  */

static void
expand_one_error_var (tree var)
{
  enum machine_mode mode = DECL_MODE (var);
  rtx x;

  if (mode == BLKmode)
    x = gen_rtx_MEM (BLKmode, const0_rtx);
  else if (mode == VOIDmode)
    x = const0_rtx;
  else
    x = gen_reg_rtx (mode);

  SET_DECL_RTL (var, x);
}

/* A subroutine of expand_one_var.  VAR is a variable that will be 
   allocated to the local stack frame.  Return true if we wish to
   add VAR to STACK_VARS so that it will be coalesced with other
   variables.  Return false to allocate VAR immediately.

   This function is used to reduce the number of variables considered
   for coalescing, which reduces the size of the quadratic problem.  */

static bool
defer_stack_allocation (tree var, bool toplevel)
{
  /* Variables in the outermost scope automatically conflict with
     every other variable.  The only reason to want to defer them
     at all is that, after sorting, we can more efficiently pack
     small variables in the stack frame.  Continue to defer at -O2.  */
  if (toplevel && optimize < 2)
    return false;

  /* Without optimization, *most* variables are allocated from the
     stack, which makes the quadratic problem large exactly when we
     want compilation to proceed as quickly as possible.  On the 
     other hand, we don't want the function's stack frame size to
     get completely out of hand.  So we avoid adding scalars and
     "small" aggregates to the list at all.  */
  if (optimize == 0 && tree_low_cst (DECL_SIZE_UNIT (var), 1) < 32)
    return false;

  return true;
}

/* A subroutine of expand_used_vars.  Expand one variable according to
   its flavor.  Variables to be placed on the stack are not actually
   expanded yet, merely recorded.  */

static void
expand_one_var (tree var, bool toplevel)
{
  if (TREE_CODE (var) != VAR_DECL)
    lang_hooks.expand_decl (var);
  else if (DECL_EXTERNAL (var))
    ;
  else if (DECL_VALUE_EXPR (var))
    ;
  else if (TREE_STATIC (var))
    expand_one_static_var (var);
  else if (DECL_RTL_SET_P (var))
    ;
  else if (TREE_TYPE (var) == error_mark_node)
    expand_one_error_var (var);
  else if (DECL_HARD_REGISTER (var))
    expand_one_hard_reg_var (var);
  else if (use_register_for_decl (var))
    expand_one_register_var (var);
  else if (defer_stack_allocation (var, toplevel))
    add_stack_var (var);
  else
    expand_one_stack_var (var);
}

/* A subroutine of expand_used_vars.  Walk down through the BLOCK tree
   expanding variables.  Those variables that can be put into registers
   are allocated pseudos; those that can't are put on the stack.

   TOPLEVEL is true if this is the outermost BLOCK.  */

static void
expand_used_vars_for_block (tree block, bool toplevel)
{
  size_t i, j, old_sv_num, this_sv_num, new_sv_num;
  tree t;

  old_sv_num = toplevel ? 0 : stack_vars_num;

  /* Expand all variables at this level.  */
  for (t = BLOCK_VARS (block); t ; t = TREE_CHAIN (t))
    if (TREE_USED (t))
      expand_one_var (t, toplevel);

  this_sv_num = stack_vars_num;

  /* Expand all variables at containing levels.  */
  for (t = BLOCK_SUBBLOCKS (block); t ; t = BLOCK_CHAIN (t))
    expand_used_vars_for_block (t, false);

  /* Since we do not track exact variable lifetimes (which is not even
     possible for varibles whose address escapes), we mirror the block
     tree in the interference graph.  Here we cause all variables at this
     level, and all sublevels, to conflict.  Do make certain that a
     variable conflicts with itself.  */
  if (old_sv_num < this_sv_num)
    {
      new_sv_num = stack_vars_num;
      resize_stack_vars_conflict (new_sv_num);

      for (i = old_sv_num; i < new_sv_num; ++i)
	for (j = i < this_sv_num ? i+1 : this_sv_num; j-- > old_sv_num ;)
	  add_stack_var_conflict (i, j);
    }
}

/* A subroutine of expand_used_vars.  Walk down through the BLOCK tree
   and clear TREE_USED on all local variables.  */

static void
clear_tree_used (tree block)
{
  tree t;

  for (t = BLOCK_VARS (block); t ; t = TREE_CHAIN (t))
    /* if (!TREE_STATIC (t) && !DECL_EXTERNAL (t)) */
      TREE_USED (t) = 0;

  for (t = BLOCK_SUBBLOCKS (block); t ; t = BLOCK_CHAIN (t))
    clear_tree_used (t);
}

/* Expand all variables used in the function.  */

static void
expand_used_vars (void)
{
  tree t, outer_block = DECL_INITIAL (current_function_decl);

  /* Compute the phase of the stack frame for this function.  */
  {
    int align = PREFERRED_STACK_BOUNDARY / BITS_PER_UNIT;
    int off = STARTING_FRAME_OFFSET % align;
    frame_phase = off ? align - off : 0;
  }

  /* Set TREE_USED on all variables in the unexpanded_var_list.  */
  for (t = cfun->unexpanded_var_list; t; t = TREE_CHAIN (t))
    TREE_USED (TREE_VALUE (t)) = 1;

  /* Clear TREE_USED on all variables associated with a block scope.  */
  clear_tree_used (outer_block);

  /* At this point all variables on the unexpanded_var_list with TREE_USED
     set are not associated with any block scope.  Lay them out.  */
  for (t = cfun->unexpanded_var_list; t; t = TREE_CHAIN (t))
    {
      tree var = TREE_VALUE (t);
      bool expand_now = false;

      /* We didn't set a block for static or extern because it's hard
	 to tell the difference between a global variable (re)declared
	 in a local scope, and one that's really declared there to
	 begin with.  And it doesn't really matter much, since we're
	 not giving them stack space.  Expand them now.  */
      if (TREE_STATIC (var) || DECL_EXTERNAL (var))
	expand_now = true;

      /* Any variable that could have been hoisted into an SSA_NAME
	 will have been propagated anywhere the optimizers chose,
	 i.e. not confined to their original block.  Allocate them
	 as if they were defined in the outermost scope.  */
      else if (is_gimple_reg (var))
	expand_now = true;

      /* If the variable is not associated with any block, then it
	 was created by the optimizers, and could be live anywhere
	 in the function.  */
      else if (TREE_USED (var))
	expand_now = true;

      /* Finally, mark all variables on the list as used.  We'll use
	 this in a moment when we expand those associated with scopes.  */
      TREE_USED (var) = 1;

      if (expand_now)
	expand_one_var (var, true);
    }
  cfun->unexpanded_var_list = NULL_TREE;

  /* At this point, all variables within the block tree with TREE_USED
     set are actually used by the optimized function.  Lay them out.  */
  expand_used_vars_for_block (outer_block, true);

  if (stack_vars_num > 0)
    {
      /* Due to the way alias sets work, no variables with non-conflicting
	 alias sets may be assigned the same address.  Add conflicts to 
	 reflect this.  */
      add_alias_set_conflicts ();

      /* Now that we have collected all stack variables, and have computed a 
	 minimal interference graph, attempt to save some stack space.  */
      partition_stack_vars ();
      if (dump_file)
	dump_stack_var_partition ();

      /* Assign rtl to each variable based on these partitions.  */
      expand_stack_vars ();

      /* Free up stack variable graph data.  */
      XDELETEVEC (stack_vars);
      XDELETEVEC (stack_vars_sorted);
      XDELETEVEC (stack_vars_conflict);
      stack_vars = NULL;
      stack_vars_alloc = stack_vars_num = 0;
      stack_vars_conflict = NULL;
      stack_vars_conflict_alloc = 0;
    }

  /* If the target requires that FRAME_OFFSET be aligned, do it.  */
  if (STACK_ALIGNMENT_NEEDED)
    {
      HOST_WIDE_INT align = PREFERRED_STACK_BOUNDARY / BITS_PER_UNIT;
      if (!FRAME_GROWS_DOWNWARD)
	frame_offset += align - 1;
      frame_offset &= -align;
    }
}


/* A subroutine of expand_gimple_basic_block.  Expand one COND_EXPR.
   Returns a new basic block if we've terminated the current basic
   block and created a new one.  */

static basic_block
expand_gimple_cond_expr (basic_block bb, tree stmt)
{
  basic_block new_bb, dest;
  edge new_edge;
  edge true_edge;
  edge false_edge;
  tree pred = COND_EXPR_COND (stmt);
  tree then_exp = COND_EXPR_THEN (stmt);
  tree else_exp = COND_EXPR_ELSE (stmt);
  rtx last = get_last_insn ();

  extract_true_false_edges_from_block (bb, &true_edge, &false_edge);
  if (EXPR_LOCUS (stmt))
    {
      emit_line_note (*(EXPR_LOCUS (stmt)));
      record_block_change (TREE_BLOCK (stmt));
    }

  /* These flags have no purpose in RTL land.  */
  true_edge->flags &= ~EDGE_TRUE_VALUE;
  false_edge->flags &= ~EDGE_FALSE_VALUE;

  /* We can either have a pure conditional jump with one fallthru edge or
     two-way jump that needs to be decomposed into two basic blocks.  */
  if (TREE_CODE (then_exp) == GOTO_EXPR && IS_EMPTY_STMT (else_exp))
    {
      jumpif (pred, label_rtx (GOTO_DESTINATION (then_exp)));
      add_reg_br_prob_note (dump_file, last, true_edge->probability);
      return NULL;
    }
  if (TREE_CODE (else_exp) == GOTO_EXPR && IS_EMPTY_STMT (then_exp))
    {
      jumpifnot (pred, label_rtx (GOTO_DESTINATION (else_exp)));
      add_reg_br_prob_note (dump_file, last, false_edge->probability);
      return NULL;
    }
  gcc_assert (TREE_CODE (then_exp) == GOTO_EXPR
	      && TREE_CODE (else_exp) == GOTO_EXPR);

  jumpif (pred, label_rtx (GOTO_DESTINATION (then_exp)));
  add_reg_br_prob_note (dump_file, last, true_edge->probability);
  last = get_last_insn ();
  expand_expr (else_exp, const0_rtx, VOIDmode, 0);

  BB_END (bb) = last;
  if (BARRIER_P (BB_END (bb)))
    BB_END (bb) = PREV_INSN (BB_END (bb));
  update_bb_for_insn (bb);

  new_bb = create_basic_block (NEXT_INSN (last), get_last_insn (), bb);
  dest = false_edge->dest;
  redirect_edge_succ (false_edge, new_bb);
  false_edge->flags |= EDGE_FALLTHRU;
  new_bb->count = false_edge->count;
  new_bb->frequency = EDGE_FREQUENCY (false_edge);
  new_edge = make_edge (new_bb, dest, 0);
  new_edge->probability = REG_BR_PROB_BASE;
  new_edge->count = new_bb->count;
  if (BARRIER_P (BB_END (new_bb)))
    BB_END (new_bb) = PREV_INSN (BB_END (new_bb));
  update_bb_for_insn (new_bb);

  if (dump_file)
    {
      dump_bb (bb, dump_file, 0);
      dump_bb (new_bb, dump_file, 0);
    }

  return new_bb;
}

/* A subroutine of expand_gimple_basic_block.  Expand one CALL_EXPR
   that has CALL_EXPR_TAILCALL set.  Returns non-null if we actually
   generated a tail call (something that might be denied by the ABI
   rules governing the call; see calls.c).

   Sets CAN_FALLTHRU if we generated a *conditional* tail call, and
   can still reach the rest of BB.  The case here is __builtin_sqrt,
   where the NaN result goes through the external function (with a
   tailcall) and the normal result happens via a sqrt instruction.  */

static basic_block
expand_gimple_tailcall (basic_block bb, tree stmt, bool *can_fallthru)
{
  rtx last = get_last_insn ();
  edge e;
  edge_iterator ei;
  int probability;
  gcov_type count;

  expand_expr_stmt (stmt);

  for (last = NEXT_INSN (last); last; last = NEXT_INSN (last))
    if (CALL_P (last) && SIBLING_CALL_P (last))
      goto found;

  *can_fallthru = true;
  return NULL;

 found:
  /* ??? Wouldn't it be better to just reset any pending stack adjust?
     Any instructions emitted here are about to be deleted.  */
  do_pending_stack_adjust ();

  /* Remove any non-eh, non-abnormal edges that don't go to exit.  */
  /* ??? I.e. the fallthrough edge.  HOWEVER!  If there were to be
     EH or abnormal edges, we shouldn't have created a tail call in
     the first place.  So it seems to me we should just be removing
     all edges here, or redirecting the existing fallthru edge to
     the exit block.  */

  probability = 0;
  count = 0;

  for (ei = ei_start (bb->succs); (e = ei_safe_edge (ei)); )
    {
      if (!(e->flags & (EDGE_ABNORMAL | EDGE_EH)))
	{
	  if (e->dest != EXIT_BLOCK_PTR)
	    {
	      e->dest->count -= e->count;
	      e->dest->frequency -= EDGE_FREQUENCY (e);
	      if (e->dest->count < 0)
	        e->dest->count = 0;
	      if (e->dest->frequency < 0)
	        e->dest->frequency = 0;
	    }
	  count += e->count;
	  probability += e->probability;
	  remove_edge (e);
	}
      else
	ei_next (&ei);
    }

  /* This is somewhat ugly: the call_expr expander often emits instructions
     after the sibcall (to perform the function return).  These confuse the
     find_sub_basic_blocks code, so we need to get rid of these.  */
  last = NEXT_INSN (last);
  gcc_assert (BARRIER_P (last));

  *can_fallthru = false;
  while (NEXT_INSN (last))
    {
      /* For instance an sqrt builtin expander expands if with
	 sibcall in the then and label for `else`.  */
      if (LABEL_P (NEXT_INSN (last)))
	{
	  *can_fallthru = true;
	  break;
	}
      delete_insn (NEXT_INSN (last));
    }

  e = make_edge (bb, EXIT_BLOCK_PTR, EDGE_ABNORMAL | EDGE_SIBCALL);
  e->probability += probability;
  e->count += count;
  BB_END (bb) = last;
  update_bb_for_insn (bb);

  if (NEXT_INSN (last))
    {
      bb = create_basic_block (NEXT_INSN (last), get_last_insn (), bb);

      last = BB_END (bb);
      if (BARRIER_P (last))
	BB_END (bb) = PREV_INSN (last);
    }

  return bb;
}

/* Expand basic block BB from GIMPLE trees to RTL.  */

static basic_block
expand_gimple_basic_block (basic_block bb, FILE * dump_file)
{
  block_stmt_iterator bsi = bsi_start (bb);
  tree stmt = NULL;
  rtx note, last;
  edge e;
  edge_iterator ei;

  if (dump_file)
    {
      tree_register_cfg_hooks ();
      dump_bb (bb, dump_file, 0);
      rtl_register_cfg_hooks ();
    }

  if (!bsi_end_p (bsi))
    stmt = bsi_stmt (bsi);

  if (stmt && TREE_CODE (stmt) == LABEL_EXPR)
    {
      last = get_last_insn ();

      expand_expr_stmt (stmt);

      /* Java emits line number notes in the top of labels.
         ??? Make this go away once line number notes are obsoleted.  */
      BB_HEAD (bb) = NEXT_INSN (last);
      if (NOTE_P (BB_HEAD (bb)))
	BB_HEAD (bb) = NEXT_INSN (BB_HEAD (bb));
      bsi_next (&bsi);
      note = emit_note_after (NOTE_INSN_BASIC_BLOCK, BB_HEAD (bb));
    }
  else
    note = BB_HEAD (bb) = emit_note (NOTE_INSN_BASIC_BLOCK);

  NOTE_BASIC_BLOCK (note) = bb;

  for (ei = ei_start (bb->succs); (e = ei_safe_edge (ei)); )
    {
      /* Clear EDGE_EXECUTABLE.  This flag is never used in the backend.  */
      e->flags &= ~EDGE_EXECUTABLE;

      /* At the moment not all abnormal edges match the RTL representation.
         It is safe to remove them here as find_sub_basic_blocks will
         rediscover them.  In the future we should get this fixed properly.  */
      if (e->flags & EDGE_ABNORMAL)
	remove_edge (e);
      else
	ei_next (&ei);
    }

  for (; !bsi_end_p (bsi); bsi_next (&bsi))
    {
      tree stmt = bsi_stmt (bsi);
      basic_block new_bb;

      if (!stmt)
	continue;

      /* Expand this statement, then evaluate the resulting RTL and
	 fixup the CFG accordingly.  */
      if (TREE_CODE (stmt) == COND_EXPR)
	{
	  new_bb = expand_gimple_cond_expr (bb, stmt);
	  if (new_bb)
	    return new_bb;
	}
      else
	{
	  tree call = get_call_expr_in (stmt);
	  if (call && CALL_EXPR_TAILCALL (call))
	    {
	      bool can_fallthru;
	      new_bb = expand_gimple_tailcall (bb, stmt, &can_fallthru);
	      if (new_bb)
		{
		  if (can_fallthru)
		    bb = new_bb;
		  else
		    return new_bb;
		}
	    }
	  else
	    expand_expr_stmt (stmt);
	}
    }

  do_pending_stack_adjust ();

  /* Find the the block tail.  The last insn is the block is the insn
     before a barrier and/or table jump insn.  */
  last = get_last_insn ();
  if (BARRIER_P (last))
    last = PREV_INSN (last);
  if (JUMP_TABLE_DATA_P (last))
    last = PREV_INSN (PREV_INSN (last));
  BB_END (bb) = last;

  if (dump_file)
    dump_bb (bb, dump_file, 0);
  update_bb_for_insn (bb);

  return bb;
}


/* Create a basic block for initialization code.  */

static basic_block
construct_init_block (void)
{
  basic_block init_block, first_block;
  edge e = NULL, e2;
  edge_iterator ei;

  FOR_EACH_EDGE (e2, ei, ENTRY_BLOCK_PTR->succs)
    {
      /* Clear EDGE_EXECUTABLE.  This flag is never used in the backend.

	 For all other blocks this edge flag is cleared while expanding
	 a basic block in expand_gimple_basic_block, but there we never
	 looked at the successors of the entry block.
	 This caused PR17513.  */
      e2->flags &= ~EDGE_EXECUTABLE;

      if (e2->dest == ENTRY_BLOCK_PTR->next_bb)
	e = e2;
    }

  init_block = create_basic_block (NEXT_INSN (get_insns ()),
				   get_last_insn (),
				   ENTRY_BLOCK_PTR);
  init_block->frequency = ENTRY_BLOCK_PTR->frequency;
  init_block->count = ENTRY_BLOCK_PTR->count;
  if (e)
    {
      first_block = e->dest;
      redirect_edge_succ (e, init_block);
      e = make_edge (init_block, first_block, EDGE_FALLTHRU);
    }
  else
    e = make_edge (init_block, EXIT_BLOCK_PTR, EDGE_FALLTHRU);
  e->probability = REG_BR_PROB_BASE;
  e->count = ENTRY_BLOCK_PTR->count;

  update_bb_for_insn (init_block);
  return init_block;
}


/* Create a block containing landing pads and similar stuff.  */

static void
construct_exit_block (void)
{
  rtx head = get_last_insn ();
  rtx end;
  basic_block exit_block;
  edge e, e2;
  unsigned ix;
  edge_iterator ei;

  /* Make sure the locus is set to the end of the function, so that
     epilogue line numbers and warnings are set properly.  */
#ifdef USE_MAPPED_LOCATION
  if (cfun->function_end_locus != UNKNOWN_LOCATION)
#else
  if (cfun->function_end_locus.file)
#endif
    input_location = cfun->function_end_locus;

  /* The following insns belong to the top scope.  */
  record_block_change (DECL_INITIAL (current_function_decl));

  /* Generate rtl for function exit.  */
  expand_function_end ();

  end = get_last_insn ();
  if (head == end)
    return;
  while (NEXT_INSN (head) && NOTE_P (NEXT_INSN (head)))
    head = NEXT_INSN (head);
  exit_block = create_basic_block (NEXT_INSN (head), end,
				   EXIT_BLOCK_PTR->prev_bb);
  exit_block->frequency = EXIT_BLOCK_PTR->frequency;
  exit_block->count = EXIT_BLOCK_PTR->count;

  ix = 0;
  while (ix < EDGE_COUNT (EXIT_BLOCK_PTR->preds))
    {
      e = EDGE_I (EXIT_BLOCK_PTR->preds, ix);
      if (!(e->flags & EDGE_ABNORMAL))
	redirect_edge_succ (e, exit_block);
      else
	ix++;
    }

  e = make_edge (exit_block, EXIT_BLOCK_PTR, EDGE_FALLTHRU);
  e->probability = REG_BR_PROB_BASE;
  e->count = EXIT_BLOCK_PTR->count;
  FOR_EACH_EDGE (e2, ei, EXIT_BLOCK_PTR->preds)
    if (e2 != e)
      {
        e->count -= e2->count;
	exit_block->count -= e2->count;
	exit_block->frequency -= EDGE_FREQUENCY (e2);
      }
  if (e->count < 0)
    e->count = 0;
  if (exit_block->count < 0)
    exit_block->count = 0;
  if (exit_block->frequency < 0)
    exit_block->frequency = 0;
  update_bb_for_insn (exit_block);
}

/* Translate the intermediate representation contained in the CFG
   from GIMPLE trees to RTL.

   We do conversion per basic block and preserve/update the tree CFG.
   This implies we have to do some magic as the CFG can simultaneously
   consist of basic blocks containing RTL and GIMPLE trees.  This can
   confuse the CFG hooks, so be careful to not manipulate CFG during
   the expansion.  */

static void
tree_expand_cfg (void)
{
  basic_block bb, init_block;
  sbitmap blocks;

  /* Some backends want to know that we are expanding to RTL.  */
  currently_expanding_to_rtl = 1;

  /* Prepare the rtl middle end to start recording block changes.  */
  reset_block_changes ();

  /* Expand the variables recorded during gimple lowering.  */
  expand_used_vars ();

  /* Set up parameters and prepare for return, for the function.  */
  expand_function_start (current_function_decl);

  /* If this function is `main', emit a call to `__main'
     to run global initializers, etc.  */
  if (DECL_NAME (current_function_decl)
      && MAIN_NAME_P (DECL_NAME (current_function_decl))
      && DECL_FILE_SCOPE_P (current_function_decl))
    expand_main_function ();

  /* Register rtl specific functions for cfg.  */
  rtl_register_cfg_hooks ();

  init_block = construct_init_block ();

  FOR_BB_BETWEEN (bb, init_block->next_bb, EXIT_BLOCK_PTR, next_bb)
    bb = expand_gimple_basic_block (bb, dump_file);

  construct_exit_block ();

  /* We're done expanding trees to RTL.  */
  currently_expanding_to_rtl = 0;

  /* Convert from NOTE_INSN_EH_REGION style notes, and do other
     sorts of eh initialization.  */
  convert_from_eh_region_ranges ();

  rebuild_jump_labels (get_insns ());
  find_exception_handler_labels ();

  blocks = sbitmap_alloc (last_basic_block);
  sbitmap_ones (blocks);
  find_many_sub_basic_blocks (blocks);
  purge_all_dead_edges (0);
  sbitmap_free (blocks);

  compact_blocks ();
#ifdef ENABLE_CHECKING
  verify_flow_info();
#endif

  /* There's no need to defer outputting this function any more; we
     know we want to output it.  */
  DECL_DEFER_OUTPUT (current_function_decl) = 0;

  /* Now that we're done expanding trees to RTL, we shouldn't have any
     more CONCATs anywhere.  */
  generating_concat_p = 0;

  finalize_block_changes ();
}

struct tree_opt_pass pass_expand =
{
  "expand",		                /* name */
  NULL,                                 /* gate */
  tree_expand_cfg,	                /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  TV_EXPAND,		                /* tv_id */
  /* ??? If TER is enabled, we actually receive GENERIC.  */
  PROP_gimple_leh | PROP_cfg,           /* properties_required */
  PROP_rtl,                             /* properties_provided */
  PROP_gimple_leh,			/* properties_destroyed */
  0,                                    /* todo_flags_start */
  0,					/* todo_flags_finish */
  'r'					/* letter */
};
