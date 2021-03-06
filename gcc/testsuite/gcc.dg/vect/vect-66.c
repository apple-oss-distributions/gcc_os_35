/* { dg-do run { target powerpc*-*-* } } */
/* { dg-do run { target i?86-*-* x86_64-*-* } } */
/* { dg-options "-O2 -ftree-vectorize -fdump-tree-vect-stats -maltivec" { target powerpc*-*-* } } */
/* { dg-options "-O2 -ftree-vectorize -fdump-tree-vect-stats -msse2" { target i?86-*-* x86_64-*-* } } */

#include <stdarg.h>
#include "tree-vect.h"

#define N 16

int main1 ()
{
  int i, j;
  int ib[6] = {0,3,6,9,12,15};
  int ia[8][5][6];
  int ic[16][16][5][6];

  /* Multidimensional array. Aligned. */
  for (i = 0; i < 16; i++)
    {
      for (j = 0; j < 4; j++)
        {
           ia[2][6][j] = 5;
        }
    }

  /* check results: */  
  for (i = 0; i < 16; i++)
    {
      for (j = 0; j < 4; j++)
        {
           if (ia[2][6][j] != 5)
                abort();
        }
    }
  /* Multidimensional array. Aligned. */
  for (i = 0; i < 16; i++)
    {
      for (j = 0; j < 4; j++)
           ia[3][6][j+2] = 5;
    }

  /* check results: */  
  for (i = 0; i < 16; i++)
    {
      for (j = 2; j < 6; j++)
        {
           if (ia[3][6][j] != 5)
                abort();
        }
    }

  /* Multidimensional array. Not aligned. */
  for (i = 0; i < 16; i++)
    {
      for (j = 0; j < 4; j++)
        {
           ic[2][1][6][j] = 5;
        }
    }

  /* check results: */  
  for (i = 0; i < 16; i++)
    {
      for (j = 0; j < 4; j++)
        {
           if (ic[2][1][6][j] != 5)
                abort();
        }
    }

  return 0;
}

int main (void)
{ 
  check_vect ();

  return main1 ();
}

/* { dg-final { scan-tree-dump-times "vectorized 2 loops" 1 "vect" } } */
