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

#include "api.h"
#include "fftw3-mpi.h"
#include "ifftw-mpi.h"
#include "mpi-transpose.h"
#include "mpi-dft.h"

/* Convert API flags to internal MPI flags. */
#define MPI_FLAGS(f) ((f) >> 28)

/*************************************************************************/

static int mpi_inited = 0;

static double cost_hook(const problem *p, double t, cost_kind k)
{
     MPI_Comm comm;
     double tsum;
     switch (p->adt->problem_kind) {
	 case PROBLEM_MPI_DFT:
	      comm = ((const problem_mpi_dft *) p)->comm;
	      break;
	 case PROBLEM_MPI_TRANSPOSE:
	      comm = ((const problem_mpi_transpose *) p)->comm;
	      break;
	 default:
	      return t;
     }
     MPI_Allreduce(&t, &tsum, 1, MPI_DOUBLE, 
		   k == COST_SUM ? MPI_SUM : MPI_MAX, comm);
     return tsum;
}

void XM(init)(void)
{
     if (!mpi_inited) {
	  planner *plnr = X(the_planner)();
	  plnr->cost_hook = cost_hook;
          XM(conf_standard)(plnr);
	  mpi_inited = 1;	  
     }
}

void XM(cleanup)(void)
{
     X(cleanup)();
     mpi_inited = 0;
}

/*************************************************************************/

static dtensor *mkdtensor_api(int rnk, const XM(ddim) *dims0)
{
     dtensor *x = XM(mkdtensor)(rnk);
     int i;
     for (i = 0; i < rnk; ++i) {
	  x->dims[i].n = dims0[i].n;
	  x->dims[i].b[IB] = dims0[i].ib;
	  x->dims[i].b[OB] = dims0[i].ob;
     }
     return x;
}

static dtensor *default_sz(int rnk, const XM(ddim) *dims0, int n_pes)
{
     dtensor *sz = XM(mkdtensor)(rnk);
     dtensor *sz0 = mkdtensor_api(rnk, dims0);
     block_kind k;
     int i;

     for (i = 0; i < rnk; ++i) {
	  sz->dims[i].n = dims0[i].n;
	  sz->dims[i].b[IB] = dims0[i].ib ? dims0[i].ib : sz->dims[i].n;
	  sz->dims[i].b[OB] = dims0[i].ob ? dims0[i].ob : sz->dims[i].n;
     }

     /* If we haven't used all of the processes yet, and some of the
	block sizes weren't specified (i.e. 0), then set the
	unspecified blocks so as to use as many processes as
	possible with as few distributed dimensions as possible. */
     for (k = IB; k <= OB; ++k) {
	  INT nb = XM(num_blocks_total)(sz, k);
	  INT np = n_pes / nb;
	  for (i = 0; i < rnk && np > 1; ++i)
	       if (!sz0->dims[i].b[k]) {
		    sz->dims[i].b[k] = XM(default_block)(sz->dims[i].n, np);
		    nb *= XM(num_blocks)(sz->dims[i].n, sz->dims[i].b[k]);
		    np = n_pes / nb;
	       }
     }

     XM(dtensor_destroy)(sz0);
     sz0 = XM(dtensor_canonical)(sz);
     XM(dtensor_destroy)(sz);
     return sz0;
}

/* allocate simple local (serial) dims array corresponding to n[rnk] */
static XM(ddim) *simple_dims(int rnk, const ptrdiff_t *n)
{
     XM(ddim) *dims = (XM(ddim) *) MALLOC(sizeof(XM(ddim)) * rnk,
						TENSORS);
     int i;
     for (i = 0; i < rnk; ++i)
	  dims[i].n = dims[i].ib = dims[i].ob = n[i];
     return dims;
}

/*************************************************************************/

static void local_size(int my_pe, const dtensor *sz, block_kind k,
		       ptrdiff_t *local_n, ptrdiff_t *local_start)
{
     int i;
     if (my_pe >= XM(num_blocks_total)(sz, k))
	  for (i = 0; i < sz->rnk; ++i)
	       local_n[i] = local_start[i] = 0;
     else {
	  XM(block_coords)(sz, k, my_pe, local_start);
	  for (i = 0; i < sz->rnk; ++i) {
	       local_n[i] = XM(block)(sz->dims[i].n, sz->dims[i].b[k],
				      local_start[i]);
	       local_start[i] *= sz->dims[i].b[k];
	  }
     }
}

static INT prod(int rnk, const ptrdiff_t *local_n) 
{
     int i;
     INT N = 1;
     for (i = 0; i < rnk; ++i) N *= local_n[i];
     return N;
}

ptrdiff_t XM(local_size_guru)(int rnk, const XM(ddim) *dims0,
			      ptrdiff_t howmany, MPI_Comm comm,
			      ptrdiff_t *local_n_in,
			      ptrdiff_t *local_start_in,
			      ptrdiff_t *local_n_out, 
			      ptrdiff_t *local_start_out,
			      int sign, unsigned flags)
{
     INT N;
     int my_pe, n_pes, i;
     dtensor *sz;

     if (rnk == 0)
	  return howmany;

     MPI_Comm_rank(comm, &my_pe);
     MPI_Comm_size(comm, &n_pes);
     sz = default_sz(rnk, dims0, n_pes);

     /* Now, we must figure out how much local space the user should
	allocate (or at least an upper bound).  This depends strongly
	on the exact algorithms we employ...ugh!  FIXME: get this info
	from the solvers somehow? */
     N = 1; /* never return zero allocation size */
     if (rnk > 1 && XM(is_block1d)(sz, IB) && XM(is_block1d)(sz, OB)) {
	  INT Nafter;
	  ddim odims[2];

	  /* dft-rank-geq2-transposed */
	  odims[0] = sz->dims[0]; odims[1] = sz->dims[1]; /* save */
	  /* we may need extra space for transposed intermediate data */
	  for (i = 0; i < 2; ++i)
	       if (XM(num_blocks)(sz->dims[i].n, sz->dims[i].b[IB]) == 1 &&
		   XM(num_blocks)(sz->dims[i].n, sz->dims[i].b[OB]) == 1) {
		    sz->dims[i].b[IB]
			 = XM(default_block)(sz->dims[i].n, n_pes);
		    sz->dims[1-i].b[IB] = sz->dims[1-i].n;
		    local_size(my_pe, sz, IB, local_n_in, local_start_in);
		    N = X(imax)(N, prod(rnk, local_n_in));
		    sz->dims[i] = odims[i];
		    sz->dims[1-i] = odims[1-i];
		    break;
	       }

	  /* dft-rank-geq2 */
	  Nafter = howmany;
	  for (i = 1; i < sz->rnk; ++i) Nafter *= sz->dims[i].n;
	  N = X(imax)(N, (sz->dims[0].n
			  * XM(block)(Nafter, XM(default_block)(Nafter, n_pes),
				      my_pe) + howmany - 1) / howmany);

	  /* dft-rank-geq2 with dimensions swapped */
	  Nafter = howmany * sz->dims[0].n;
          for (i = 2; i < sz->rnk; ++i) Nafter *= sz->dims[i].n;
          N = X(imax)(N, (sz->dims[1].n
                          * XM(block)(Nafter, XM(default_block)(Nafter, n_pes),
                                      my_pe) + howmany - 1) / howmany);
     }
     else if (rnk == 1) {
	  if (howmany >= n_pes && !flags) { /* dft-rank1-bigvec */
	       ptrdiff_t n[2], start[2];
	       dtensor *sz2 = XM(mkdtensor)(2);
	       sz2->dims[0] = sz->dims[0];
	       sz2->dims[0].b[IB] = sz->dims[0].n;
	       sz2->dims[1].n = sz2->dims[1].b[OB] = howmany;
	       sz2->dims[1].b[IB] = XM(default_block)(howmany, n_pes);
	       local_size(my_pe, sz2, IB, n, start);
	       XM(dtensor_destroy)(sz2);
	       N = X(imax)(N, (prod(2, n) + howmany - 1) / howmany);
	  }
	  else { /* dft-rank1 */
	       INT r, m, rblock[2], mblock[2];

	       /* Since the 1d transforms are so different, we require
		  the user to call local_size_1d for this case.  Ugh. */
	       CK(sign == FFTW_FORWARD || sign == FFTW_BACKWARD);

	       if ((r = XM(choose_radix)(sz->dims[0], n_pes, flags, sign,
					 rblock, mblock))) {
		    m = sz->dims[0].n / r;
		    if (flags & FFTW_MPI_SCRAMBLED_IN)
			 sz->dims[0].b[IB] = rblock[IB] * m;
		    else { /* !SCRAMBLED_IN */
			 sz->dims[0].b[IB] = r * mblock[IB];
			 N = X(imax)(N, rblock[IB] * m);
		    }
		    if (flags & FFTW_MPI_SCRAMBLED_OUT)
			 sz->dims[0].b[OB] = r * mblock[OB];
		    else { /* !SCRAMBLED_OUT */
			 N = X(imax)(N, r * mblock[OB]);
			 sz->dims[0].b[OB] = rblock[OB] * m;
		    }
	       }
	  }
     }

     local_size(my_pe, sz, IB, local_n_in, local_start_in);
     local_size(my_pe, sz, OB, local_n_out, local_start_out);

     /* at least, make sure we have enough space to store input & output */
     N = X(imax)(N, X(imax)(prod(rnk, local_n_in), prod(rnk, local_n_out)));

     XM(dtensor_destroy)(sz);
     return N * howmany;
}

ptrdiff_t XM(local_size_many_transposed)(int rnk, const ptrdiff_t *n,
					 ptrdiff_t howmany,
					 ptrdiff_t xblock, ptrdiff_t yblock,
					 MPI_Comm comm,
					 ptrdiff_t *local_nx,
					 ptrdiff_t *local_x_start,
					 ptrdiff_t *local_ny,
					 ptrdiff_t *local_y_start)
{
     ptrdiff_t N;
     XM(ddim) *dims; 
     ptrdiff_t *local;

     if (rnk == 0) {
	  *local_nx = *local_ny = 1;
	  *local_x_start = *local_y_start = 0;
	  return howmany;
     }

     dims = simple_dims(rnk, n);
     local = (ptrdiff_t *) MALLOC(sizeof(ptrdiff_t) * rnk * 4, TENSORS);

     /* default 1d block distribution, with transposed output
        if yblock < n[1] */
     dims[0].ib = xblock;
     if (rnk > 1) {
	  if (yblock < n[1])
	       dims[1].ob = yblock;
	  else
	       dims[0].ob = xblock;
     }
     else
	  dims[0].ob = xblock; /* FIXME: 1d not really supported here 
				         since we don't have flags/sign */
     
     N = XM(local_size_guru)(rnk, dims, howmany, comm, 
			     local, local + rnk,
			     local + 2*rnk, local + 3*rnk,
			     0, 0);
     *local_nx = local[0];
     *local_x_start = local[rnk];
     if (rnk > 1) {
	  *local_ny = local[2*rnk + 1];
	  *local_y_start = local[3*rnk + 1];
     }
     else {
	  *local_ny = *local_nx;
	  *local_y_start = *local_x_start;
     }
     X(ifree)(local);
     X(ifree)(dims);
     return N;
}

ptrdiff_t XM(local_size_many)(int rnk, const ptrdiff_t *n,
			      ptrdiff_t howmany, 
			      ptrdiff_t xblock,
			      MPI_Comm comm,
			      ptrdiff_t *local_nx,
			      ptrdiff_t *local_x_start)
{
     ptrdiff_t local_ny, local_y_start;
     return XM(local_size_many_transposed)(rnk, n, howmany,
					   xblock, rnk > 1 
					   ? n[1] : FFTW_MPI_DEFAULT_BLOCK,
					   comm,
					   local_nx, local_x_start,
					   &local_ny, &local_y_start);
}


ptrdiff_t XM(local_size_transposed)(int rnk, const ptrdiff_t *n,
				    MPI_Comm comm,
				    ptrdiff_t *local_nx,
				    ptrdiff_t *local_x_start,
				    ptrdiff_t *local_ny,
				    ptrdiff_t *local_y_start)
{
     return XM(local_size_many_transposed)(rnk, n, 1,
					   FFTW_MPI_DEFAULT_BLOCK,
					   FFTW_MPI_DEFAULT_BLOCK,
					   comm,
					   local_nx, local_x_start,
					   local_ny, local_y_start);
}

ptrdiff_t XM(local_size)(int rnk, const ptrdiff_t *n,
			 MPI_Comm comm,
			 ptrdiff_t *local_nx,
			 ptrdiff_t *local_x_start)
{
     return XM(local_size_many)(rnk, n, 1, FFTW_MPI_DEFAULT_BLOCK, comm,
				local_nx, local_x_start);
}

ptrdiff_t XM(local_size_many_1d)(ptrdiff_t nx, ptrdiff_t howmany, 
				 MPI_Comm comm, int sign, unsigned flags,
				 ptrdiff_t *local_nx, ptrdiff_t *local_x_start,
				 ptrdiff_t *local_ny, ptrdiff_t *local_y_start)
{
     XM(ddim) d;
     d.n = nx;
     d.ib = d.ob = FFTW_MPI_DEFAULT_BLOCK;
     return XM(local_size_guru)(1, &d, howmany, comm,
				local_nx, local_x_start,
				local_ny, local_y_start, sign, flags);
}

ptrdiff_t XM(local_size_1d)(ptrdiff_t nx,
			    MPI_Comm comm, int sign, unsigned flags,
			    ptrdiff_t *local_nx, ptrdiff_t *local_x_start,
			    ptrdiff_t *local_ny, ptrdiff_t *local_y_start)
{
     return XM(local_size_many_1d)(nx, 1, comm, sign, flags,
				   local_nx, local_x_start,
				   local_ny, local_y_start);
}

ptrdiff_t XM(local_size_2d_transposed)(ptrdiff_t nx, ptrdiff_t ny,
				       MPI_Comm comm,
				       ptrdiff_t *local_nx,
				       ptrdiff_t *local_x_start,
				       ptrdiff_t *local_ny, 
				       ptrdiff_t *local_y_start)
{
     ptrdiff_t n[2];
     n[0] = nx; n[1] = ny;
     return XM(local_size_transposed)(2, n, comm,
				      local_nx, local_x_start,
				      local_ny, local_y_start);
}

ptrdiff_t XM(local_size_2d)(ptrdiff_t nx, ptrdiff_t ny, MPI_Comm comm,
			       ptrdiff_t *local_nx, ptrdiff_t *local_x_start)
{
     ptrdiff_t n[2];
     n[0] = nx; n[1] = ny;
     return XM(local_size)(2, n, comm, local_nx, local_x_start);
}

ptrdiff_t XM(local_size_3d_transposed)(ptrdiff_t nx, ptrdiff_t ny,
				       ptrdiff_t nz,
				       MPI_Comm comm,
				       ptrdiff_t *local_nx,
				       ptrdiff_t *local_x_start,
				       ptrdiff_t *local_ny, 
				       ptrdiff_t *local_y_start)
{
     ptrdiff_t n[3];
     n[0] = nx; n[1] = ny; n[2] = nz;
     return XM(local_size_transposed)(3, n, comm,
				      local_nx, local_x_start,
				      local_ny, local_y_start);
}

ptrdiff_t XM(local_size_3d)(ptrdiff_t nx, ptrdiff_t ny, ptrdiff_t nz,
			    MPI_Comm comm,
			    ptrdiff_t *local_nx, ptrdiff_t *local_x_start)
{
     ptrdiff_t n[3];
     n[0] = nx; n[1] = ny; n[2] = nz;
     return XM(local_size)(3, n, comm, local_nx, local_x_start);
}

/*************************************************************************/
/* Transpose API */

X(plan) XM(plan_many_transpose)(ptrdiff_t nx, ptrdiff_t ny, 
				ptrdiff_t howmany,
				ptrdiff_t xblock, ptrdiff_t yblock,
				R *in, R *out, 
				MPI_Comm comm, unsigned flags)
{
     int n_pes;
     XM(init)();

     if (howmany < 0 || xblock < 0 || yblock < 0 ||
	 nx <= 0 || ny <= 0) return 0;

     MPI_Comm_size(comm, &n_pes);
     if (!xblock) xblock = XM(default_block)(nx, n_pes);
     if (!yblock) yblock = XM(default_block)(ny, n_pes);
     if (n_pes < XM(num_blocks)(nx, xblock)
	 || n_pes < XM(num_blocks)(ny, yblock))
	  return 0;

     return 
	  X(mkapiplan)(FFTW_FORWARD, flags,
		       XM(mkproblem_transpose)(nx, ny, howmany,
					       in, out, xblock, yblock,
					       comm, MPI_FLAGS(flags)));
}

X(plan) XM(plan_transpose)(ptrdiff_t nx, ptrdiff_t ny, R *in, R *out, 
			   MPI_Comm comm, unsigned flags)
			      
{
     return XM(plan_many_transpose)(nx, ny, 1,
				    FFTW_MPI_DEFAULT_BLOCK,
				    FFTW_MPI_DEFAULT_BLOCK,
				    in, out, comm, flags);
}

/*************************************************************************/
/* Complex DFT API */

X(plan) XM(plan_guru_dft)(int rnk, const XM(ddim) *dims0,
			  ptrdiff_t howmany,
			  C *in, C *out,
			  MPI_Comm comm, int sign, unsigned flags)
{
     int n_pes, i;
     dtensor *sz;
     
     XM(init)();

     if (howmany < 0 || rnk < 1) return 0;
     for (i = 0; i < rnk; ++i)
	  if (dims0[i].n < 1 || dims0[i].ib < 0 || dims0[i].ob < 0)
	       return 0;

     MPI_Comm_size(comm, &n_pes);
     sz = default_sz(rnk, dims0, n_pes);

     if (XM(num_blocks_total)(sz, IB) > n_pes
	 || XM(num_blocks_total)(sz, OB) > n_pes) {
	  XM(dtensor_destroy)(sz);
	  return 0;
     }

     return
          X(mkapiplan)(sign, flags,
                       XM(mkproblem_dft_d)(sz, howmany,
					   (R *) in, (R *) out,
					   comm, sign, 
					   MPI_FLAGS(flags)));
}

X(plan) XM(plan_many_dft)(int rnk, const ptrdiff_t *n,
			  ptrdiff_t howmany,
			  ptrdiff_t iblock, ptrdiff_t oblock,
			  C *in, C *out,
			  MPI_Comm comm, int sign, unsigned flags)
{
     XM(ddim) *dims = simple_dims(rnk, n);
     X(plan) pln;

     if (rnk == 1) {
	  dims[0].ib = iblock;
	  dims[0].ob = oblock;
     }
     else if (rnk > 1) {
	  dims[0 != (flags & FFTW_MPI_TRANSPOSED_IN)].ib = iblock;
	  dims[0 != (flags & FFTW_MPI_TRANSPOSED_OUT)].ob = oblock;
     }

     pln = XM(plan_guru_dft)(rnk,dims,howmany, in,out, comm, sign, flags);
     X(ifree)(dims);
     return pln;
}

X(plan) XM(plan_dft)(int rnk, const ptrdiff_t *n, C *in, C *out,
		     MPI_Comm comm, int sign, unsigned flags)
{
     return XM(plan_many_dft)(rnk, n, 1,
			      FFTW_MPI_DEFAULT_BLOCK,
			      FFTW_MPI_DEFAULT_BLOCK,
			      in, out, comm, sign, flags);
}

X(plan) XM(plan_dft_1d)(ptrdiff_t nx, C *in, C *out,
			MPI_Comm comm, int sign, unsigned flags)
{
     return XM(plan_dft)(1, &nx, in, out, comm, sign, flags);
}

X(plan) XM(plan_dft_2d)(ptrdiff_t nx, ptrdiff_t ny, C *in, C *out,
			MPI_Comm comm, int sign, unsigned flags)
{
     ptrdiff_t n[2];
     n[0] = nx; n[1] = ny;
     return XM(plan_dft)(2, n, in, out, comm, sign, flags);
}

X(plan) XM(plan_dft_3d)(ptrdiff_t nx, ptrdiff_t ny, ptrdiff_t nz,
			C *in, C *out,
			MPI_Comm comm, int sign, unsigned flags)
{
     ptrdiff_t n[3];
     n[0] = nx; n[1] = ny; n[2] = nz;
     return XM(plan_dft)(3, n, in, out, comm, sign, flags);
}
