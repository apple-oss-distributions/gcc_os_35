/* Control flow graph analysis code for GNU compiler.
   Copyright (C) 1987, 1988, 1992, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000, 2001, 2003, 2004 Free Software Foundation, Inc.

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

/* This file contains various simple utilities to analyze the CFG.  */
#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "insn-config.h"
#include "recog.h"
#include "toplev.h"
#include "tm_p.h"
#include "timevar.h"

/* Store the data structures necessary for depth-first search.  */
struct depth_first_search_dsS {
  /* stack for backtracking during the algorithm */
  basic_block *stack;

  /* number of edges in the stack.  That is, positions 0, ..., sp-1
     have edges.  */
  unsigned int sp;

  /* record of basic blocks already seen by depth-first search */
  sbitmap visited_blocks;
};
typedef struct depth_first_search_dsS *depth_first_search_ds;

static void flow_dfs_compute_reverse_init (depth_first_search_ds);
static void flow_dfs_compute_reverse_add_bb (depth_first_search_ds,
					     basic_block);
static basic_block flow_dfs_compute_reverse_execute (depth_first_search_ds);
static void flow_dfs_compute_reverse_finish (depth_first_search_ds);
static bool flow_active_insn_p (rtx);

/* Like active_insn_p, except keep the return value clobber around
   even after reload.  */

static bool
flow_active_insn_p (rtx insn)
{
  if (active_insn_p (insn))
    return true;

  /* A clobber of the function return value exists for buggy
     programs that fail to return a value.  Its effect is to
     keep the return value from being live across the entire
     function.  If we allow it to be skipped, we introduce the
     possibility for register livetime aborts.  */
  if (GET_CODE (PATTERN (insn)) == CLOBBER
      && REG_P (XEXP (PATTERN (insn), 0))
      && REG_FUNCTION_VALUE_P (XEXP (PATTERN (insn), 0)))
    return true;

  return false;
}

/* Return true if the block has no effect and only forwards control flow to
   its single destination.  */

bool
forwarder_block_p (basic_block bb)
{
  rtx insn;

  if (bb == EXIT_BLOCK_PTR || bb == ENTRY_BLOCK_PTR
      || EDGE_COUNT (bb->succs) != 1)
    return false;

  for (insn = BB_HEAD (bb); insn != BB_END (bb); insn = NEXT_INSN (insn))
    if (INSN_P (insn) && flow_active_insn_p (insn))
      return false;

  return (!INSN_P (insn)
	  || (JUMP_P (insn) && simplejump_p (insn))
	  || !flow_active_insn_p (insn));
}

/* Return nonzero if we can reach target from src by falling through.  */

bool
can_fallthru (basic_block src, basic_block target)
{
  rtx insn = BB_END (src);
  rtx insn2;
  edge e;
  edge_iterator ei;

  if (target == EXIT_BLOCK_PTR)
    return true;
  if (src->next_bb != target)
    return 0;
  FOR_EACH_EDGE (e, ei, src->succs)
    if (e->dest == EXIT_BLOCK_PTR
	&& e->flags & EDGE_FALLTHRU)
      return 0;

  insn2 = BB_HEAD (target);
  if (insn2 && !active_insn_p (insn2))
    insn2 = next_active_insn (insn2);

  /* ??? Later we may add code to move jump tables offline.  */
  return next_active_insn (insn) == insn2;
}

/* Return nonzero if we could reach target from src by falling through,
   if the target was made adjacent.  If we already have a fall-through
   edge to the exit block, we can't do that.  */
bool
could_fall_through (basic_block src, basic_block target)
{
  edge e;
  edge_iterator ei;

  if (target == EXIT_BLOCK_PTR)
    return true;
  FOR_EACH_EDGE (e, ei, src->succs)
    if (e->dest == EXIT_BLOCK_PTR
	&& e->flags & EDGE_FALLTHRU)
      return 0;
  return true;
}

/* Mark the back edges in DFS traversal.
   Return nonzero if a loop (natural or otherwise) is present.
   Inspired by Depth_First_Search_PP described in:

     Advanced Compiler Design and Implementation
     Steven Muchnick
     Morgan Kaufmann, 1997

   and heavily borrowed from flow_depth_first_order_compute.  */

bool
mark_dfs_back_edges (void)
{
  edge_iterator *stack;
  int *pre;
  int *post;
  int sp;
  int prenum = 1;
  int postnum = 1;
  sbitmap visited;
  bool found = false;

  /* Allocate the preorder and postorder number arrays.  */
  pre = xcalloc (last_basic_block, sizeof (int));
  post = xcalloc (last_basic_block, sizeof (int));

  /* Allocate stack for back-tracking up CFG.  */
  stack = xmalloc ((n_basic_blocks + 1) * sizeof (edge_iterator));
  sp = 0;

  /* Allocate bitmap to track nodes that have been visited.  */
  visited = sbitmap_alloc (last_basic_block);

  /* None of the nodes in the CFG have been visited yet.  */
  sbitmap_zero (visited);

  /* Push the first edge on to the stack.  */
  stack[sp++] = ei_start (ENTRY_BLOCK_PTR->succs);

  while (sp)
    {
      edge_iterator ei;
      basic_block src;
      basic_block dest;

      /* Look at the edge on the top of the stack.  */
      ei = stack[sp - 1];
      src = ei_edge (ei)->src;
      dest = ei_edge (ei)->dest;
      ei_edge (ei)->flags &= ~EDGE_DFS_BACK;

      /* Check if the edge destination has been visited yet.  */
      if (dest != EXIT_BLOCK_PTR && ! TEST_BIT (visited, dest->index))
	{
	  /* Mark that we have visited the destination.  */
	  SET_BIT (visited, dest->index);

	  pre[dest->index] = prenum++;
	  if (EDGE_COUNT (dest->succs) > 0)
	    {
	      /* Since the DEST node has been visited for the first
		 time, check its successors.  */
	      stack[sp++] = ei_start (dest->succs);
	    }
	  else
	    post[dest->index] = postnum++;
	}
      else
	{
	  if (dest != EXIT_BLOCK_PTR && src != ENTRY_BLOCK_PTR
	      && pre[src->index] >= pre[dest->index]
	      && post[dest->index] == 0)
	    ei_edge (ei)->flags |= EDGE_DFS_BACK, found = true;

	  if (ei_one_before_end_p (ei) && src != ENTRY_BLOCK_PTR)
	    post[src->index] = postnum++;

	  if (!ei_one_before_end_p (ei))
	    ei_next (&stack[sp - 1]);
	  else
	    sp--;
	}
    }

  free (pre);
  free (post);
  free (stack);
  sbitmap_free (visited);

  return found;
}

/* Set the flag EDGE_CAN_FALLTHRU for edges that can be fallthru.  */

void
set_edge_can_fallthru_flag (void)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    {
      edge e;
      edge_iterator ei;

      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  e->flags &= ~EDGE_CAN_FALLTHRU;

	  /* The FALLTHRU edge is also CAN_FALLTHRU edge.  */
	  if (e->flags & EDGE_FALLTHRU)
	    e->flags |= EDGE_CAN_FALLTHRU;
	}

      /* If the BB ends with an invertible condjump all (2) edges are
	 CAN_FALLTHRU edges.  */
      if (EDGE_COUNT (bb->succs) != 2)
	continue;
      if (!any_condjump_p (BB_END (bb)))
	continue;
      if (!invert_jump (BB_END (bb), JUMP_LABEL (BB_END (bb)), 0))
	continue;
      invert_jump (BB_END (bb), JUMP_LABEL (BB_END (bb)), 0);
      EDGE_SUCC (bb, 0)->flags |= EDGE_CAN_FALLTHRU;
      EDGE_SUCC (bb, 1)->flags |= EDGE_CAN_FALLTHRU;
    }
}

/* Find unreachable blocks.  An unreachable block will have 0 in
   the reachable bit in block->flags.  A nonzero value indicates the
   block is reachable.  */

void
find_unreachable_blocks (void)
{
  edge e;
  edge_iterator ei;
  basic_block *tos, *worklist, bb;

  tos = worklist = xmalloc (sizeof (basic_block) * n_basic_blocks);

  /* Clear all the reachability flags.  */

  FOR_EACH_BB (bb)
    bb->flags &= ~BB_REACHABLE;

  /* Add our starting points to the worklist.  Almost always there will
     be only one.  It isn't inconceivable that we might one day directly
     support Fortran alternate entry points.  */

  FOR_EACH_EDGE (e, ei, ENTRY_BLOCK_PTR->succs)
    {
      *tos++ = e->dest;

      /* Mark the block reachable.  */
      e->dest->flags |= BB_REACHABLE;
    }

  /* Iterate: find everything reachable from what we've already seen.  */

  while (tos != worklist)
    {
      basic_block b = *--tos;

      FOR_EACH_EDGE (e, ei, b->succs)
	if (!(e->dest->flags & BB_REACHABLE))
	  {
	    *tos++ = e->dest;
	    e->dest->flags |= BB_REACHABLE;
	  }
    }

  free (worklist);
}

/* Functions to access an edge list with a vector representation.
   Enough data is kept such that given an index number, the
   pred and succ that edge represents can be determined, or
   given a pred and a succ, its index number can be returned.
   This allows algorithms which consume a lot of memory to
   represent the normally full matrix of edge (pred,succ) with a
   single indexed vector,  edge (EDGE_INDEX (pred, succ)), with no
   wasted space in the client code due to sparse flow graphs.  */

/* This functions initializes the edge list. Basically the entire
   flowgraph is processed, and all edges are assigned a number,
   and the data structure is filled in.  */

struct edge_list *
create_edge_list (void)
{
  struct edge_list *elist;
  edge e;
  int num_edges;
  int block_count;
  basic_block bb;
  edge_iterator ei;

  block_count = n_basic_blocks + 2;   /* Include the entry and exit blocks.  */

  num_edges = 0;

  /* Determine the number of edges in the flow graph by counting successor
     edges on each basic block.  */
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
    {
      num_edges += EDGE_COUNT (bb->succs);
    }

  elist = xmalloc (sizeof (struct edge_list));
  elist->num_blocks = block_count;
  elist->num_edges = num_edges;
  elist->index_to_edge = xmalloc (sizeof (edge) * num_edges);

  num_edges = 0;

  /* Follow successors of blocks, and register these edges.  */
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
    FOR_EACH_EDGE (e, ei, bb->succs)
      elist->index_to_edge[num_edges++] = e;

  return elist;
}

/* This function free's memory associated with an edge list.  */

void
free_edge_list (struct edge_list *elist)
{
  if (elist)
    {
      free (elist->index_to_edge);
      free (elist);
    }
}

/* This function provides debug output showing an edge list.  */

void
print_edge_list (FILE *f, struct edge_list *elist)
{
  int x;

  fprintf (f, "Compressed edge list, %d BBs + entry & exit, and %d edges\n",
	   elist->num_blocks - 2, elist->num_edges);

  for (x = 0; x < elist->num_edges; x++)
    {
      fprintf (f, " %-4d - edge(", x);
      if (INDEX_EDGE_PRED_BB (elist, x) == ENTRY_BLOCK_PTR)
	fprintf (f, "entry,");
      else
	fprintf (f, "%d,", INDEX_EDGE_PRED_BB (elist, x)->index);

      if (INDEX_EDGE_SUCC_BB (elist, x) == EXIT_BLOCK_PTR)
	fprintf (f, "exit)\n");
      else
	fprintf (f, "%d)\n", INDEX_EDGE_SUCC_BB (elist, x)->index);
    }
}

/* This function provides an internal consistency check of an edge list,
   verifying that all edges are present, and that there are no
   extra edges.  */

void
verify_edge_list (FILE *f, struct edge_list *elist)
{
  int pred, succ, index;
  edge e;
  basic_block bb, p, s;
  edge_iterator ei;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
    {
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  pred = e->src->index;
	  succ = e->dest->index;
	  index = EDGE_INDEX (elist, e->src, e->dest);
	  if (index == EDGE_INDEX_NO_EDGE)
	    {
	      fprintf (f, "*p* No index for edge from %d to %d\n", pred, succ);
	      continue;
	    }

	  if (INDEX_EDGE_PRED_BB (elist, index)->index != pred)
	    fprintf (f, "*p* Pred for index %d should be %d not %d\n",
		     index, pred, INDEX_EDGE_PRED_BB (elist, index)->index);
	  if (INDEX_EDGE_SUCC_BB (elist, index)->index != succ)
	    fprintf (f, "*p* Succ for index %d should be %d not %d\n",
		     index, succ, INDEX_EDGE_SUCC_BB (elist, index)->index);
	}
    }

  /* We've verified that all the edges are in the list, now lets make sure
     there are no spurious edges in the list.  */

  FOR_BB_BETWEEN (p, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
    FOR_BB_BETWEEN (s, ENTRY_BLOCK_PTR->next_bb, NULL, next_bb)
      {
	int found_edge = 0;

	FOR_EACH_EDGE (e, ei, p->succs)
	  if (e->dest == s)
	    {
	      found_edge = 1;
	      break;
	    }

	FOR_EACH_EDGE (e, ei, s->preds)
	  if (e->src == p)
	    {
	      found_edge = 1;
	      break;
	    }

	if (EDGE_INDEX (elist, p, s)
	    == EDGE_INDEX_NO_EDGE && found_edge != 0)
	  fprintf (f, "*** Edge (%d, %d) appears to not have an index\n",
		   p->index, s->index);
	if (EDGE_INDEX (elist, p, s)
	    != EDGE_INDEX_NO_EDGE && found_edge == 0)
	  fprintf (f, "*** Edge (%d, %d) has index %d, but there is no edge\n",
		   p->index, s->index, EDGE_INDEX (elist, p, s));
      }
}

/* Given PRED and SUCC blocks, return the edge which connects the blocks.
   If no such edge exists, return NULL.  */

edge
find_edge (basic_block pred, basic_block succ)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, pred->succs)
    if (e->dest == succ)
      return e;

  return NULL;
}

/* This routine will determine what, if any, edge there is between
   a specified predecessor and successor.  */

int
find_edge_index (struct edge_list *edge_list, basic_block pred, basic_block succ)
{
  int x;

  for (x = 0; x < NUM_EDGES (edge_list); x++)
    if (INDEX_EDGE_PRED_BB (edge_list, x) == pred
	&& INDEX_EDGE_SUCC_BB (edge_list, x) == succ)
      return x;

  return (EDGE_INDEX_NO_EDGE);
}

/* Dump the list of basic blocks in the bitmap NODES.  */

void
flow_nodes_print (const char *str, const sbitmap nodes, FILE *file)
{
  int node;

  if (! nodes)
    return;

  fprintf (file, "%s { ", str);
  EXECUTE_IF_SET_IN_SBITMAP (nodes, 0, node, {fprintf (file, "%d ", node);});
  fputs ("}\n", file);
}

/* Dump the list of edges in the array EDGE_LIST.  */

void
flow_edge_list_print (const char *str, const edge *edge_list, int num_edges, FILE *file)
{
  int i;

  if (! edge_list)
    return;

  fprintf (file, "%s { ", str);
  for (i = 0; i < num_edges; i++)
    fprintf (file, "%d->%d ", edge_list[i]->src->index,
	     edge_list[i]->dest->index);

  fputs ("}\n", file);
}


/* This routine will remove any fake predecessor edges for a basic block.
   When the edge is removed, it is also removed from whatever successor
   list it is in.  */

static void
remove_fake_predecessors (basic_block bb)
{
  edge e;
  edge_iterator ei;

  for (ei = ei_start (bb->preds); (e = ei_safe_edge (ei)); )
    {
      if ((e->flags & EDGE_FAKE) == EDGE_FAKE)
	remove_edge (e);
      else
	ei_next (&ei);
    }
}

/* This routine will remove all fake edges from the flow graph.  If
   we remove all fake successors, it will automatically remove all
   fake predecessors.  */

void
remove_fake_edges (void)
{
  basic_block bb;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR->next_bb, NULL, next_bb)
    remove_fake_predecessors (bb);
}

/* This routine will remove all fake edges to the EXIT_BLOCK.  */

void
remove_fake_exit_edges (void)
{
  remove_fake_predecessors (EXIT_BLOCK_PTR);
}


/* This function will add a fake edge between any block which has no
   successors, and the exit block. Some data flow equations require these
   edges to exist.  */

void
add_noreturn_fake_exit_edges (void)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    if (EDGE_COUNT (bb->succs) == 0)
      make_single_succ_edge (bb, EXIT_BLOCK_PTR, EDGE_FAKE);
}

/* This function adds a fake edge between any infinite loops to the
   exit block.  Some optimizations require a path from each node to
   the exit node.

   See also Morgan, Figure 3.10, pp. 82-83.

   The current implementation is ugly, not attempting to minimize the
   number of inserted fake edges.  To reduce the number of fake edges
   to insert, add fake edges from _innermost_ loops containing only
   nodes not reachable from the exit block.  */

void
connect_infinite_loops_to_exit (void)
{
  basic_block unvisited_block;
  struct depth_first_search_dsS dfs_ds;

  /* Perform depth-first search in the reverse graph to find nodes
     reachable from the exit block.  */
  flow_dfs_compute_reverse_init (&dfs_ds);
  flow_dfs_compute_reverse_add_bb (&dfs_ds, EXIT_BLOCK_PTR);

  /* Repeatedly add fake edges, updating the unreachable nodes.  */
  while (1)
    {
      unvisited_block = flow_dfs_compute_reverse_execute (&dfs_ds);
      if (!unvisited_block)
	break;

      make_edge (unvisited_block, EXIT_BLOCK_PTR, EDGE_FAKE);
      flow_dfs_compute_reverse_add_bb (&dfs_ds, unvisited_block);
    }

  flow_dfs_compute_reverse_finish (&dfs_ds);
  return;
}

/* Compute reverse top sort order.  */

void
flow_reverse_top_sort_order_compute (int *rts_order)
{
  edge_iterator *stack;
  int sp;
  int postnum = 0;
  sbitmap visited;

  /* Allocate stack for back-tracking up CFG.  */
  stack = xmalloc ((n_basic_blocks + 1) * sizeof (edge_iterator));
  sp = 0;

  /* Allocate bitmap to track nodes that have been visited.  */
  visited = sbitmap_alloc (last_basic_block);

  /* None of the nodes in the CFG have been visited yet.  */
  sbitmap_zero (visited);

  /* Push the first edge on to the stack.  */
  stack[sp++] = ei_start (ENTRY_BLOCK_PTR->succs);

  while (sp)
    {
      edge_iterator ei;
      basic_block src;
      basic_block dest;

      /* Look at the edge on the top of the stack.  */
      ei = stack[sp - 1];
      src = ei_edge (ei)->src;
      dest = ei_edge (ei)->dest;

      /* Check if the edge destination has been visited yet.  */
      if (dest != EXIT_BLOCK_PTR && ! TEST_BIT (visited, dest->index))
	{
	  /* Mark that we have visited the destination.  */
	  SET_BIT (visited, dest->index);

	  if (EDGE_COUNT (dest->succs) > 0)
	    /* Since the DEST node has been visited for the first
	       time, check its successors.  */
	    stack[sp++] = ei_start (dest->succs);
	  else
	    rts_order[postnum++] = dest->index;
	}
      else
	{
	  if (ei_one_before_end_p (ei) && src != ENTRY_BLOCK_PTR)
	   rts_order[postnum++] = src->index;

	  if (!ei_one_before_end_p (ei))
	    ei_next (&stack[sp - 1]);
	  else
	    sp--;
	}
    }

  free (stack);
  sbitmap_free (visited);
}

/* Compute the depth first search order and store in the array
  DFS_ORDER if nonzero, marking the nodes visited in VISITED.  If
  RC_ORDER is nonzero, return the reverse completion number for each
  node.  Returns the number of nodes visited.  A depth first search
  tries to get as far away from the starting point as quickly as
  possible.  */

int
flow_depth_first_order_compute (int *dfs_order, int *rc_order)
{
  edge_iterator *stack;
  int sp;
  int dfsnum = 0;
  int rcnum = n_basic_blocks - 1;
  sbitmap visited;

  /* Allocate stack for back-tracking up CFG.  */
  stack = xmalloc ((n_basic_blocks + 1) * sizeof (edge_iterator));
  sp = 0;

  /* Allocate bitmap to track nodes that have been visited.  */
  visited = sbitmap_alloc (last_basic_block);

  /* None of the nodes in the CFG have been visited yet.  */
  sbitmap_zero (visited);

  /* Push the first edge on to the stack.  */
  stack[sp++] = ei_start (ENTRY_BLOCK_PTR->succs);

  while (sp)
    {
      edge_iterator ei;
      basic_block src;
      basic_block dest;

      /* Look at the edge on the top of the stack.  */
      ei = stack[sp - 1];
      src = ei_edge (ei)->src;
      dest = ei_edge (ei)->dest;

      /* Check if the edge destination has been visited yet.  */
      if (dest != EXIT_BLOCK_PTR && ! TEST_BIT (visited, dest->index))
	{
	  /* Mark that we have visited the destination.  */
	  SET_BIT (visited, dest->index);

	  if (dfs_order)
	    dfs_order[dfsnum] = dest->index;

	  dfsnum++;

	  if (EDGE_COUNT (dest->succs) > 0)
	    /* Since the DEST node has been visited for the first
	       time, check its successors.  */
	    stack[sp++] = ei_start (dest->succs);
	  else if (rc_order)
	    /* There are no successors for the DEST node so assign
	       its reverse completion number.  */
	    rc_order[rcnum--] = dest->index;
	}
      else
	{
	  if (ei_one_before_end_p (ei) && src != ENTRY_BLOCK_PTR
	      && rc_order)
	    /* There are no more successors for the SRC node
	       so assign its reverse completion number.  */
	    rc_order[rcnum--] = src->index;

	  if (!ei_one_before_end_p (ei))
	    ei_next (&stack[sp - 1]);
	  else
	    sp--;
	}
    }

  free (stack);
  sbitmap_free (visited);

  /* The number of nodes visited should be the number of blocks.  */
  gcc_assert (dfsnum == n_basic_blocks);

  return dfsnum;
}

struct dfst_node
{
    unsigned nnodes;
    struct dfst_node **node;
    struct dfst_node *up;
};

/* Compute a preorder transversal ordering such that a sub-tree which
   is the source of a cross edge appears before the sub-tree which is
   the destination of the cross edge.  This allows for easy detection
   of all the entry blocks for a loop.

   The ordering is compute by:

     1) Generating a depth first spanning tree.

     2) Walking the resulting tree from right to left.  */

void
flow_preorder_transversal_compute (int *pot_order)
{
  edge_iterator *stack, ei;
  int i;
  int max_successors;
  int sp;
  sbitmap visited;
  struct dfst_node *node;
  struct dfst_node *dfst;
  basic_block bb;

  /* Allocate stack for back-tracking up CFG.  */
  stack = xmalloc ((n_basic_blocks + 1) * sizeof (edge));
  sp = 0;

  /* Allocate the tree.  */
  dfst = xcalloc (last_basic_block, sizeof (struct dfst_node));

  FOR_EACH_BB (bb)
    {
      max_successors = EDGE_COUNT (bb->succs);
      dfst[bb->index].node
	= (max_successors
	   ? xcalloc (max_successors, sizeof (struct dfst_node *)) : NULL);
    }

  /* Allocate bitmap to track nodes that have been visited.  */
  visited = sbitmap_alloc (last_basic_block);

  /* None of the nodes in the CFG have been visited yet.  */
  sbitmap_zero (visited);

  /* Push the first edge on to the stack.  */
  stack[sp++] = ei_start (ENTRY_BLOCK_PTR->succs);

  while (sp)
    {
      basic_block src;
      basic_block dest;

      /* Look at the edge on the top of the stack.  */
      ei = stack[sp - 1];
      src = ei_edge (ei)->src;
      dest = ei_edge (ei)->dest;

      /* Check if the edge destination has been visited yet.  */
      if (dest != EXIT_BLOCK_PTR && ! TEST_BIT (visited, dest->index))
	{
	  /* Mark that we have visited the destination.  */
	  SET_BIT (visited, dest->index);

	  /* Add the destination to the preorder tree.  */
	  if (src != ENTRY_BLOCK_PTR)
	    {
	      dfst[src->index].node[dfst[src->index].nnodes++]
		= &dfst[dest->index];
	      dfst[dest->index].up = &dfst[src->index];
	    }

	  if (EDGE_COUNT (dest->succs) > 0)
	    /* Since the DEST node has been visited for the first
	       time, check its successors.  */
	    stack[sp++] = ei_start (dest->succs);
	}

      else if (! ei_one_before_end_p (ei))
	ei_next (&stack[sp - 1]);
      else
	sp--;
    }

  free (stack);
  sbitmap_free (visited);

  /* Record the preorder transversal order by
     walking the tree from right to left.  */

  i = 0;
  node = &dfst[ENTRY_BLOCK_PTR->next_bb->index];
  pot_order[i++] = 0;

  while (node)
    {
      if (node->nnodes)
	{
	  node = node->node[--node->nnodes];
	  pot_order[i++] = node - dfst;
	}
      else
	node = node->up;
    }

  /* Free the tree.  */

  for (i = 0; i < last_basic_block; i++)
    if (dfst[i].node)
      free (dfst[i].node);

  free (dfst);
}

/* Compute the depth first search order on the _reverse_ graph and
   store in the array DFS_ORDER, marking the nodes visited in VISITED.
   Returns the number of nodes visited.

   The computation is split into three pieces:

   flow_dfs_compute_reverse_init () creates the necessary data
   structures.

   flow_dfs_compute_reverse_add_bb () adds a basic block to the data
   structures.  The block will start the search.

   flow_dfs_compute_reverse_execute () continues (or starts) the
   search using the block on the top of the stack, stopping when the
   stack is empty.

   flow_dfs_compute_reverse_finish () destroys the necessary data
   structures.

   Thus, the user will probably call ..._init(), call ..._add_bb() to
   add a beginning basic block to the stack, call ..._execute(),
   possibly add another bb to the stack and again call ..._execute(),
   ..., and finally call _finish().  */

/* Initialize the data structures used for depth-first search on the
   reverse graph.  If INITIALIZE_STACK is nonzero, the exit block is
   added to the basic block stack.  DATA is the current depth-first
   search context.  If INITIALIZE_STACK is nonzero, there is an
   element on the stack.  */

static void
flow_dfs_compute_reverse_init (depth_first_search_ds data)
{
  /* Allocate stack for back-tracking up CFG.  */
  data->stack = xmalloc ((n_basic_blocks - (INVALID_BLOCK + 1))
			 * sizeof (basic_block));
  data->sp = 0;

  /* Allocate bitmap to track nodes that have been visited.  */
  data->visited_blocks = sbitmap_alloc (last_basic_block - (INVALID_BLOCK + 1));

  /* None of the nodes in the CFG have been visited yet.  */
  sbitmap_zero (data->visited_blocks);

  return;
}

/* Add the specified basic block to the top of the dfs data
   structures.  When the search continues, it will start at the
   block.  */

static void
flow_dfs_compute_reverse_add_bb (depth_first_search_ds data, basic_block bb)
{
  data->stack[data->sp++] = bb;
  SET_BIT (data->visited_blocks, bb->index - (INVALID_BLOCK + 1));
}

/* Continue the depth-first search through the reverse graph starting with the
   block at the stack's top and ending when the stack is empty.  Visited nodes
   are marked.  Returns an unvisited basic block, or NULL if there is none
   available.  */

static basic_block
flow_dfs_compute_reverse_execute (depth_first_search_ds data)
{
  basic_block bb;
  edge e;
  edge_iterator ei;

  while (data->sp > 0)
    {
      bb = data->stack[--data->sp];

      /* Perform depth-first search on adjacent vertices.  */
      FOR_EACH_EDGE (e, ei, bb->preds)
	if (!TEST_BIT (data->visited_blocks,
		       e->src->index - (INVALID_BLOCK + 1)))
	  flow_dfs_compute_reverse_add_bb (data, e->src);
    }

  /* Determine if there are unvisited basic blocks.  */
  FOR_BB_BETWEEN (bb, EXIT_BLOCK_PTR, NULL, prev_bb)
    if (!TEST_BIT (data->visited_blocks, bb->index - (INVALID_BLOCK + 1)))
      return bb;

  return NULL;
}

/* Destroy the data structures needed for depth-first search on the
   reverse graph.  */

static void
flow_dfs_compute_reverse_finish (depth_first_search_ds data)
{
  free (data->stack);
  sbitmap_free (data->visited_blocks);
}

/* Performs dfs search from BB over vertices satisfying PREDICATE;
   if REVERSE, go against direction of edges.  Returns number of blocks
   found and their list in RSLT.  RSLT can contain at most RSLT_MAX items.  */
int
dfs_enumerate_from (basic_block bb, int reverse,
		    bool (*predicate) (basic_block, void *),
		    basic_block *rslt, int rslt_max, void *data)
{
  basic_block *st, lbb;
  int sp = 0, tv = 0;

  st = xcalloc (rslt_max, sizeof (basic_block));
  rslt[tv++] = st[sp++] = bb;
  bb->flags |= BB_VISITED;
  while (sp)
    {
      edge e;
      edge_iterator ei;
      lbb = st[--sp];
      if (reverse)
        {
	  FOR_EACH_EDGE (e, ei, lbb->preds)
	    if (!(e->src->flags & BB_VISITED) && predicate (e->src, data))
	      {
	        gcc_assert (tv != rslt_max);
	        rslt[tv++] = st[sp++] = e->src;
	        e->src->flags |= BB_VISITED;
	      }
        }
      else
        {
	  FOR_EACH_EDGE (e, ei, lbb->succs)
	    if (!(e->dest->flags & BB_VISITED) && predicate (e->dest, data))
	      {
	        gcc_assert (tv != rslt_max);
	        rslt[tv++] = st[sp++] = e->dest;
	        e->dest->flags |= BB_VISITED;
	      }
	}
    }
  free (st);
  for (sp = 0; sp < tv; sp++)
    rslt[sp]->flags &= ~BB_VISITED;
  return tv;
}


/* Computing the Dominance Frontier:

   As described in Morgan, section 3.5, this may be done simply by
   walking the dominator tree bottom-up, computing the frontier for
   the children before the parent.  When considering a block B,
   there are two cases:

   (1) A flow graph edge leaving B that does not lead to a child
   of B in the dominator tree must be a block that is either equal
   to B or not dominated by B.  Such blocks belong in the frontier
   of B.

   (2) Consider a block X in the frontier of one of the children C
   of B.  If X is not equal to B and is not dominated by B, it
   is in the frontier of B.  */

static void
compute_dominance_frontiers_1 (bitmap *frontiers, basic_block bb, sbitmap done)
{
  edge e;
  edge_iterator ei;
  basic_block c;

  SET_BIT (done, bb->index);

  /* Do the frontier of the children first.  Not all children in the
     dominator tree (blocks dominated by this one) are children in the
     CFG, so check all blocks.  */
  for (c = first_dom_son (CDI_DOMINATORS, bb);
       c;
       c = next_dom_son (CDI_DOMINATORS, c))
    {
      if (! TEST_BIT (done, c->index))
    	compute_dominance_frontiers_1 (frontiers, c, done);
    }
      
  /* Find blocks conforming to rule (1) above.  */
  FOR_EACH_EDGE (e, ei, bb->succs)
    {
      if (e->dest == EXIT_BLOCK_PTR)
	continue;
      if (get_immediate_dominator (CDI_DOMINATORS, e->dest) != bb)
	bitmap_set_bit (frontiers[bb->index], e->dest->index);
    }

  /* Find blocks conforming to rule (2).  */
  for (c = first_dom_son (CDI_DOMINATORS, bb);
       c;
       c = next_dom_son (CDI_DOMINATORS, c))
    {
      int x;
      bitmap_iterator bi;

      EXECUTE_IF_SET_IN_BITMAP (frontiers[c->index], 0, x, bi)
	{
	  if (get_immediate_dominator (CDI_DOMINATORS, BASIC_BLOCK (x)) != bb)
	    bitmap_set_bit (frontiers[bb->index], x);
	}
    }
}


void
compute_dominance_frontiers (bitmap *frontiers)
{
  sbitmap done = sbitmap_alloc (last_basic_block);

  timevar_push (TV_DOM_FRONTIERS);

  sbitmap_zero (done);

  compute_dominance_frontiers_1 (frontiers, EDGE_SUCC (ENTRY_BLOCK_PTR, 0)->dest, done);

  sbitmap_free (done);

  timevar_pop (TV_DOM_FRONTIERS);
}

