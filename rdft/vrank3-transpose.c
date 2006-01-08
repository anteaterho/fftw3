/*
 * Copyright (c) 2003, 2006 Matteo Frigo
 * Copyright (c) 2003, 2006 Massachusetts Institute of Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: vrank3-transpose.c,v 1.39 2006-01-08 08:13:53 stevenj Exp $ */

/* rank-0, vector-rank-3, square and non-square in-place transposition  */

#include "rdft.h"

#ifdef HAVE_STRING_H
#include <string.h>		/* for memcpy() */
#endif

struct P_s;

typedef struct {
     rdftapply apply;
     int (*applicable)(const problem_rdft *p, planner *plnr,
		       int dim0, int dim1, int dim2, INT *nbuf);
     int (*mkcldrn)(const problem_rdft *p, planner *plnr, struct P_s *ego);
     const char *nam;
} transpose_adt;

typedef struct {
     solver super;
     const transpose_adt *adt;
} S;

typedef struct P_s {
     plan_rdft super;
     INT n, vl;
     INT s0, s1, vs;
     INT m;
     INT nbuf; /* buffer size */
     INT nd, md, d; /* transpose_gcd params */
     INT fd;
     plan *cld1, *cld2, *cld3; /* children, null if unused */
     const S *slv;
} P;


/*************************************************************************/
/* some utilities for the solvers */

static INT gcd(INT a, INT b)
{
     INT r;
     do {
	  r = a % b;
	  a = b;
	  b = r;
     } while (r != 0);
     
     return a;
}

/* whether we can transpose with one of our routines expecting
   contiguous Ntuples */
static int Ntuple_transposable(const iodim *a, const iodim *b, INT vl, INT vs)
{
     return (vs == 1 && b->is == vl && a->os == vl &&
	     ((a->n == b->n && a->is == b->os
	       && a->is >= b->n && a->is % vl == 0)
	      || (a->is == b->n * vl && b->os == a->n * vl)));
}

/* check whether a and b correspond to the first and second dimensions
   of a transpose of tuples with vector length = vl, stride = vs. */
static int transposable(const iodim *a, const iodim *b, INT vl, INT vs)
{
     return ((a->n == b->n && a->os == b->is && a->is == b->os)
             || Ntuple_transposable(a, b, vl, vs));
}

static int pickdim(const tensor *s, int *pdim0, int *pdim1, int *pdim2)
{
     int dim0, dim1;

     for (dim0 = 0; dim0 < s->rnk; ++dim0)
          for (dim1 = 0; dim1 < s->rnk; ++dim1) {
	       int dim2 = 3 - dim0 - dim1;
	       if (dim0 == dim1) continue;
               if ((s->rnk == 2 || s->dims[dim2].is == s->dims[dim2].os)
		   && transposable(s->dims + dim0, s->dims + dim1, 
				   s->rnk == 2 ? (INT)1 : s->dims[dim2].n,
				   s->rnk == 2 ? (INT)1 : s->dims[dim2].is)) {
                    *pdim0 = dim0;
                    *pdim1 = dim1;
		    *pdim2 = dim2;
                    return 1;
               }
	  }
     return 0;
}

/* generic applicability function */
static int applicable(const solver *ego_, const problem *p_, planner *plnr,
		      int *dim0, int *dim1, int *dim2, INT *nbuf)
{
     const S *ego = (const S *) ego_;
     const problem_rdft *p = (const problem_rdft *) p_;

     return (1
	     && p->I == p->O /* FIXME: out-of-place transposes too? */
	     && p->sz->rnk == 0
	     && (p->vecsz->rnk == 2 || p->vecsz->rnk == 3)

	     && pickdim(p->vecsz, dim0, dim1, dim2)

	     /* UGLY if vecloop in wrong order for locality */
	     && (!NO_UGLYP(plnr) ||
		 p->vecsz->rnk == 2 ||
		 X(iabs)(p->vecsz->dims[*dim2].is)
		 < X(imax)(X(iabs)(p->vecsz->dims[*dim0].is),
			   X(iabs)(p->vecsz->dims[*dim0].os)))

	     /* SLOW if non-square */
	     && (!NO_SLOWP(plnr)
		 || p->vecsz->dims[*dim0].n == p->vecsz->dims[*dim1].n)
		      
	     && ego->adt->applicable(p, plnr, *dim0,*dim1,*dim2,nbuf)

	     /* buffers too big are UGLY */
	     && ((!NO_UGLYP(plnr) && !CONSERVE_MEMORYP(plnr))
		 || *nbuf <= 65536
		 || *nbuf * 8 < X(tensor_sz)(p->vecsz))
	  );
}

static void get_transpose_vec(const problem_rdft *p, int dim2, INT *vl,INT *vs)
{
     if (p->vecsz->rnk == 2) {
	  *vl = 1; *vs = 1;
     }
     else {
	  *vl = p->vecsz->dims[dim2].n;
	  *vs = p->vecsz->dims[dim2].is; /* == os */
     }  
}

/*************************************************************************/

/* Convert an n x n square matrix A between "packed" (n x n) and
   "unpacked" (n x fda) formats.  The "padding" elements at the ends
   of the rows are not preserved. */

static void pack(R *A, INT n, INT fda, INT N)
{
     INT i;
     for (i = 0; i < n; ++i)
	  memmove(A + (n*N) * i, A + (fda*N) * i, sizeof(R) * (n*N));
}

static void unpack(R *A, INT n, INT fda, INT N)
{
     INT i;
     for (i = n-1; i >= 0; --i)
	  memmove(A + (fda*N) * i, A + (n*N) * i, sizeof(R) * (n*N));
}

/*************************************************************************/
/* Cache-oblivious in-place transpose of non-square matrices, based 
   on transposes of blocks given by the gcd of the dimensions.

   This algorithm is related to algorithm V5 from Murray Dow,
   "Transposing a matrix on a vector computer," Parallel Computing 21
   (12), 1997-2005 (1995), with the modification that we use
   cache-oblivious recursive transpose subroutines (and we derived
   it independently).
   
   For a p x q matrix, this requires scratch space equal to the size
   of the matrix divided by gcd(p,q).  Alternatively, see also the
   "cut" algorithm below, if |p-q| * gcd(p,q) < max(p,q). */

static void apply_gcd(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     INT n = ego->nd, m = ego->md, d = ego->d;
     INT vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     INT i, num_el = n*m*d*vl;

     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == ego->m * vl
       && ego->n == n * d && ego->m == m * d);
     UNUSED(O);

     /* Transpose the matrix I in-place, where I is an (n*d) x (m*d) matrix
	of vl-tuples and buf contains n*m*d*vl elements.  
	
	In general, to transpose a p x q matrix, you should call this
	routine with d = gcd(p, q), n = p/d, and m = q/d.  */

     A(n > 0 && m > 0 && vl > 0);
     A(d > 1);

     /* treat as (d x n) x (d' x m) matrix.  (d' = d) */
     
     /* First, transpose d x (n x d') x m to d x (d' x n) x m,
	using the buf matrix.  This consists of d transposes
	of contiguous n x d' matrices of m-tuples. */
     if (n > 1) {
	  rdftapply cldapply = ((plan_rdft *) ego->cld1)->apply;
	  for (i = 0; i < d; ++i) {
	       cldapply(ego->cld1, I + i*num_el, buf);
	       memcpy(I + i*num_el, buf, num_el*sizeof(R));
	  }
     }
     
     /* Now, transpose (d x d') x (n x m) to (d' x d) x (n x m), which
	is a square in-place transpose of n*m-tuples: */
     {
	  rdftapply cldapply = ((plan_rdft *) ego->cld2)->apply;
	  cldapply(ego->cld2, I, I);
     }
     
     /* Finally, transpose d' x ((d x n) x m) to d' x (m x (d x n)),
	using the buf matrix.  This consists of d' transposes
	of contiguous d*n x m matrices. */
     if (m > 1) {
	  rdftapply cldapply = ((plan_rdft *) ego->cld3)->apply;
	  for (i = 0; i < d; ++i) {
	       cldapply(ego->cld3, I + i*num_el, buf);
	       memcpy(I + i*num_el, buf, num_el*sizeof(R));
	  }
     }

     X(ifree)(buf);
}

static int applicable_gcd(const problem_rdft *p, planner *plnr,
			  int dim0, int dim1, int dim2, INT *nbuf)
{
     INT n = p->vecsz->dims[dim0].n;
     INT m = p->vecsz->dims[dim1].n;
     INT d, vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     d = gcd(n, m);
     *nbuf = n * (m / d) * vl;
     return (!NO_SLOWP(plnr) /* FIXME: not really SLOW for large 1d ffts */
	     && n != m
	     && d > 1
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static int mkcldrn_gcd(const problem_rdft *p, planner *plnr, P *ego)
{
     INT n = ego->nd, m = ego->md, d = ego->d;
     INT vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     INT num_el = n*m*d*vl;

     if (n > 1) {
	  ego->cld1 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(n, d*m*vl, m*vl,
						      d, m*vl, n*m*vl,
						      m*vl, 1, 1),
				       TAINT(p->I, num_el), buf,
				       R2HC));
	  if (!ego->cld1)
	       return 0;
	  X(ops_madd)(d, &ego->cld1->ops, &ego->super.super.ops,
		      &ego->super.super.ops);
	  ego->super.super.ops.other += num_el * d * 2;
     }

     ego->cld2 = X(mkplan_d)(plnr,
			     X(mkproblem_rdft_d)(
				  X(mktensor_0d)(),
				  X(mktensor_3d)(d, d*n*m*vl, n*m*vl,
						 d, n*m*vl, d*n*m*vl,
						 n*m*vl, 1, 1),
				  p->I, p->I,
				  R2HC));
     if (!ego->cld2)
	  return 0;
     X(ops_add)(&ego->super.super.ops, &ego->cld2->ops, &ego->super.super.ops);

     if (m > 1) {
	  ego->cld3 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(d*n, m*vl, vl,
						      m, vl, d*n*vl,
						      vl, 1, 1),
				       TAINT(p->I, num_el), buf,
				       R2HC));
	  if (!ego->cld3)
	       return 0;
	  X(ops_madd)(d, &ego->cld3->ops, &ego->super.super.ops,
		      &ego->super.super.ops);
	  ego->super.super.ops.other += num_el * d * 2;
     }

     return 1;
}

static const transpose_adt adt_gcd =
{
     apply_gcd, applicable_gcd, mkcldrn_gcd,
     "rdft-transpose-gcd"
};

/*************************************************************************/
/* Cache-oblivious in-place transpose of non-square matrices, based on
   transposing a square sub-matrix first and then transposing the
   remainder with the help of a buffer.

   This algorithm is related to algorithm V3 from Murray Dow,
   "Transposing a matrix on a vector computer," Parallel Computing 21
   (12), 1997-2005 (1995), with the modification that we use
   cache-oblivious recursive transpose subroutines.

   For transposing an n x m matrix, requires scratch space equal to
   the size of the matrix times |n-m| / max(n,m).  See also
   transpose-gcd, above, if |n-m| * gcd(n,m) > max(n,m). */

static void apply_cut(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     INT n = ego->n, m = ego->m;
     INT vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == m * vl);
     UNUSED(O);

     /* Transpose the matrix I in-place, where I is an n x m matrix
	of vl-tuples and buf contains min(n,m) * |n-m| * N elements. */
     A(n > 0 && m > 0 && vl > 0 && n != m);
     if (n > m) {
	  memcpy(buf, I + m*(m*vl), (n-m)*(m*vl)*sizeof(R));
	  ((plan_rdft *) ego->cld1)->apply(ego->cld1, I, I);
	  unpack(I, m, n, vl);
	  ((plan_rdft *) ego->cld2)->apply(ego->cld2, buf, I + m*vl);
     }
     else /* if (m > n) */ {
	  ((plan_rdft *) ego->cld1)->apply(ego->cld1, I + n*vl, buf);
	  pack(I, n, m, vl);
	  ((plan_rdft *) ego->cld2)->apply(ego->cld2, I, I);
	  memcpy(I + n*(n*vl), buf, (m-n)*(n*vl)*sizeof(R));
     }

     X(ifree)(buf);
}

static int applicable_cut(const problem_rdft *p, planner *plnr,
			  int dim0, int dim1, int dim2, INT *nbuf)
{
     INT n = p->vecsz->dims[dim0].n;
     INT m = p->vecsz->dims[dim1].n;
     INT vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = X(imin)(n, m) * X(iabs)(n - m) * vl;
     return (!NO_SLOWP(plnr) /* FIXME: not really SLOW for large 1d ffts? */
	     && n != m
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static int mkcldrn_cut(const problem_rdft *p, planner *plnr, P *ego)
{
     INT n = ego->n, m = ego->m;
     INT vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);

     if (n > m) {
	  ego->cld1 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(m, m*vl, vl,
						      m, vl, m*vl,
						      vl, 1, 1),
				       p->I, p->I,
				       R2HC));
	  if (!ego->cld1)
	       return 0;

	  ego->cld2 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(n-m, m*vl, vl,
						      m, vl, n*vl,
						      vl, 1, 1),
				       buf, p->I + m*vl,
				       R2HC));
	  if (!ego->cld2)
	       return 0;
     }
     else /* if (m > n) */ {
	  ego->cld1 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(n, m*vl, vl,
						      m-n, vl, n*vl,
						      vl, 1, 1),
				       p->I + n*vl, buf,
				       R2HC));
	  if (!ego->cld1)
	       return 0;

	  ego->cld2 = X(mkplan_d)(plnr,
				  X(mkproblem_rdft_d)(
				       X(mktensor_0d)(),
				       X(mktensor_3d)(n, n*vl, vl,
						      n, vl, n*vl,
						      vl, 1, 1),
				       p->I, p->I,
				       R2HC));
	  if (!ego->cld2)
	       return 0;
     }

     X(ops_add)(&ego->super.super.ops, &ego->cld1->ops, &ego->super.super.ops);
     X(ops_add)(&ego->super.super.ops, &ego->cld2->ops, &ego->super.super.ops);
     ego->super.super.ops.other += 
	  vl * X(imin)(n,m) * (X(iabs)(n-m) + X(imin)(n,m)) * 2;

     /* heuristic penalty so that estimator prefers transpose-gcd when
	gcd is large and |n-m| are large, and vice-versa when |n-m| small): */
     ego->super.super.ops.other += vl*(X(iabs)(n-m)*ego->d - X(imax)(n,m))*4;

     return 1;
}

static const transpose_adt adt_cut =
{
     apply_cut, applicable_cut, mkcldrn_cut,
     "rdft-transpose-cut"
};

/*************************************************************************/
/* In-place transpose routine from TOMS.  This routine is much slower
   than the cache-oblivious algorithm above, but is has the advantage
   of requiring less buffer space for the case of gcd(nx,ny) small. */

/*
 * TOMS Transpose.  Algorithm 513 (Revised version of algorithm 380).
 * 
 * These routines do in-place transposes of arrays.
 * 
 * [ Cate, E.G. and Twigg, D.W., ACM Transactions on Mathematical Software, 
 *   vol. 3, no. 1, 104-110 (1977) ]
 * 
 * C version by Steven G. Johnson (February 1997).
 */

/*
 * "a" is a 1D array of length ny*nx*N which constains the nx x ny
 * matrix of N-tuples to be transposed.  "a" is stored in row-major
 * order (last index varies fastest).  move is a 1D array of length
 * move_size used to store information to speed up the process.  The
 * value move_size=(ny+nx)/2 is recommended.  buf should be an array
 * of length 2*N.
 * 
 */

static void transpose_toms513(R *a, INT nx, INT ny, INT N,
                              char *move, INT move_size, R *buf)
{
     INT i, im, mn;
     R *b, *c, *d;
     INT ncount;
     INT k;
     
     /* check arguments and initialize: */
     A(ny > 0 && nx > 0 && N > 0 && move_size > 0);
     
     b = buf;
     
     /* Cate & Twigg have a special case for nx == ny, but we don't
	bother, since we already have special code for this case elsewhere. */

     c = buf + N;
     ncount = 2;		/* always at least 2 fixed points */
     k = (mn = ny * nx) - 1;
     
     for (i = 0; i < move_size; ++i)
	  move[i] = 0;
     
     if (ny >= 3 && nx >= 3)
	  ncount += gcd(ny - 1, nx - 1) - 1;	/* # fixed points */
     
     i = 1;
     im = ny;
     
     while (1) {
	  INT i1, i2, i1c, i2c;
	  INT kmi;
	  
	  /** Rearrange the elements of a loop
	      and its companion loop: **/
	  
	  i1 = i;
	  kmi = k - i;
	  i1c = kmi;
	  switch (N) {
	      case 1:
		   b[0] = a[i1];
		   c[0] = a[i1c];
		   break;
	      case 2:
		   b[0] = a[2*i1];
		   b[1] = a[2*i1+1];
		   c[0] = a[2*i1c];
		   c[1] = a[2*i1c+1];
		   break;
	      default:
		   memcpy(b, &a[N * i1], N * sizeof(R));
		   memcpy(c, &a[N * i1c], N * sizeof(R));
	  }
	  while (1) {
	       i2 = ny * i1 - k * (i1 / nx);
	       i2c = k - i2;
	       if (i1 < move_size)
		    move[i1] = 1;
	       if (i1c < move_size)
		    move[i1c] = 1;
	       ncount += 2;
	       if (i2 == i)
		    break;
	       if (i2 == kmi) {
		    d = b;
		    b = c;
		    c = d;
		    break;
	       }
	       switch (N) {
		   case 1:
			a[i1] = a[i2];
			a[i1c] = a[i2c];
			break;
		   case 2:
			a[2*i1] = a[2*i2];
			a[2*i1+1] = a[2*i2+1];
			a[2*i1c] = a[2*i2c];
			a[2*i1c+1] = a[2*i2c+1];
			break;
		   default:
			memcpy(&a[N * i1], &a[N * i2], 
			       N * sizeof(R));
			memcpy(&a[N * i1c], &a[N * i2c], 
			       N * sizeof(R));
	       }
	       i1 = i2;
	       i1c = i2c;
	  }
	  switch (N) {
	      case 1:
		   a[i1] = b[0];
		   a[i1c] = c[0];
		   break;
	      case 2:
		   a[2*i1] = b[0];
		   a[2*i1+1] = b[1];
		   a[2*i1c] = c[0];
		   a[2*i1c+1] = c[1];
		   break;
	      default:
		   memcpy(&a[N * i1], b, N * sizeof(R));
		   memcpy(&a[N * i1c], c, N * sizeof(R));
	  }
	  if (ncount >= mn)
	       break;	/* we've moved all elements */
	  
	  /** Search for loops to rearrange: **/
	  
	  while (1) {
	       INT max = k - i;
	       ++i;
	       A(i <= max);
	       im += ny;
	       if (im > k)
		    im -= k;
	       i2 = im;
	       if (i == i2)
		    continue;
	       if (i >= move_size) {
		    while (i2 > i && i2 < max) {
			 i1 = i2;
			 i2 = ny * i1 - k * (i1 / nx);
		    }
		    if (i2 == i)
			 break;
	       } else if (!move[i])
		    break;
	  }
     }
}

static void apply_toms513(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     INT n = ego->n, m = ego->m;
     INT vl = ego->vl;
     R *buf = (R *)MALLOC(sizeof(R) * ego->nbuf, BUFFERS);
     A(ego->vs == 1 && ego->s1 == vl && ego->s0 == m * vl);
     UNUSED(O);
     transpose_toms513(I, n, m, vl, (char *) (buf + 2*vl), (n+m)/2, buf);
     X(ifree)(buf);
}

static int applicable_toms513(const problem_rdft *p, planner *plnr,
			   int dim0, int dim1, int dim2, INT *nbuf)
{
     INT n = p->vecsz->dims[dim0].n;
     INT m = p->vecsz->dims[dim1].n;
     INT vl, vs;
     get_transpose_vec(p, dim2, &vl, &vs);
     *nbuf = 2*vl 
	  + ((n + m) / 2 * sizeof(char) + sizeof(R) - 1) / sizeof(R);
     return (!NO_SLOWP(plnr)
	     && n != m
	     && Ntuple_transposable(p->vecsz->dims + dim0,
				    p->vecsz->dims + dim1,
				    vl, vs));
}

static int mkcldrn_toms513(const problem_rdft *p, planner *plnr, P *ego)
{
     UNUSED(p); UNUSED(plnr);
     /* heuristic so that TOMS algorithm is last resort */
     ego->super.super.ops.other += ego->n * ego->m * ego->vl * 2 * 20;
     return 1;
}

static const transpose_adt adt_toms513 =
{
     apply_toms513, applicable_toms513, mkcldrn_toms513,
     "rdft-transpose-toms513"
};

/*-----------------------------------------------------------------------*/
/*-----------------------------------------------------------------------*/
/* generic stuff: */

static void awake(plan *ego_, enum wakefulness wakefulness)
{
     P *ego = (P *) ego_;
     if (ego->cld1) AWAKE(ego->cld1, wakefulness);
     if (ego->cld2) AWAKE(ego->cld2, wakefulness);
     if (ego->cld3) AWAKE(ego->cld3, wakefulness);
}

static void print(const plan *ego_, printer *p)
{
     const P *ego = (const P *) ego_;
     p->print(p, "(%s-%Dx%D%v", ego->slv->adt->nam,
	      ego->n, ego->m, ego->vl);
     if (ego->cld1) p->print(p, "%(%p%)", ego->cld1);
     if (ego->cld2) p->print(p, "%(%p%)", ego->cld2);
     if (ego->cld3) p->print(p, "%(%p%)", ego->cld3);
     p->print(p, ")");
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy_internal)(ego->cld3);
     X(plan_destroy_internal)(ego->cld2);
     X(plan_destroy_internal)(ego->cld1);
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     const problem_rdft *p;
     int dim0, dim1, dim2;
     INT nbuf;
     P *pln;

     static const plan_adt padt = {
	  X(rdft_solve), awake, print, destroy
     };

     if (!applicable(ego_, p_, plnr, &dim0, &dim1, &dim2, &nbuf))
          return (plan *) 0;

     p = (const problem_rdft *) p_;
     pln = MKPLAN_RDFT(P, &padt, ego->adt->apply);

     pln->n = p->vecsz->dims[dim0].n;
     pln->m = p->vecsz->dims[dim1].n;
     pln->s0 = p->vecsz->dims[dim0].is;
     pln->s1 = p->vecsz->dims[dim1].is;
     get_transpose_vec(p, dim2, &pln->vl, &pln->vs);
     pln->nbuf = nbuf;
     pln->d = gcd(pln->n, pln->m);
     pln->nd = pln->n / pln->d;
     pln->md = pln->m / pln->d;
     pln->fd = pln->s0 / pln->vl;
     pln->slv = ego;

     X(ops_zero)(&pln->super.super.ops); /* mkcldrn is responsible for ops */

     pln->cld1 = pln->cld2 = pln->cld3 = 0;
     if (!ego->adt->mkcldrn(p, plnr, pln)) {
	  X(plan_destroy_internal)(&(pln->super.super));
	  return 0;
     }

     return &(pln->super.super);
}

static solver *mksolver(const transpose_adt *adt)
{
     static const solver_adt sadt = { PROBLEM_RDFT, mkplan };
     S *slv = MKSOLVER(S, &sadt);
     slv->adt = adt;
     return &(slv->super);
}

void X(rdft_vrank3_transpose_register)(planner *p)
{
     unsigned i;
     static const transpose_adt *const adts[] = {
	  &adt_gcd, &adt_cut, &adt_toms513
     };
     for (i = 0; i < sizeof(adts) / sizeof(adts[0]); ++i)
          REGISTER_SOLVER(p, mksolver(adts[i]));
}
