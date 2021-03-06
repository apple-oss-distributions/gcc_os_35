/* Perform doloop optimizations
   Copyright (C) 2004 Free Software Foundation, Inc.
   Based on code by Michael P. Hayes (m.hayes@elec.canterbury.ac.nz)

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "flags.h"
#include "expr.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "toplev.h"
#include "tm_p.h"
#include "cfgloop.h"
#include "output.h"
#include "params.h"

/* This module is used to modify loops with a determinable number of
   iterations to use special low-overhead looping instructions.

   It first validates whether the loop is well behaved and has a
   determinable number of iterations (either at compile or run-time).
   It then modifies the loop to use a low-overhead looping pattern as
   follows:

   1. A pseudo register is allocated as the loop iteration counter.

   2. The number of loop iterations is calculated and is stored
      in the loop counter.

   3. At the end of the loop, the jump insn is replaced by the
      doloop_end pattern.  The compare must remain because it might be
      used elsewhere.  If the loop-variable or condition register are
      used elsewhere, they will be eliminated by flow.

   4. An optional doloop_begin pattern is inserted at the top of the
      loop.

   TODO The optimization should only performed when either the biv used for exit
   condition is unused at all except for the exit test, or if we do not have to
   change its value, since otherwise we have to add a new induction variable,
   which usually will not pay up (unless the cost of the doloop pattern is
   somehow extremely lower than the cost of compare & jump, or unless the bct
   register cannot be used for anything else but doloop -- ??? detect these
   cases).  */

#ifdef HAVE_doloop_end

/* Return the loop termination condition for PATTERN or zero
   if it is not a decrement and branch jump insn.  */

static rtx
doloop_condition_get (rtx pattern)
{
  rtx cmp;
  rtx inc;
  rtx reg;
  rtx condition;

  /* The canonical doloop pattern we expect is:

     (parallel [(set (pc) (if_then_else (condition)
					(label_ref (label))
					(pc)))
		(set (reg) (plus (reg) (const_int -1)))
		(additional clobbers and uses)])
     
     Some machines (IA-64) make the decrement conditional on
     the condition as well, so we don't bother verifying the
     actual decrement.  In summary, the branch must be the
     first entry of the parallel (also required by jump.c),
     and the second entry of the parallel must be a set of
     the loop counter register.  */

  if (GET_CODE (pattern) != PARALLEL)
    return 0;

  cmp = XVECEXP (pattern, 0, 0);
  inc = XVECEXP (pattern, 0, 1);

  /* Check for (set (reg) (something)).  */
  if (GET_CODE (inc) != SET || ! REG_P (SET_DEST (inc)))
    return 0;

  /* Extract loop counter register.  */
  reg = SET_DEST (inc);

  /* Check for (set (pc) (if_then_else (condition)
				       (label_ref (label))
				       (pc))).  */
  if (GET_CODE (cmp) != SET
      || SET_DEST (cmp) != pc_rtx
      || GET_CODE (SET_SRC (cmp)) != IF_THEN_ELSE
      || GET_CODE (XEXP (SET_SRC (cmp), 1)) != LABEL_REF
      || XEXP (SET_SRC (cmp), 2) != pc_rtx)
    return 0;

  /* Extract loop termination condition.  */
  condition = XEXP (SET_SRC (cmp), 0);

  if ((GET_CODE (condition) != GE && GET_CODE (condition) != NE)
      || GET_CODE (XEXP (condition, 1)) != CONST_INT)
    return 0;

  if (XEXP (condition, 0) == reg)
    return condition;

  if (GET_CODE (XEXP (condition, 0)) == PLUS
      && XEXP (XEXP (condition, 0), 0) == reg)
    return condition;

  /* ??? If a machine uses a funny comparison, we could return a
     canonicalized form here.  */

  return 0;
}

/* Return nonzero if the loop specified by LOOP is suitable for
   the use of special low-overhead looping instructions.  DESC
   describes the number of iterations of the loop.  */

static bool
doloop_valid_p (struct loop *loop, struct niter_desc *desc)
{
  basic_block *body = get_loop_body (loop), bb;
  rtx insn;
  unsigned i;
  bool result = true;

  /* Check for loops that may not terminate under special conditions.  */
  if (!desc->simple_p
      || desc->assumptions
      || desc->infinite)
    {
      /* There are some cases that would require a special attention.
	 For example if the comparison is LEU and the comparison value
	 is UINT_MAX then the loop will not terminate.  Similarly, if the
	 comparison code is GEU and the comparison value is 0, the
	 loop will not terminate.

	 If the absolute increment is not 1, the loop can be infinite
	 even with LTU/GTU, e.g. for (i = 3; i > 0; i -= 2)

	 APPLE LOCAL begin lno
	 Note that with LE and GE, the loop behavior is undefined
	 (C++ standard section 5 clause 5) if an overflow occurs, say
	 between INT_MAX and INT_MAX + 1.  We thus don't have to worry
	 about these two cases.
	 APPLE LOCAL end lno

	 ??? We could compute these conditions at run-time and have a
	 additional jump around the loop to ensure an infinite loop.
	 However, it is very unlikely that this is the intended
	 behavior of the loop and checking for these rare boundary
	 conditions would pessimize all other code.

	 If the loop is executed only a few times an extra check to
	 restart the loop could use up most of the benefits of using a
	 count register loop.  Note however, that normally, this
	 restart branch would never execute, so it could be predicted
	 well by the CPU.  We should generate the pessimistic code by
	 default, and have an option, e.g. -funsafe-loops that would
	 enable count-register loops in this case.  */
      if (dump_file)
	fprintf (dump_file, "Doloop: Possible infinite iteration case.\n");
      result = false;
      goto cleanup;
    }

  for (i = 0; i < loop->num_nodes; i++)
    {
      bb = body[i];

      for (insn = BB_HEAD (bb);
	   insn != NEXT_INSN (BB_END (bb));
	   insn = NEXT_INSN (insn))
	{
	  /* A called function may clobber any special registers required for
	     low-overhead looping.  */
	  if (CALL_P (insn))
	    {
	      if (dump_file)
		fprintf (dump_file, "Doloop: Function call in loop.\n");
	      result = false;
	      goto cleanup;
	    }

	  /* Some targets (eg, PPC) use the count register for branch on table
	     instructions.  ??? This should be a target specific check.  */
	  if (JUMP_P (insn)
	      && (GET_CODE (PATTERN (insn)) == ADDR_DIFF_VEC
		  || GET_CODE (PATTERN (insn)) == ADDR_VEC))
	    {
	      if (dump_file)
		fprintf (dump_file, "Doloop: Computed branch in the loop.\n");
	      result = false;
	      goto cleanup;
	    }
	}
    }
  result = true;

cleanup:
  free (body);

  return result;
}

/* Adds test of COND jumping to DEST to the end of BB.  */

static void
add_test (rtx cond, basic_block bb, basic_block dest)
{
  rtx seq, jump, label;
  enum machine_mode mode;
  rtx op0 = XEXP (cond, 0), op1 = XEXP (cond, 1);
  enum rtx_code code = GET_CODE (cond);

  mode = GET_MODE (XEXP (cond, 0));
  if (mode == VOIDmode)
    mode = GET_MODE (XEXP (cond, 1));

  start_sequence ();
  op0 = force_operand (op0, NULL_RTX);
  op1 = force_operand (op1, NULL_RTX);
  label = block_label (dest);
  do_compare_rtx_and_jump (op0, op1, code, 0, mode, NULL_RTX, NULL_RTX, label);

  jump = get_last_insn ();
  JUMP_LABEL (jump) = label;

  /* The jump is supposed to handle an unlikely special case.  */
  REG_NOTES (jump)
	  = gen_rtx_EXPR_LIST (REG_BR_PROB,
			       const0_rtx, REG_NOTES (jump));

  LABEL_NUSES (label)++;

  seq = get_insns ();
  end_sequence ();
  emit_insn_after (seq, BB_END (bb));
}

/* Modify the loop to use the low-overhead looping insn where LOOP
   describes the loop, DESC describes the number of iterations of the
   loop, and DOLOOP_INSN is the low-overhead looping insn to emit at the
   end of the loop.  CONDITION is the condition separated from the
   DOLOOP_SEQ.  */

static void
doloop_modify (struct loop *loop, struct niter_desc *desc,
	       rtx doloop_seq, rtx condition)
{
  rtx counter_reg;
  rtx count, tmp, noloop = NULL_RTX;
  rtx sequence;
  rtx jump_insn;
  rtx jump_label;
  int nonneg = 0, irr;
  bool increment_count;
  basic_block loop_end = desc->out_edge->src;

  jump_insn = BB_END (loop_end);

  if (dump_file)
    {
      fprintf (dump_file, "Doloop: Inserting doloop pattern (");
      if (desc->const_iter)
	fprintf (dump_file, HOST_WIDEST_INT_PRINT_DEC, desc->niter);
      else
	fputs ("runtime", dump_file);
      fputs (" iterations).\n", dump_file);
    }

  /* Discard original jump to continue loop.  The original compare
     result may still be live, so it cannot be discarded explicitly.  */
  delete_insn (jump_insn);

  counter_reg = XEXP (condition, 0);
  if (GET_CODE (counter_reg) == PLUS)
    counter_reg = XEXP (counter_reg, 0);

  count = copy_rtx (desc->niter_expr);
  increment_count = false;
  switch (GET_CODE (condition))
    {
    case NE:
      /* Currently only NE tests against zero and one are supported.  */
      if (XEXP (condition, 1) == const1_rtx)
	{
	  increment_count = true;
	  noloop = const1_rtx;
	}
      else if (XEXP (condition, 1) == const0_rtx)
       	noloop = const0_rtx;
      else
	abort ();
      break;

    case GE:
      /* Currently only GE tests against zero are supported.  */
      if (XEXP (condition, 1) != const0_rtx)
	abort ();

      noloop = constm1_rtx;

      /* The iteration count does not need incrementing for a GE test.  */
      increment_count = false;

      /* Determine if the iteration counter will be non-negative.
	 Note that the maximum value loaded is iterations_max - 1.  */
      if (desc->niter_max
	  <= ((unsigned HOST_WIDEST_INT) 1
	      << (GET_MODE_BITSIZE (GET_MODE (counter_reg)) - 1)))
	nonneg = 1;
      break;

      /* Abort if an invalid doloop pattern has been generated.  */
    default:
      abort ();
    }

  if (increment_count)
    count = simplify_gen_binary (PLUS, desc->mode, count, const1_rtx);

  /* Insert initialization of the count register into the loop header.  */
  start_sequence ();
  tmp = force_operand (count, counter_reg);
  convert_move (counter_reg, tmp, 1);
  sequence = get_insns ();
  end_sequence ();
  emit_insn_after (sequence, BB_END (loop_preheader_edge (loop)->src));

  if (desc->noloop_assumptions)
    {
      rtx ass = copy_rtx (desc->noloop_assumptions);
      basic_block preheader = loop_preheader_edge (loop)->src;
      basic_block set_zero
	      = loop_split_edge_with (loop_preheader_edge (loop), NULL_RTX);
      basic_block new_preheader
	      = loop_split_edge_with (loop_preheader_edge (loop), NULL_RTX);
      basic_block bb;
      edge te;
      gcov_type cnt;

      /* Expand the condition testing the assumptions and if it does not pass,
	 reset the count register to 0.  */
      add_test (XEXP (ass, 0), preheader, set_zero);
      EDGE_SUCC (preheader, 0)->flags &= ~EDGE_FALLTHRU;
      cnt = EDGE_SUCC (preheader, 0)->count;
      EDGE_SUCC (preheader, 0)->probability = 0;
      EDGE_SUCC (preheader, 0)->count = 0;
      irr = EDGE_SUCC (preheader, 0)->flags & EDGE_IRREDUCIBLE_LOOP;
      te = make_edge (preheader, new_preheader, EDGE_FALLTHRU | irr);
      te->probability = REG_BR_PROB_BASE;
      te->count = cnt;
      set_immediate_dominator (CDI_DOMINATORS, new_preheader, preheader);

      set_zero->count = 0;
      set_zero->frequency = 0;

      for (ass = XEXP (ass, 1); ass; ass = XEXP (ass, 1))
	{
	  bb = loop_split_edge_with (te, NULL_RTX);
	  te = EDGE_SUCC (bb, 0);
	  add_test (XEXP (ass, 0), bb, set_zero);
	  make_edge (bb, set_zero, irr);
	}
  
      start_sequence ();
      convert_move (counter_reg, noloop, 0);
      sequence = get_insns ();
      end_sequence ();
      emit_insn_after (sequence, BB_END (set_zero));
    }

  /* Some targets (eg, C4x) need to initialize special looping
     registers.  */
#ifdef HAVE_doloop_begin
  {
    rtx init;
    unsigned level = get_loop_level (loop) + 1;
    init = gen_doloop_begin (counter_reg,
			     desc->const_iter ? desc->niter_expr : const0_rtx,
			     desc->niter_max,
			     GEN_INT (level));
    if (init)
      {
	start_sequence ();
	emit_insn (init);
	sequence = get_insns ();
	end_sequence ();
	emit_insn_after (sequence, BB_END (loop_preheader_edge (loop)->src));
      }
  }
#endif

  /* Insert the new low-overhead looping insn.  */
  emit_jump_insn_after (doloop_seq, BB_END (loop_end));
  jump_insn = BB_END (loop_end);
  jump_label = block_label (desc->in_edge->dest);
  JUMP_LABEL (jump_insn) = jump_label;
  LABEL_NUSES (jump_label)++;

  /* Ensure the right fallthru edge is marked, for case we have reversed
     the condition.  */
  desc->in_edge->flags &= ~EDGE_FALLTHRU;
  desc->out_edge->flags |= EDGE_FALLTHRU;

  /* Add a REG_NONNEG note if the actual or estimated maximum number
     of iterations is non-negative.  */
  if (nonneg)
    {
      REG_NOTES (jump_insn)
	= gen_rtx_EXPR_LIST (REG_NONNEG, NULL_RTX, REG_NOTES (jump_insn));
    }
}

/* Process loop described by LOOP validating that the loop is suitable for
   conversion to use a low overhead looping instruction, replacing the jump
   insn where suitable.  Returns true if the loop was successfully
   modified.  */

static bool
doloop_optimize (struct loop *loop)
{
  enum machine_mode mode;
  rtx doloop_seq, doloop_pat, doloop_reg;
  rtx iterations;
  rtx iterations_max;
  rtx start_label;
  rtx condition;
  unsigned level, est_niter;
  struct niter_desc *desc;

  if (dump_file)
    fprintf (dump_file, "Doloop: Processing loop %d.\n", loop->num);

  /* APPLE LOCAL begin lno */
  /* Ignore large loops.  */
  if (loop->ninsns > (unsigned) PARAM_VALUE (PARAM_MAX_DOLOOP_INSNS))
    {
      if (dump_file)
	fprintf (dump_file,
		 "Doloop: The loop is too large.\n");
      return false;
    }
  /* APPLE LOCAL end lno */

  iv_analysis_loop_init (loop);

  /* Find the simple exit of a LOOP.  */
  desc = get_simple_loop_desc (loop);

  /* Check that loop is a candidate for a low-overhead looping insn.  */
  if (!doloop_valid_p (loop, desc))
    {
      if (dump_file)
	fprintf (dump_file,
		 "Doloop: The loop is not suitable.\n");
      return false;
    }
  mode = desc->mode;

  est_niter = 3;
  if (desc->const_iter)
    est_niter = desc->niter;
  /* If the estimate on number of iterations is reliable (comes from profile
     feedback), use it.  Do not use it normally, since the expected number
     of iterations of an unrolled loop is 2.  */
  if (loop->header->count)
    est_niter = expected_loop_iterations (loop);

  if (est_niter < 3)
    {
      if (dump_file)
	fprintf (dump_file,
		 "Doloop: Too few iterations (%u) to be profitable.\n",
		 est_niter);
      return false;
    }

  iterations = desc->const_iter ? desc->niter_expr : const0_rtx;
  iterations_max = GEN_INT (desc->niter_max);
  level = get_loop_level (loop) + 1;

  /* Generate looping insn.  If the pattern FAILs then give up trying
     to modify the loop since there is some aspect the back-end does
     not like.  */
  start_label = block_label (desc->in_edge->dest);
  doloop_reg = gen_reg_rtx (mode);
  doloop_seq = gen_doloop_end (doloop_reg, iterations, iterations_max,
			       GEN_INT (level), start_label);
  if (! doloop_seq && mode != word_mode)
    {
      PUT_MODE (doloop_reg, word_mode);
      doloop_seq = gen_doloop_end (doloop_reg, iterations, iterations_max,
				   GEN_INT (level), start_label);
    }
  if (! doloop_seq)
    {
      if (dump_file)
	fprintf (dump_file,
		 "Doloop: Target unwilling to use doloop pattern!\n");
      return false;
    }

  /* If multiple instructions were created, the last must be the
     jump instruction.  Also, a raw define_insn may yield a plain
     pattern.  */
  doloop_pat = doloop_seq;
  if (INSN_P (doloop_pat))
    {
      while (NEXT_INSN (doloop_pat) != NULL_RTX)
	doloop_pat = NEXT_INSN (doloop_pat);
      if (JUMP_P (doloop_pat))
	doloop_pat = PATTERN (doloop_pat);
      else
	doloop_pat = NULL_RTX;
    }

  if (! doloop_pat
      || ! (condition = doloop_condition_get (doloop_pat)))
    {
      if (dump_file)
	fprintf (dump_file, "Doloop: Unrecognizable doloop pattern!\n");
      return false;
    }

  doloop_modify (loop, desc, doloop_seq, condition);
  return true;
}

/* This is the main entry point.  Process all LOOPS using doloop_optimize.  */

void
doloop_optimize_loops (struct loops *loops)
{
  unsigned i;
  struct loop *loop;

  for (i = 1; i < loops->num; i++)
    {
      loop = loops->parray[i];
      if (!loop)
	continue;

      doloop_optimize (loop);
    }

  iv_analysis_done ();

#ifdef ENABLE_CHECKING
  verify_dominators (CDI_DOMINATORS);
  verify_loop_structure (loops);
#endif
}
#endif /* HAVE_doloop_end */

