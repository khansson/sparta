/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.sandia.gov
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Arnaud Borner (NASA Ames)
------------------------------------------------------------------------- */

#include "mpi.h"
#include "math.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "read_isurf.h"
#include "surf.h"
#include "domain.h"
#include "grid.h"
#include "comm.h"
#include "modify.h"
#include "fix_ablate.h"
#include "input.h"
#include "irregular.h"
#include "geometry.h"
#include "lookup_table.h"
#include "my_page.h"
#include "memory.h"
#include "error.h"

using namespace SPARTA_NS;

// prototype for non-class function

int compare_indices(const void *, const void *);

enum{NEITHER,BAD,GOOD};
enum{NONE,CHECK,KEEP};
enum{UNKNOWN,OUTSIDE,INSIDE,OVERLAP};           // several files
enum{XLO,XHI,YLO,YHI,ZLO,ZHI,INTERIOR};         // same as Domain
enum{NCHILD,NPARENT,NUNKNOWN,NPBCHILD,NPBPARENT,NPBUNKNOWN,NBOUND};  // Grid

#define CHUNK 8192
#define DELTA 128
#define BIG 1.0e20
#define EPSILON 1.0e-16

/* ---------------------------------------------------------------------- */

ReadISurf::ReadISurf(SPARTA *sparta) : Pointers(sparta)
{
  MPI_Comm_rank(world,&me);

  cvalues = NULL;
  svalues = NULL;
}

/* ---------------------------------------------------------------------- */

ReadISurf::~ReadISurf()
{
  memory->destroy(cvalues);
  memory->destroy(svalues);
}

/* ---------------------------------------------------------------------- */

void ReadISurf::command(int narg, char **arg)
{
  // NOTE: at some point could allow another chunk of isurfs to be read
  //       see note below about clear_surf()

  if (!grid->exist)
    error->all(FLERR,"Cannot read_isurf before grid is defined");
  if (!surf->implicit)
    error->all(FLERR,"Cannot read_isurf unless global surfs implicit is set");
  if (surf->exist)
    error->all(FLERR,"Cannot read_isurf when surfs already exist");
  if (particle->exist)
    error->all(FLERR,"Cannot read_isurf when particles exist");
  if (domain->axisymmetric)
    error->all(FLERR,"Cannot read_isurf for axisymmetric domains");

  surf->exist = 1;
  dimension = domain->dimension;

  if (narg < 6) error->all(FLERR,"Illegal read_isurf command");

  ggroup = grid->find_group(arg[0]);
  if (ggroup < 0) error->all(FLERR,"Read_isurf grid group ID does not exist");

  nx = input->inumeric(FLERR,arg[1]);
  ny = input->inumeric(FLERR,arg[2]);
  nz = input->inumeric(FLERR,arg[3]);

  if (dimension == 2 && nz != 1) error->all(FLERR,"Invalid read_isurf command");

  char *gridfile = arg[4];

  thresh = input->numeric(FLERR,arg[5]);
  if (thresh <= 0 || thresh >= 255) 
    error->all(FLERR,"Invalid read_isurf command");
  int ithresh = static_cast<int> (thresh);
  if (ithresh == thresh) 
    error->all(FLERR,"An integer value for read_isurf thresh is not allowed");

  // process command line args

  process_args(narg-6,&arg[6]);

  // verify that grid group is a set of uniform child cells
  // must comprise a 3d contiguous block

  int nxyz[3];
  int count = grid->check_uniform_group(ggroup,nxyz,corner,xyzsize);
  if (nx != nxyz[0] || ny != nxyz[1] || nz != nxyz[2])
    error->all(FLERR,"Read_isurf grid group does not match nx,ny,nz");

  // read grid corner point values
  // create and destroy dictionary of my grid cells in group
  //   used to assign per-grid values to local grid cells

  if (screen && me == 0) fprintf(screen,"Reading isurf file ...\n");

  MPI_Barrier(world);
  double time1 = MPI_Wtime();

  create_hash(count);

  if (dimension == 3) {
    memory->create(cvalues,grid->nlocal,8,"readisurf:cvalues");
    memset(&cvalues[0][0],0,grid->nlocal*8*sizeof(int));
  } else {
    memory->create(cvalues,grid->nlocal,4,"readisurf:cvalues");
    memset(&cvalues[0][0],0,grid->nlocal*4*sizeof(int));
  }

  read_corners(gridfile);

  if (typefile) {
    memory->create(svalues,grid->nlocal,"readisurf:svalues");
    read_types(typefile);
  }

  delete hash;

  // create surfs in each grid cell based on corner point values
  // call clear_surf first so cell/corner flags are all set
  // set surf->nsurf and surf->nown
  // if specified, apply group keyword to reset per-surf mask info

  MPI_Barrier(world);
  double time2 = MPI_Wtime();

  grid->clear_surf();

  if (dimension == 3) marching_cubes();
  else marching_squares();

  surf->nown = surf->nlocal;
  bigint nlocal = surf->nlocal;
  MPI_Allreduce(&nlocal,&surf->nsurf,1,MPI_SPARTA_BIGINT,MPI_SUM,world);
  
  if (sgrouparg) {
    int sgroup = surf->find_group(arg[sgrouparg]);
    if (sgroup < 0) sgroup = surf->add_group(arg[sgrouparg]);
    int sgroupbit = surf->bitmask[sgroup];
 
    int nsurf = surf->nlocal;
    if (dimension == 3) {
      Surf::Tri *tris = surf->tris;
      for (int i = 0; i < nsurf; i++) tris[i].mask |= sgroupbit;
    } else {
      Surf::Line *lines = surf->lines;
      for (int i = 0; i < nsurf; i++) lines[i].mask |= sgroupbit;
    }
  }

  // output extent of implicit surfs, some may be tiny

  if (dimension == 2) surf->output_extent(0);
  else surf->output_extent(0);

  MPI_Barrier(world);
  double time3 = MPI_Wtime();

  // compute normals of new surfs

  if (dimension == 2) surf->compute_line_normal(0);
  else surf->compute_tri_normal(0);

  // cleanup_MC() checks for consistent triangles on grid cell faces
  // needs to come after normals are computed

  if (dimension == 3) cleanup_MC();

  // watertight check can be done before surfs are mapped to grid cells

  if (dimension == 2) surf->check_watertight_2d();
  else surf->check_watertight_3d();

  MPI_Barrier(world);
  double time4 = MPI_Wtime();

  // -----------------------
  // map surfs to grid cells
  // -----------------------

  // surfs are already assigned to grid cells
  // create split cells due to new surfs

  grid->unset_neighbors();
  grid->remove_ghosts();

  grid->surf2grid_implicit(1,1);

  MPI_Barrier(world);
  double time5 = MPI_Wtime();

  // re-setup grid ghosts and neighbors

  grid->setup_owned();
  grid->acquire_ghosts();
  grid->reset_neighbors();
  comm->reset_neighbors();

  MPI_Barrier(world);
  double time6 = MPI_Wtime();

  // flag cells and corners as OUTSIDE or INSIDE

  grid->set_inout();
  grid->type_check();

  MPI_Barrier(world);
  double time7 = MPI_Wtime();

  // store corner point values in FixAblate if requested
  // cannot do until now, b/c need split cells to exist via surf2grid()

  if (storeflag) ablate->store_corners(cvalues);

  MPI_Barrier(world);
  double time8 = MPI_Wtime();

  // stats

  double time_total = time8-time1;
  double time_s2g = time5-time4;

  if (comm->me == 0) {
    if (screen) {
      fprintf(screen,"  CPU time = %g secs\n",time_total);
      fprintf(screen,"  read/marching/check/surf2grid/ghost/inout/store "
              "percent = %g %g %g %g %g %g %g\n",
              100.0*(time2-time1)/time_total,100.0*(time3-time2)/time_total,
              100.0*(time4-time3)/time_total,100.0*(time5-time4)/time_total,
              100.0*(time6-time5)/time_total,100.0*(time7-time6)/time_total,
              100.0*(time8-time7)/time_total);
      fprintf(screen,"  surf2grid time = %g secs\n",time_s2g);
      fprintf(screen,"  map/rvous/split percent = %g %g %g\n",
              100.0*grid->tmap/time_s2g,100.0*grid->trvous1/time_s2g,
              100.0*grid->tsplit/time_s2g);
    }
    if (logfile) {
      fprintf(logfile,"  CPU time = %g secs\n",time_total);
      fprintf(logfile,"  read/marching/check/surf2grid/ghost/inout/store "
              "percent = %g %g %g %g %g %g %g\n",
              100.0*(time2-time1)/time_total,100.0*(time3-time2)/time_total,
              100.0*(time4-time3)/time_total,100.0*(time5-time4)/time_total,
              100.0*(time6-time5)/time_total,100.0*(time7-time6)/time_total,
              100.0*(time8-time7)/time_total);
      fprintf(logfile,"  surf2grid time = %g secs\n",time_s2g);
      fprintf(logfile,"  map/rvous/split percent = %g %g %g\n",
              100.0*grid->tmap/time_s2g,100.0*grid->trvous1/time_s2g,
              100.0*grid->tsplit/time_s2g);
    }
  }
}

/* ----------------------------------------------------------------------
   read/store all grid corner point values
------------------------------------------------------------------------- */

void ReadISurf::read_corners(char *gridfile)
{
  int nchunk;
  int nxyz[3];
  FILE *fp;

  uint8_t *buf;
  memory->create(buf,CHUNK,"readisurf:buf");

  // proc 0 opens and reads binary file
  // error check the file grid matches input script extent

  if (me == 0) {
    fp = fopen(gridfile,"rb");
    if (fp == NULL) {
      char str[128];
      snprintf(str,128,"Cannot open read_isurf grid corner point file %s",
               gridfile);
      error->one(FLERR,str);
    }
    fread(nxyz,sizeof(int),dimension,fp);
  }

  MPI_Bcast(nxyz,dimension,MPI_INT,0,world);

  int flag = 0;
  if (nxyz[0] != nx+1) flag = 1;
  if (nxyz[1] != ny+1) flag = 1;
  if (dimension == 3 && nxyz[2] != nz+1) flag = 1;
  if (flag) 
    error->all(FLERR,"Grid size in read_isurf grid corner point file "
               "does not match request");

  // read and broadcast one CHUNK of values at a time
  // each proc stores grid corner point values it needs in assign_corners()

  bigint ncorners;
  if (dimension == 3) ncorners = (bigint) (nx+1) * (ny+1)*(nz+1);
  else ncorners = (bigint) (nx+1) * (ny+1)*nz;

  bigint nread = 0;

  while (nread < ncorners) {
    if (ncorners-nread > CHUNK) nchunk = CHUNK;
    else nchunk = ncorners-nread;

    if (me == 0) fread(buf,sizeof(uint8_t),nchunk,fp);
    MPI_Bcast(buf,nchunk,MPI_CHAR,0,world);

    assign_corners(nchunk,nread,buf);
    nread += nchunk;
  }

  if (me == 0) {
    if (screen) fprintf(screen,"  " BIGINT_FORMAT " corner points\n",ncorners);
    if (logfile) fprintf(logfile,"  " BIGINT_FORMAT " corner points\n",ncorners);
  }

  memory->destroy(buf);

  // close file

  if (me == 0) fclose(fp);
}

/* ----------------------------------------------------------------------
   read/store all grid surface type values
------------------------------------------------------------------------- */

void ReadISurf::read_types(char *typefile)
{
  int nchunk;
  int nxyz[3];
  FILE *fp;

  int *buf;
  memory->create(buf,CHUNK,"readisurf:buf");

  // proc 0 opens and reads binary file
  // error check the file grid matches input script extent

  if (me == 0) {
    fp = fopen(typefile,"rb");
    if (fp == NULL) {
      char str[128];
      snprintf(str,128,"Cannot open read_isurf type file %s",typefile);
      error->one(FLERR,str);
    }
    fread(nxyz,sizeof(int),dimension,fp);
  }

  MPI_Bcast(nxyz,dimension,MPI_INT,0,world);

  int flag = 0;
  if (nxyz[0] != nx) flag = 1;
  if (nxyz[1] != ny) flag = 1;
  if (dimension == 3 && nxyz[2] != nz) flag = 1;
  if (flag) 
    error->all(FLERR,"Grid size in read_isurf type file does not match request");

  // read and broadcast one CHUNK of values at a time
  // each proc stores grid corner point values it needs in assign_corners()

  bigint ntypes = (bigint) nx * ny*nz;
  bigint nread = 0;

  while (nread < ntypes) {
    if (ntypes-nread > CHUNK) nchunk = CHUNK;
    else nchunk = ntypes-nread;

    if (me == 0) fread(buf,sizeof(int),nchunk,fp);
    MPI_Bcast(buf,nchunk,MPI_INT,0,world);

    assign_types(nchunk,nread,buf);
    nread += nchunk;
  }

  if (me == 0) {
    if (screen) fprintf(screen,"  " BIGINT_FORMAT " surface types\n",ntypes);
    if (logfile) fprintf(logfile,"  " BIGINT_FORMAT " surface types\n",ntypes);
  }

  memory->destroy(buf);

  // close file

  if (me == 0) fclose(fp);
}

/* ----------------------------------------------------------------------
   create hash for my grid cells in group
   key = index (0 to N-1) of grid cell in Nx by Ny by Nz contiguous block
   value = my local icell
   NOTE: could use count to prealloc the hash size
------------------------------------------------------------------------- */

void ReadISurf::create_hash(int count)
{
  hash = new MyHash;

  Grid::ChildCell *cells = grid->cells;
  Grid::ChildInfo *cinfo = grid->cinfo;
  int nglocal = grid->nlocal;
  int groupbit = grid->bitmask[ggroup];

  int ix,iy,iz;
  bigint index;

  for (int icell = 0; icell < nglocal; icell++) {
    if (!(cinfo[icell].mask & groupbit)) continue;
    ix = static_cast<int> ((cells[icell].lo[0]-corner[0]) / xyzsize[0] + 0.5);
    iy = static_cast<int> ((cells[icell].lo[1]-corner[1]) / xyzsize[1] + 0.5);
    iz = static_cast<int> ((cells[icell].lo[2]-corner[2]) / xyzsize[2] + 0.5);
    index = (bigint) nx * ny*iz + nx*iy + ix;
    (*hash)[index] = icell;
  }
}

/* ----------------------------------------------------------------------
   store all grid corner point values
   use hash to see if I own any grid cells that contain a corner point
   each corner point can be stored by as many as 4 or 8 grid cells
   check that corner point values = 0 on boundary of grid block
------------------------------------------------------------------------- */

void ReadISurf::assign_corners(int n, bigint offset, uint8_t *buf)
{
  int icell,ncorner,zeroflag;
  int pix,piy,piz;
  bigint pointindex,cellindex;

  for (int i = 0; i < n; i++) {
    pointindex = offset + i;
    pix = pointindex % (nx+1);
    piy = (pointindex / (nx+1)) % (ny+1);
    piz = pointindex / ((nx+1)*(ny+1));

    zeroflag = 0;
    if (buf[i]) {
      if (pix == 0 || piy == 0) zeroflag = 1;
      if (pix == nx || piy == ny) zeroflag = 1;
      if (dimension == 3 && (piz == 0 || piz == nz)) zeroflag = 1;
      if (zeroflag) error->all(FLERR,"Grid boundary value != 0");
    }

    // ncorner = 0,1,2,3,4,5,6,7 when corner point is 
    //   bottom-lower-left, bottom-lower-right, 
    //   bottom-upper-left, bottom-upper-right,
    //   top-lower-left, top-lower-right, top-upper-left, top-upper-right
    //   of cell
    // if test on cix,ciy,ciz excludes cells that are outside of grid block

    if (dimension == 3) {
      ncorner = 8;
      for (int ciz = piz-1; ciz <= piz; ciz++) {
        for (int ciy = piy-1; ciy <= piy; ciy++) {
          for (int cix = pix-1; cix <= pix; cix++) {
            ncorner--;
            if (cix < 0 || cix >= nx || ciy < 0 || ciy >=ny || 
                ciz < 0 || ciz >= nz) continue;
            cellindex = (bigint) nx * ny*ciz + nx*ciy + cix;
            if (hash->find(cellindex) == hash->end()) continue;
            icell = (*hash)[cellindex];
            cvalues[icell][ncorner] = buf[i];
          }
        }
      }

    // ncorner = 0,1,2,3 when corner point is 
    //   lower-left, lower-right, upper-left, upper-right of cell
    // if test on cix,ciy excludes cells that are outside of grid block

    } else {
      ncorner = 4;
      for (int ciy = piy-1; ciy <= piy; ciy++) {
        for (int cix = pix-1; cix <= pix; cix++) {
          ncorner--;
          if (cix < 0 || cix >= nx || ciy < 0 || ciy >=ny) continue;
          cellindex = (bigint) nx * ciy + cix;
          if (hash->find(cellindex) == hash->end()) continue;
          icell = (*hash)[cellindex];
          cvalues[icell][ncorner] = buf[i];
        }
      }
    }
  }
}

/* ----------------------------------------------------------------------
   store all grid surf type values
   use hash to see if I own grid cell corresponding to index (0 to N-1)
------------------------------------------------------------------------- */

void ReadISurf::assign_types(int n, bigint offset, int *buf)
{
  int icell;
  bigint cellindex;

  for (int i = 0; i < n; i++) {
    cellindex = offset + i;
    if (hash->find(cellindex) == hash->end()) continue;
    icell = (*hash)[cellindex];
    svalues[icell] = buf[i];
  }
}

/* ----------------------------------------------------------------------
   process command line args
------------------------------------------------------------------------- */

void ReadISurf::process_args(int narg, char **arg)
{
  sgrouparg = 0;
  typefile = NULL;
  storeflag = 0;

  int iarg = 0;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"group") == 0)  {
      if (iarg+2 > narg) error->all(FLERR,"Invalid read_isurf command");
      sgrouparg = iarg+1;
      iarg += 2;
    } else if (strcmp(arg[iarg],"type") == 0)  {
      if (iarg+2 > narg) error->all(FLERR,"Invalid read_isurf command");
      typefile = arg[iarg+1];
      iarg += 2;
    } else if (strcmp(arg[iarg],"store") == 0)  {
      if (iarg+2 > narg) error->all(FLERR,"Invalid read_isurf command");
      storeflag = 1;
      storeID = arg[iarg+1];
      int ifix = modify->find_fix(storeID);
      if (ifix < 0)
        error->all(FLERR,"Fix ID for read_isurf store does not exist");
      if (strcmp(modify->fix[ifix]->style,"ablate") != 0)
        error->all(FLERR,"Fix for read_isurf store is not a fix ablate");
      FixAblate *ablate = (FixAblate *) modify->fix[ifix];
      if (ggroup != ablate->igroup)
        error->all(FLERR,"Read_isurf group does not match fix ablate group");
      iarg += 2;
    } else error->all(FLERR,"Invalid read_isurf command");
  }
}

// ----------------------------------------------------------------------
// ----------------------------------------------------------------------
// Marching squares (2d) and Marching cubes (3d) algorithms
// ----------------------------------------------------------------------
// ----------------------------------------------------------------------

/* ----------------------------------------------------------------------
   interpolate function used by both marching squares and cubes
   lo/hi = coordinates of end points of edge of square
   v0/v1 = values at lo/hi end points
   value = interpolated coordinate for thresh value
------------------------------------------------------------------------- */

double ReadISurf::interpolate(int v0, int v1, double lo, double hi)
{
  double value = lo + (hi-lo)*(thresh-v0)/(v1-v0);
  value = MAX(value,lo);
  value = MIN(value,hi);
  return value;
}
 
/* ----------------------------------------------------------------------
   create 2d implicit surfs from grid point values
   follows https://en.wikipedia.org/wiki/Marching_squares
   see 2 sections: Basic algorithm and Disambiguation of saddle points
     treating open circles as flow volume, solid circles as material
     NOTE: Wiki page numbers points counter-clockwise
           SPARTA numbers them in x, then in y
           so bit2 and bit3 are swapped below
           this gives case #s here consistent with Wiki page
   process each grid cell independently
   4 corner points open/solid -> 2^4 = 16 cases
   cases infer 0,1,2 line segments in each grid cell
   order 2 points in each line segment to give normal into flow volume
   treat two saddle point cases (my 9,6) (Wiki 5,10)
     based on ave value at cell center
------------------------------------------------------------------------- */

void ReadISurf::marching_squares()
{
  int i,ipt,isurf,nsurf,which,splitflag;
  int v00,v01,v10,v11,bit0,bit1,bit2,bit3;
  double ave;
  double *lo,*hi;
  surfint *ptr;

  double pt[4][3];
  pt[0][2] = pt[1][2] = pt[2][2] = pt[3][2] = 0.0;

  Grid::ChildCell *cells = grid->cells;
  Grid::ChildInfo *cinfo = grid->cinfo;
  MyPage<surfint> *csurfs = grid->csurfs;
  int nglocal = grid->nlocal;
  int groupbit = grid->bitmask[ggroup];

  for (int icell = 0; icell < nglocal; icell++) {
    if (!(cinfo[icell].mask & groupbit)) continue;
    lo = cells[icell].lo;
    hi = cells[icell].hi;

    // cvalues are ordered lower-left, lower-right, upper-left, upper-right
    // Vyx encodes this as 0/1 in each dim

    v00 = cvalues[icell][0];
    v01 = cvalues[icell][1];
    v10 = cvalues[icell][2];
    v11 = cvalues[icell][3];
    
    // make last 2 bits consistent with Wiki page (see NOTE above)

    bit0 = v00 <= thresh ? 0 : 1;
    bit1 = v01 <= thresh ? 0 : 1;
    bit2 = v11 <= thresh ? 0 : 1;
    bit3 = v10 <= thresh ? 0 : 1;
    
    which = (bit3 << 3) + (bit2 << 2) + (bit1 << 1) + bit0;
    splitflag = 0;

    switch (which) {

    case 0: 
      nsurf = 0;
      break;

    case 1:
      nsurf = 1;
      pt[0][0] = lo[0];
      pt[0][1] = interpolate(v00,v10,lo[1],hi[1]);
      pt[1][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[1][1] = lo[1];
      break;

    case 2:
      nsurf = 1;
      pt[0][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[0][1] = lo[1];
      pt[1][0] = hi[0];
      pt[1][1] = interpolate(v01,v11,lo[1],hi[1]);
      break;

    case 3:
      nsurf = 1;
      pt[0][0] = lo[0];
      pt[0][1] = interpolate(v00,v10,lo[1],hi[1]);
      pt[1][0] = hi[0];
      pt[1][1] = interpolate(v01,v11,lo[1],hi[1]);
      break;

    case 4:
      nsurf = 1;
      pt[0][0] = hi[0];
      pt[0][1] = interpolate(v01,v11,lo[1],hi[1]);
      pt[1][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[1][1] = hi[1];
      break;

    case 5:
      nsurf = 2;
      ave = 0.25 * (v00 + v01 + v10 + v11);
      if (ave > thresh) {
        splitflag = 1;
        pt[0][0] = lo[0];
        pt[0][1] = interpolate(v00,v10,lo[1],hi[1]);
        pt[1][0] = interpolate(v10,v11,lo[0],hi[0]);
        pt[1][1] = hi[1];
        pt[2][0] = hi[0];
        pt[2][1] = interpolate(v01,v11,lo[1],hi[1]);
        pt[3][0] = interpolate(v00,v01,lo[0],hi[0]);
        pt[3][1] = lo[1];
      } else {
        pt[0][0] = lo[0];
        pt[0][1] = interpolate(v00,v10,lo[1],hi[1]);
        pt[1][0] = interpolate(v00,v01,lo[0],hi[0]);
        pt[1][1] = lo[1];
        pt[2][0] = hi[0];
        pt[2][1] = interpolate(v01,v11,lo[1],hi[1]);
        pt[3][0] = interpolate(v10,v11,lo[0],hi[0]);
        pt[3][1] = hi[1];
      }
      break;

    case 6:
      nsurf = 1;
      pt[0][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[0][1] = lo[1];
      pt[1][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[1][1] = hi[1];
      break;

    case 7:
      nsurf = 1;
      pt[0][0] = lo[0];
      pt[0][1] = interpolate(v00,v10,lo[1],hi[1]);
      pt[1][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[1][1] = hi[1];
      break;

    case 8:
      nsurf = 1;
      pt[0][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[0][1] = hi[1];
      pt[1][0] = lo[0];
      pt[1][1] = interpolate(v00,v10,lo[1],hi[1]);
      break;

    case 9:
      nsurf = 1;
      pt[0][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[0][1] = hi[1];
      pt[1][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[1][1] = lo[1];
      break;

    case 10:
      nsurf = 2;
      ave = 0.25 * (v00 + v01 + v10 + v11);
      if (ave > thresh) {
        splitflag = 1;
        pt[0][0] = interpolate(v00,v01,lo[0],hi[0]);
        pt[0][1] = lo[1];
        pt[1][0] = lo[0];
        pt[1][1] = interpolate(v00,v10,lo[1],hi[1]);
        pt[2][0] = interpolate(v10,v11,lo[0],hi[0]);
        pt[2][1] = hi[1];
        pt[3][0] = hi[0];
        pt[3][1] = interpolate(v01,v11,lo[1],hi[1]);
      } else {
        pt[0][0] = interpolate(v10,v11,lo[0],hi[0]);
        pt[0][1] = hi[1];
        pt[1][0] = lo[0];
        pt[1][1] = interpolate(v00,v10,lo[1],hi[1]);
        pt[2][0] = interpolate(v00,v01,lo[0],hi[0]);
        pt[2][1] = lo[1];
        pt[3][0] = hi[0];
        pt[3][1] = interpolate(v01,v11,lo[1],hi[1]);
      }
      break;

    case 11:
      nsurf = 1;
      pt[0][0] = interpolate(v10,v11,lo[0],hi[0]);
      pt[0][1] = hi[1];
      pt[1][0] = hi[0];
      pt[1][1] = interpolate(v01,v11,lo[1],hi[1]);
      break;

    case 12:
      nsurf = 1;
      pt[0][0] = hi[0];
      pt[0][1] = interpolate(v01,v11,lo[1],hi[1]);
      pt[1][0] = lo[0];
      pt[1][1] = interpolate(v00,v10,lo[1],hi[1]);
      break;

    case 13: 
      nsurf = 1;
      pt[0][0] = hi[0];
      pt[0][1] = interpolate(v01,v11,lo[1],hi[1]);
      pt[1][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[1][1] = lo[1];
      break;
    
    case 14:
      nsurf = 1;
      pt[0][0] = interpolate(v00,v01,lo[0],hi[0]);
      pt[0][1] = lo[1];
      pt[1][0] = lo[0];
      pt[1][1] = interpolate(v00,v10,lo[1],hi[1]);
      break;
    
    case 15:
      nsurf = 0;
      break;
    }

    // populate Grid and Surf data structs
    // points will be duplicated, not unique
    // surf ID = cell ID for all surfs in cell
    
    ptr = csurfs->get(nsurf);

    ipt = 0;
    for (i = 0; i < nsurf; i++) {
      if (svalues) surf->add_line(svalues[icell],pt[ipt],pt[ipt+1]);
      else surf->add_line(1,pt[ipt],pt[ipt+1]);
      ipt += 2;
      isurf = surf->nlocal - 1;
      surf->lines[isurf].id = cells[icell].id;
      ptr[i] = isurf;
    }

    cells[icell].nsurf = nsurf;
    if (nsurf) {
      cells[icell].csurfs = ptr;
      cinfo[icell].type = OVERLAP;
    }
  }
}

/* ----------------------------------------------------------------------
   create 3d implicit surfs from grid point values
------------------------------------------------------------------------- */

void ReadISurf::marching_cubes()
{
  int i,j,ipt,isurf,nsurf,icase,which;
  surfint *ptr;
    
  Grid::ChildCell *cells = grid->cells;
  Grid::ChildInfo *cinfo = grid->cinfo;
  MyPage<surfint> *csurfs = grid->csurfs;
  int nglocal = grid->nlocal;
  int groupbit = grid->bitmask[ggroup];
    
  for (int icell = 0; icell < nglocal; icell++) {
    if (!(cinfo[icell].mask & groupbit)) continue;
    lo = cells[icell].lo;
    hi = cells[icell].hi;
        
    // nsurf = # of tris in cell
    // cvalues[8] = 8 corner point values, each is 0 to 255 inclusive
    // thresh = value between 0 and 255 to threshhold on
    // lo[3] = lower left corner pt of grid cell
    // hi[3] = upper right corner pt of grid cell
    // pt = list of 3*nsurf points that are the corner pts of each tri
    
    // cvalues are ordered
    // bottom-lower-left, bottom-lower-right, 
    // bottom-upper-left, bottom-upper-right
    // top-lower-left, top-lower-right, top-upper-left, top-upper-right
    // Vzyx encodes this as 0/1 in each dim
        
    v000 = cvalues[icell][0];
    v001 = cvalues[icell][1];
    v010 = cvalues[icell][2];
    v011 = cvalues[icell][3];
    v100 = cvalues[icell][4];
    v101 = cvalues[icell][5];
    v110 = cvalues[icell][6];
    v111 = cvalues[icell][7];
     
    v000iso = v000 - thresh;
    v001iso = v001 - thresh;
    v010iso = v010 - thresh;
    v011iso = v011 - thresh;
    v100iso = v100 - thresh;
    v101iso = v101 - thresh;
    v110iso = v110 - thresh;
    v111iso = v111 - thresh;
        
    // make bits 2, 3, 6 and 7 consistent with Lewiner paper (see NOTE above)
        
    bit0 = v000 <= thresh ? 0 : 1;
    bit1 = v001 <= thresh ? 0 : 1;
    bit2 = v011 <= thresh ? 0 : 1;
    bit3 = v010 <= thresh ? 0 : 1;
    bit4 = v100 <= thresh ? 0 : 1;
    bit5 = v101 <= thresh ? 0 : 1;
    bit6 = v111 <= thresh ? 0 : 1;
    bit7 = v110 <= thresh ? 0 : 1;
    
    which = (bit7 << 7) + (bit6 << 6) + (bit5 << 5) + (bit4 << 4) + 
      (bit3 << 3) + (bit2 << 2) + (bit1 << 1) + bit0;
        
    // icase = case of the active cube in [0..15]

    icase = cases[which][0];
    config = cases[which][1];
    subconfig = 0;
    
    // DEBUG
    // printf("case %d and config %d lo %1.0f %1.0f %1.0f hi %1.0f %1.0f %1.0f cvalues %d %d %d %d %d %d %d %d\n",icase,config,lo[0],lo[1],lo[2],hi[0],hi[1],hi[2],cvalues[icell][0],cvalues[icell][1],cvalues[icell][2],cvalues[icell][3],cvalues[icell][4],cvalues[icell][5],cvalues[icell][6],cvalues[icell][7]);
    
    switch (icase) {
    case  0:
      nsurf = 0;
      break;
                
    case  1:
      nsurf = add_triangle(tiling1[config], 1);
      break;
                
    case  2:
      nsurf = add_triangle(tiling2[config], 2);
      break;
                
    case  3:
      if (test_face(test3[config]))
        nsurf = add_triangle(tiling3_2[config], 4); // 3.2
      else
        nsurf = add_triangle(tiling3_1[config], 2); // 3.1
      break;
                
    case  4:
      if (modified_test_interior(test4[config],icase))
        nsurf = add_triangle(tiling4_1[config], 2); // 4.1.1
      else
        nsurf = add_triangle(tiling4_2[config], 6); // 4.1.2
      break;
                
    case  5:
      nsurf = add_triangle(tiling5[config], 3);
      break;
                
    case  6:
      if (test_face(test6[config][0]))
        nsurf = add_triangle(tiling6_2[config], 5); // 6.2
      else {
        if (modified_test_interior(test6[config][1],icase))
          nsurf = add_triangle(tiling6_1_1[config], 3); // 6.1.1
        else {
          nsurf = add_triangle(tiling6_1_2[config], 9); // 6.1.2
        }
      }
      break;
                
    case  7:
      if (test_face(test7[config][0])) subconfig +=  1;
      if (test_face(test7[config][1])) subconfig +=  2;
      if (test_face(test7[config][2])) subconfig +=  4;
      switch (subconfig) {
      case 0:
        nsurf = add_triangle(tiling7_1[config], 3); break;
      case 1:
        nsurf = add_triangle(tiling7_2[config][0], 5); break;
      case 2:
        nsurf = add_triangle(tiling7_2[config][1], 5); break;
      case 3:
        nsurf = add_triangle(tiling7_3[config][0], 9); break;
      case 4:
        nsurf = add_triangle(tiling7_2[config][2], 5); break;
      case 5:
        nsurf = add_triangle(tiling7_3[config][1], 9); break;
      case 6:
        nsurf = add_triangle(tiling7_3[config][2], 9); break;
      case 7:
        if (test_interior(test7[config][3],icase))
          nsurf = add_triangle(tiling7_4_2[config], 9);
        else
          nsurf = add_triangle(tiling7_4_1[config], 5);
        break;
      };
      break;
                
    case  8:
      nsurf = add_triangle(tiling8[config], 2);
      break;
                
    case  9:
      nsurf = add_triangle(tiling9[config], 4);
      break;
                
    case 10:
      if (test_face(test10[config][0])) {
        if (test_face(test10[config][1]))
          nsurf = add_triangle(tiling10_1_1_[config], 4); // 10.1.1
        else {
          nsurf = add_triangle(tiling10_2[config], 8); // 10.2
        }
      } else {
        if (test_face(test10[config][1])) {
          nsurf = add_triangle(tiling10_2_[config], 8); // 10.2
        } else {
          if (test_interior(test10[config][2],icase))
            nsurf = add_triangle(tiling10_1_1[config], 4); // 10.1.1
          else
            nsurf = add_triangle(tiling10_1_2[config], 8); // 10.1.2
        }
      }
      break;
                
    case 11:
      nsurf = add_triangle(tiling11[config], 4);
      break;
                
    case 12:
      if (test_face(test12[config][0])) {
        if (test_face(test12[config][1]))
          nsurf = add_triangle(tiling12_1_1_[config], 4); // 12.1.1
        else {
          nsurf = add_triangle(tiling12_2[config], 8); // 12.2
        }
      } else {
        if (test_face(test12[config][1])) {
          nsurf = add_triangle(tiling12_2_[config], 8); // 12.2
        } else {
          if (test_interior(test12[config][2],icase))
            nsurf = add_triangle(tiling12_1_1[config], 4); // 12.1.1
          else
            nsurf = add_triangle(tiling12_1_2[config], 8); // 12.1.2
        }
      }
      break;
                
    case 13:
      if (test_face(test13[config][0])) subconfig +=  1;
      if (test_face(test13[config][1])) subconfig +=  2;
      if (test_face(test13[config][2])) subconfig +=  4;
      if (test_face(test13[config][3])) subconfig +=  8;
      if (test_face(test13[config][4])) subconfig += 16;
      if (test_face(test13[config][5])) subconfig += 32;
                               
      switch (subconfig13[subconfig]) {
      case 0:/* 13.1 */
        nsurf = add_triangle(tiling13_1[config], 4); break;

      case 1:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][0], 6); break;
      case 2:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][1], 6); break;
      case 3:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][2], 6); break;
      case 4:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][3], 6); break;
      case 5:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][4], 6); break;
      case 6:/* 13.2 */
        nsurf = add_triangle(tiling13_2[config][5], 6); break;

      case 7:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][0], 10); break;
      case 8:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][1], 10); break;
      case 9:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][2], 10); break;
      case 10:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][3], 10); break;
      case 11:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][4], 10); break;
      case 12:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][5], 10); break;
      case 13:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][6], 10); break;
      case 14:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][7], 10); break;
      case 15:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][8], 10); break;
      case 16:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][9], 10); break;
      case 17:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][10], 10); break;
      case 18:/* 13.3 */
        nsurf = add_triangle(tiling13_3[config][11], 10); break;
        
      case 19:/* 13.4 */
        nsurf = add_triangle(tiling13_4[config][0], 12); break;
      case 20:/* 13.4 */
        nsurf = add_triangle(tiling13_4[config][1], 12); break;
      case 21:/* 13.4 */
        nsurf = add_triangle(tiling13_4[config][2], 12); break;
      case 22:/* 13.4 */
        nsurf = add_triangle(tiling13_4[config][3], 12); break;

      case 23:/* 13.5 */
        subconfig = 0;
        if (interior_test_case13())
          nsurf = add_triangle(tiling13_5_1[config][0], 6);
        else
          nsurf = add_triangle(tiling13_5_2[config][0], 10);
        break;

      case 24:/* 13.5 */
        subconfig = 1;
        if (interior_test_case13())
          nsurf = add_triangle(tiling13_5_1[config][1], 6);
        else
          nsurf = add_triangle(tiling13_5_2[config][1], 10);
        break;

      case 25:/* 13.5 */
        subconfig = 2;
        if (interior_test_case13())
          nsurf = add_triangle(tiling13_5_1[config][2], 6);
        else
          nsurf = add_triangle(tiling13_5_2[config][2], 10);
        break;

      case 26:/* 13.5 */
        subconfig = 3;
        if (interior_test_case13())
          nsurf = add_triangle(tiling13_5_1[config][3], 6);
        else
          nsurf = add_triangle(tiling13_5_2[config][3], 10);
        break;

      case 27:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][0], 10); break;
      case 28:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][1], 10); break;
      case 29:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][2], 10); break;
      case 30:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][3], 10); break;
      case 31:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][4], 10); break;
      case 32:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][5], 10); break;
      case 33:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][6], 10); break;
      case 34:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][7], 10); break;
      case 35:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][8], 10); break;
      case 36:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][9], 10); break;
      case 37:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][10], 10); break;
      case 38:/* 13.3 */
        nsurf = add_triangle(tiling13_3_[config][11], 10); break;
        
      case 39:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][0], 6); break;
      case 40:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][1], 6); break;
      case 41:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][2], 6); break;
      case 42:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][3], 6); break;
      case 43:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][4], 6); break;
      case 44:/* 13.2 */
        nsurf = add_triangle(tiling13_2_[config][5], 6); break;
        
      case 45:/* 13.1 */
        nsurf = add_triangle(tiling13_1_[config], 4); break;
        
      default:
        print_cube();
        error->one(FLERR,"Marching cubes - impossible case 13");
      }
      break;
                
    case 14:
      nsurf = add_triangle(tiling14[config], 4);
      break;
    };
        
    // populate Grid and Surf data structs
    // points will be duplicated, not unique
    // surf ID = cell ID for all surfs in cell
        
    ptr = csurfs->get(nsurf);
        
    ipt = 0;
    for (i = 0; i < nsurf; i++) {
      if (svalues) surf->add_tri(svalues[icell],pt[ipt+2],pt[ipt+1],pt[ipt]);
      else surf->add_tri(1,pt[ipt+2],pt[ipt+1],pt[ipt]);
      ipt += 3;
      isurf = surf->nlocal - 1;
      surf->tris[isurf].id = cells[icell].id;
      ptr[i] = isurf;
    }
        
    cells[icell].nsurf = nsurf;
    if (nsurf) {
      cells[icell].csurfs = ptr;
      cinfo[icell].type = OVERLAP;
    }
  }
}

/* ----------------------------------------------------------------------
   adding triangles
------------------------------------------------------------------------- */

int ReadISurf::add_triangle(int *trig, int n)
{
  for(int t = 0; t < 3*n; t++) {
    switch (trig[t]) {
    case 0:
      pt[t][0] = interpolate(v000,v001,lo[0],hi[0]);
      pt[t][1] = lo[1];
      pt[t][2] = lo[2];
      break;
    case 1:
      pt[t][0] = hi[0];
      pt[t][1] = interpolate(v001,v011,lo[1],hi[1]);
      pt[t][2] = lo[2];
      break;
    case 2:
      pt[t][0] = interpolate(v010,v011,lo[0],hi[0]);
      pt[t][1] = hi[1];
      pt[t][2] = lo[2];
      break;
    case 3:
      pt[t][0] = lo[0];
      pt[t][1] = interpolate(v000,v010,lo[1],hi[1]);
      pt[t][2] = lo[2];
      break;
    case 4:
      pt[t][0] = interpolate(v100,v101,lo[0],hi[0]);
      pt[t][1] = lo[1];
      pt[t][2] = hi[2];
      break;
    case 5:
      pt[t][0] = hi[0];
      pt[t][1] = interpolate(v101,v111,lo[1],hi[1]);
      pt[t][2] = hi[2];
      break;
    case 6:
      pt[t][0] = interpolate(v110,v111,lo[0],hi[0]);
      pt[t][1] = hi[1];
      pt[t][2] = hi[2];
      break;
    case 7:
      pt[t][0] = lo[0];
      pt[t][1] = interpolate(v100,v110,lo[1],hi[1]);
      pt[t][2] = hi[2];
      break;
    case 8:
      pt[t][0] = lo[0];
      pt[t][1] = lo[1];
      pt[t][2] = interpolate(v000,v100,lo[2],hi[2]);
      break;
    case 9:
      pt[t][0] = hi[0];
      pt[t][1] = lo[1];
      pt[t][2] = interpolate(v001,v101,lo[2],hi[2]);
      break;
    case 10:
      pt[t][0] = hi[0];
      pt[t][1] = hi[1];
      pt[t][2] = interpolate(v011,v111,lo[2],hi[2]);
      break;
    case 11:
      pt[t][0] = lo[0];
      pt[t][1] = hi[1];
      pt[t][2] = interpolate(v010,v110,lo[2],hi[2]);
      break;
    case 12: {
      int u = 0;
      pt[t][0] = pt[t][1] = pt[t][2] = 0.0;
      if (bit0 ^ bit1) {
        ++u;
        pt[t][0] += interpolate(v000,v001,lo[0],hi[0]);
        pt[t][1] += lo[1];
        pt[t][2] += lo[2];
      }
      if (bit1 ^ bit2) {
        ++u;
        pt[t][0] += hi[0];
        pt[t][1] += interpolate(v001,v011,lo[1],hi[1]);
        pt[t][2] += lo[2];
      }
      if (bit2 ^ bit3) {
        ++u;
        pt[t][0] += interpolate(v010,v011,lo[0],hi[0]);
        pt[t][1] += hi[1];
        pt[t][2] += lo[2];
      }
      if (bit3 ^ bit0) {
        ++u;
        pt[t][0] += lo[0];
        pt[t][1] += interpolate(v000,v010,lo[1],hi[1]);
        pt[t][2] += lo[2];
      }
      if (bit4 ^ bit5) {
        ++u;
        pt[t][0] += interpolate(v100,v101,lo[0],hi[0]);
        pt[t][1] += lo[1];
        pt[t][2] += hi[2];
      }
      if (bit5 ^ bit6) {
        ++u;
        pt[t][0] += hi[0];
        pt[t][1] += interpolate(v101,v111,lo[1],hi[1]);
        pt[t][2] += hi[2];
      }
      if (bit6 ^ bit7) {
        ++u;
        pt[t][0] += interpolate(v110,v111,lo[0],hi[0]);
        pt[t][1] += hi[1];
        pt[t][2] += hi[2];
      }
      if (bit7 ^ bit4) {
        ++u;
        pt[t][0] += lo[0];
        pt[t][1] += interpolate(v100,v110,lo[1],hi[1]);
        pt[t][2] += hi[2];
      }
      if (bit0 ^ bit4) {
        ++u;
        pt[t][0] += lo[0];
        pt[t][1] += lo[1];
        pt[t][2] += interpolate(v000,v100,lo[2],hi[2]);
      }
      if (bit1 ^ bit5) {
        ++u;
        pt[t][0] += hi[0];
        pt[t][1] += lo[1];
        pt[t][2] += interpolate(v001,v101,lo[2],hi[2]);
      }
      if (bit2 ^ bit6) {
        ++u;
        pt[t][0] += hi[0];
        pt[t][1] += hi[1];
        pt[t][2] += interpolate(v011,v111,lo[2],hi[2]);
      }
      if (bit3 ^ bit7) {
        ++u;
        pt[t][0] += lo[0];
        pt[t][1] += hi[1];
        pt[t][2] += interpolate(v010,v110,lo[2],hi[2]);
      }

      pt[t][0] /= static_cast<double> (u);
      pt[t][1] /= static_cast<double> (u);
      pt[t][2] /= static_cast<double> (u);
      break;
    }

    default:
      break;
    }

    // DEBUG
    // printf("pt number %d n %d case %d and config %d lo %1.0f %1.0f %1.0f hi %1.0f %1.0f %1.0f cvalues %d %d %d %d %d %d %d %d coords %f %f %f\n" ,t,n,icase,config,lo[0],lo[1],lo[2],hi[0],hi[1],hi[2],v000,v001,v011,v010,v100,v101,v111,v110,pt[t][0],pt[t][1],pt[t][2]);

  }

  return n;
}

/* ----------------------------------------------------------------------
   test a face
   if face > 0 return true if the face contains a part of the surface
------------------------------------------------------------------------- */

bool ReadISurf::test_face(int face)
{
  double A,B,C,D;
    
  switch (face) {
  case -1:
  case 1:
    A = v000iso;
    B = v100iso;
    C = v101iso;
    D = v001iso;
    break;
  case -2:
  case 2:
    A = v001iso;
    B = v101iso;
    C = v111iso;
    D = v011iso;
    break;
  case -3:
  case 3:
    A = v011iso;
    B = v111iso;
    C = v110iso;
    D = v010iso;
    break;
  case -4:
  case 4:
    A = v010iso;
    B = v110iso;
    C = v100iso;
    D = v000iso;
    break;
  case -5:
  case 5:
    A = v000iso;
    B = v010iso;
    C = v011iso;
    D = v001iso;
    break;
  case -6:
  case 6:
    A = v100iso;
    B = v110iso;
    C = v111iso;
    D = v101iso;
    break;

  default: 
    A = B = C = D = 0.0;
    print_cube();
    error->one(FLERR,"Invalid face code");
  };
    
  if (fabs(A*C - B*D) < EPSILON) return face >= 0;
  return face * A * (A*C - B*D) >= 0 ;  // face and A invert signs
}

/* ----------------------------------------------------------------------
   test the interior of a cube
   icase = case of the active cube in [0..15]
   if s ==  7, return true if the interior is empty
   if s == -7, return false if the interior is empty
------------------------------------------------------------------------- */

bool ReadISurf::test_interior(int s, int icase)
{
  double t,a,b,At=0.0,Bt=0.0,Ct=0.0,Dt=0.0;
  int test = 0;
  int edge = -1;   // reference edge of the triangulation
    
  switch (icase) {
  case  4 :
  case 10 :
    a = ( v100iso - v000iso ) * ( v111iso - v011iso ) - 
      ( v110iso - v010iso ) * ( v101iso - v001iso ) ;
    b =  v011iso * ( v100iso - v000iso ) + v000iso * ( v111iso - v011iso ) - 
      v001iso * ( v110iso - v010iso ) - v010iso * ( v101iso - v001iso ) ;
    t = - b / (2*a) ;
    if (t < 0 || t > 1) return s>0 ;

    At = v000iso + ( v100iso - v000iso ) * t ;
    Bt = v010iso + ( v110iso - v010iso ) * t ;
    Ct = v011iso + ( v111iso - v011iso ) * t ;
    Dt = v001iso + ( v101iso - v001iso ) * t ;
    break ;

  case  6 :
  case  7 :
  case 12 :
  case 13 :
    switch( icase ) {
    case  6 : edge = test6 [config][2] ; break ;
    case  7 : edge = test7 [config][4] ; break ;
    case 12 : edge = test12[config][3] ; break ;
    case 13 : edge = tiling13_5_1[config][subconfig][0] ; break ;
    }
    switch( edge ) {
    case  0 :
      t  = v000iso / ( v000iso - v001iso ) ;
      At = 0.0 ;
      Bt = v010iso + ( v011iso - v010iso ) * t ;
      Ct = v110iso + ( v111iso - v110iso ) * t ;
      Dt = v100iso + ( v101iso - v100iso ) * t ;
      break ;
    case  1 :
      t  = v001iso / ( v001iso - v011iso ) ;
      At = 0.0 ;
      Bt = v000iso + ( v010iso - v000iso ) * t ;
      Ct = v100iso + ( v110iso - v100iso ) * t ;
      Dt = v101iso + ( v111iso - v101iso ) * t ;
      break ;
    case  2 :
      t  = v011iso / ( v011iso - v010iso ) ;
      At = 0.0 ;
      Bt = v001iso + ( v000iso - v001iso ) * t ;
      Ct = v101iso + ( v100iso - v101iso ) * t ;
      Dt = v111iso + ( v110iso - v111iso ) * t ;
      break ;
    case  3 :
      t  = v010iso / ( v010iso - v000iso ) ;
      At = 0.0 ;
      Bt = v011iso + ( v001iso - v011iso ) * t ;
      Ct = v111iso + ( v101iso - v111iso ) * t ;
      Dt = v110iso + ( v100iso - v110iso ) * t ;
      break ;
    case  4 :
      t  = v100iso / ( v100iso - v101iso ) ;
      At = 0.0 ;
      Bt = v110iso + ( v111iso - v110iso ) * t ;
      Ct = v010iso + ( v011iso - v010iso ) * t ;
      Dt = v000iso + ( v001iso - v000iso ) * t ;
      break ;
    case  5 :
      t  = v101iso / ( v101iso - v111iso ) ;
      At = 0.0 ;
      Bt = v100iso + ( v110iso - v100iso ) * t ;
      Ct = v000iso + ( v010iso - v000iso ) * t ;
      Dt = v001iso + ( v011iso - v001iso ) * t ;
      break ;
    case  6 :
      t  = v111iso / ( v111iso - v110iso ) ;
      At = 0.0 ;
      Bt = v101iso + ( v100iso - v101iso ) * t ;
      Ct = v001iso + ( v000iso - v001iso ) * t ;
      Dt = v011iso + ( v010iso - v011iso ) * t ;
      break ;
    case  7 :
      t  = v110iso / ( v110iso - v100iso ) ;
      At = 0.0 ;
      Bt = v111iso + ( v101iso - v111iso ) * t ;
      Ct = v011iso + ( v001iso - v011iso ) * t ;
      Dt = v010iso + ( v000iso - v010iso ) * t ;
      break ;
    case  8 :
      t  = v000iso / ( v000iso - v100iso ) ;
      At = 0.0 ;
      Bt = v010iso + ( v110iso - v010iso ) * t ;
      Ct = v011iso + ( v111iso - v011iso ) * t ;
      Dt = v001iso + ( v101iso - v001iso ) * t ;
      break ;
    case  9 :
      t  = v001iso / ( v001iso - v101iso ) ;
      At = 0.0 ;
      Bt = v000iso + ( v100iso - v000iso ) * t ;
      Ct = v010iso + ( v110iso - v010iso ) * t ;
      Dt = v011iso + ( v111iso - v011iso ) * t ;
      break ;
    case 10 :
      t  = v011iso / ( v011iso - v111iso ) ;
      At = 0.0 ;
      Bt = v001iso + ( v101iso - v001iso ) * t ;
      Ct = v000iso + ( v100iso - v000iso ) * t ;
      Dt = v010iso + ( v110iso - v010iso ) * t ;
      break ;
    case 11 :
      t  = v010iso / ( v010iso - v110iso ) ;
      At = 0.0 ;
      Bt = v011iso + ( v111iso - v011iso ) * t ;
      Ct = v001iso + ( v101iso - v001iso ) * t ;
      Dt = v000iso + ( v100iso - v000iso ) * t ;
      break ;

    default: 
      print_cube();
      error->one(FLERR,"Marching cubes - invalid edge");
    }
    break;
      
  default: 
    print_cube();
    error->one(FLERR,"Marching cubes - invalid ambiguous case");
  }
    
  if (At >= 0.0) test ++;
  if (Bt >= 0.0) test += 2;
  if (Ct >= 0.0) test += 4;
  if (Dt >= 0.0) test += 8;
  switch (test) {
  case  0: return s>0;
  case  1: return s>0;
  case  2: return s>0;
  case  3: return s>0;
  case  4: return s>0;
  case  5: 
    if (At * Ct - Bt * Dt <  EPSILON) return s>0;
    break;
  case  6: return s>0;
  case  7: return s<0;
  case  8: return s>0;
  case  9: return s>0;
  case 10: 
    if (At * Ct - Bt * Dt >= EPSILON) return s>0;
    break;
  case 11: return s<0;
  case 12: return s>0;
  case 13: return s<0;
  case 14: return s<0;
  case 15: return s<0;
  }
  
  return s<0;
}

/* ---------------------------------------------------------------------- */

bool ReadISurf::modified_test_interior(int s, int icase)
{
  int edge = -1;
  int amb_face;

  int inter_amb = 0;

  switch (icase) {
  case 4:
    amb_face = 1;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    amb_face = 2;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    amb_face = 5;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    if (inter_amb == 0) return false;
    else                return true;
    break;

  case 6:
    amb_face = abs(test6[config][0]);

    edge = interior_ambiguity(amb_face, s);
    inter_amb = interior_ambiguity_verification(edge);

    if (inter_amb == 0) return false;
    else		return true;

    break;

  case 7:
    s = s * -1;

    amb_face = 1;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    amb_face = 2;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    amb_face = 5;
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    if (inter_amb == 0) return false;
    else                return true;
    break;

  case 10:
    amb_face = abs(test10[config][0]);

    edge = interior_ambiguity(amb_face, s);
    inter_amb = interior_ambiguity_verification(edge);

    if (inter_amb == 0) return false;
    else                return true;
    break;

  case 12:
    amb_face = abs(test12[config][0]);
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);


    amb_face = abs(test12[config][1]);
    edge = interior_ambiguity(amb_face, s);
    inter_amb += interior_ambiguity_verification(edge);

    if (inter_amb == 0) return false;
    else                return true;
    break;
  }

  // should never reach here

  return true;
}

/* ---------------------------------------------------------------------- */

int ReadISurf::interior_ambiguity(int amb_face, int s)
{
  int edge;

  switch (amb_face) {
  case 1:
  case 3:
    if (((v001iso * s) > 0) && ((v110iso * s) > 0)) edge = 4;
    if (((v000iso * s) > 0) && ((v111iso * s) > 0)) edge = 5;
    if (((v010iso * s) > 0) && ((v101iso * s) > 0)) edge = 6;
    if (((v011iso * s) > 0) && ((v100iso * s) > 0)) edge = 7;
    break;

  case 2:
  case 4:
    if (((v001iso * s) > 0) && ((v110iso * s) > 0)) edge = 0;
    if (((v011iso * s) > 0) && ((v100iso * s) > 0)) edge = 1;
    if (((v010iso * s) > 0) && ((v101iso * s) > 0)) edge = 2;
    if (((v000iso * s) > 0) && ((v111iso * s) > 0)) edge = 3;
    break;

  case 5:
  case 6:
  case 0:
    if (((v000iso * s) > 0) && ((v111iso * s) > 0)) edge = 8;
    if (((v001iso * s) > 0) && ((v110iso * s) > 0)) edge = 9;
    if (((v011iso * s) > 0) && ((v100iso * s) > 0)) edge = 10;
    if (((v010iso * s) > 0) && ((v101iso * s) > 0)) edge = 11;
    break;
  }

  return edge;
}

/* ---------------------------------------------------------------------- */

int ReadISurf::interior_ambiguity_verification(int edge)
{
  double t, At = 0.0, Bt = 0.0, Ct = 0.0, Dt = 0.0, a = 0.0, b = 0.0;
  double verify;

  switch (edge) {

  case 0:
    a = (v000iso - v001iso) * (v110iso - v111iso)
      - (v100iso - v101iso) * (v010iso - v011iso);
    b = v111iso * (v000iso - v001iso) + v001iso * (v110iso - v111iso)
      - v011iso * (v100iso - v101iso)
      - v101iso * (v010iso - v011iso);

    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v001iso + (v000iso - v001iso) * t;
    Bt = v101iso + (v100iso - v101iso) * t;
    Ct = v111iso + (v110iso - v111iso) * t;
    Dt = v011iso + (v010iso - v011iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;

    break;

  case 1:
    a = (v010iso - v011iso) * (v100iso - v101iso)
      - (v000iso - v001iso) * (v110iso - v111iso);
    b = v101iso * (v010iso - v011iso) + v011iso * (v100iso - v101iso)
      - v111iso * (v000iso - v001iso)
      - v001iso * (v110iso - v111iso);

    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v011iso + (v010iso - v011iso) * t;
    Bt = v001iso + (v000iso - v001iso) * t;
    Ct = v101iso + (v100iso - v101iso) * t;
    Dt = v111iso + (v110iso - v111iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 2:
    a = (v011iso - v010iso) * (v101iso - v100iso)
      - (v111iso - v110iso) * (v001iso - v000iso);
    b = v100iso * (v011iso - v010iso) + v010iso * (v101iso - v100iso)
      - v000iso * (v111iso - v110iso)
      - v110iso * (v001iso - v000iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v010iso + (v011iso - v010iso) * t;
    Bt = v110iso + (v111iso - v110iso) * t;
    Ct = v100iso + (v101iso - v100iso) * t;
    Dt = v000iso + (v001iso - v000iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 3:
    a = (v001iso - v000iso) * (v111iso - v110iso)
      - (v011iso - v010iso) * (v101iso - v100iso);
    b = v110iso * (v001iso - v000iso) + v000iso * (v111iso - v110iso)
      - v100iso * (v011iso - v010iso)
      - v010iso * (v101iso - v100iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v000iso + (v001iso - v000iso) * t;
    Bt = v010iso + (v011iso - v010iso) * t;
    Ct = v110iso + (v111iso - v110iso) * t;
    Dt = v100iso + (v101iso - v100iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 4:

    a = (v011iso - v001iso) * (v110iso - v100iso)
      - (v010iso - v000iso) * (v111iso - v101iso);
    b = v100iso * (v011iso - v001iso) + v001iso * (v110iso - v100iso)
      - v101iso * (v010iso - v000iso)
      - v000iso * (v111iso - v101iso);

    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v001iso + (v011iso - v001iso) * t;
    Bt = v000iso + (v010iso - v000iso) * t;
    Ct = v100iso + (v110iso - v100iso) * t;
    Dt = v101iso + (v111iso - v101iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 5:

    a = (v010iso - v000iso) * (v111iso - v101iso)
      - (v011iso - v001iso) * (v110iso - v100iso);
    b = v101iso * (v010iso - v000iso) + v000iso * (v111iso - v101iso)
      - v100iso * (v011iso - v001iso)
      - v001iso * (v110iso - v100iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v000iso + (v010iso - v000iso) * t;
    Bt = v001iso + (v011iso - v001iso) * t;
    Ct = v101iso + (v111iso - v101iso) * t;
    Dt = v100iso + (v110iso - v100iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 6:
    a = (v000iso - v010iso) * (v101iso - v111iso)
      - (v100iso - v110iso) * (v001iso - v011iso);
    b = v111iso * (v000iso - v010iso) + v010iso * (v101iso - v111iso)
      - v011iso * (v100iso - v110iso)
      - v110iso * (v001iso - v011iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v010iso + (v000iso - v010iso) * t;
    Bt = v110iso + (v100iso - v110iso) * t;
    Ct = v111iso + (v101iso - v111iso) * t;
    Dt = v011iso + (v001iso - v011iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 7:
    a = (v001iso - v011iso) * (v100iso - v110iso)
      - (v000iso - v010iso) * (v101iso - v111iso);
    b = v110iso * (v001iso - v011iso) + v011iso * (v100iso - v110iso)
      - v111iso * (v000iso - v010iso)
      - v010iso * (v101iso - v111iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v011iso + (v001iso - v011iso) * t;
    Bt = v010iso + (v000iso - v010iso) * t;
    Ct = v110iso + (v100iso - v110iso) * t;
    Dt = v111iso + (v101iso - v111iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 8:
    a = (v100iso - v000iso) * (v111iso - v011iso)
      - (v110iso - v010iso) * (v101iso - v001iso);
    b = v011iso * (v100iso - v000iso) + v000iso * (v111iso - v011iso)
      - v001iso * (v110iso - v010iso)
      - v010iso * (v101iso - v001iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v000iso + (v100iso - v000iso) * t;
    Bt = v010iso + (v110iso - v010iso) * t;
    Ct = v011iso + (v111iso - v011iso) * t;
    Dt = v001iso + (v101iso - v001iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 9:
    a = (v101iso - v001iso) * (v110iso - v010iso)
      - (v100iso - v000iso) * (v111iso - v011iso);
    b = v010iso * (v101iso - v001iso) + v001iso * (v110iso - v010iso)
      - v011iso * (v100iso - v000iso)
      - v000iso * (v111iso - v011iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v001iso + (v101iso - v001iso) * t;
    Bt = v000iso + (v100iso - v000iso) * t;
    Ct = v010iso + (v110iso - v010iso) * t;
    Dt = v011iso + (v111iso - v011iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 10:
    a = (v111iso - v011iso) * (v100iso - v000iso)
      - (v101iso - v001iso) * (v110iso - v010iso);
    b = v000iso * (v111iso - v011iso) + v011iso * (v100iso - v000iso)
      - v010iso * (v101iso - v001iso)
      - v001iso * (v110iso - v010iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v011iso + (v111iso - v011iso) * t;
    Bt = v001iso + (v101iso - v001iso) * t;
    Ct = v000iso + (v100iso - v000iso) * t;
    Dt = v010iso + (v110iso - v010iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;

  case 11:
    a = (v110iso - v010iso) * (v101iso - v001iso)
      - (v111iso - v011iso) * (v100iso - v000iso);
    b = v001iso * (v110iso - v010iso) + v010iso * (v101iso - v001iso)
      - v000iso * (v111iso - v011iso)
      - v011iso * (v100iso - v000iso);
    if (a > 0)
      return 1;

    t = -b / (2 * a);
    if (t < 0 || t > 1)
      return 1;

    At = v010iso + (v110iso - v010iso) * t;
    Bt = v011iso + (v111iso - v011iso) * t;
    Ct = v001iso + (v101iso - v001iso) * t;
    Dt = v000iso + (v100iso - v000iso) * t;

    verify = At * Ct - Bt * Dt;

    if (verify > 0)
      return 0;
    if (verify < 0)
      return 1;
    break;
  }

  // should never reach here

  return 1;
}

/* ----------------------------------------------------------------------
   return true if the interior is empty (two faces)
------------------------------------------------------------------------- */

bool ReadISurf::interior_test_case13()
{
  double t1, t2, At1 = 0.0, Bt1 = 0.0, Ct1 = 0.0, Dt1 = 0.0;
  double At2 = 0.0, Bt2 = 0.0, Ct2 = 0.0, Dt2 = 0.0, a = 0.0, b = 0.0, c = 0.0;

  a = (v000iso - v001iso) * (v110iso - v111iso)
    - (v100iso - v101iso) * (v010iso - v011iso);
  b = v111iso * (v000iso - v001iso) + v001iso * (v110iso - v111iso)
    - v011iso * (v100iso - v101iso)
    - v101iso * (v010iso - v011iso);
  c = v001iso*v111iso - v101iso*v011iso;

  double delta = b*b - 4*a*c;

  t1 = (-b + sqrt(delta))/(2*a);
  t2 = (-b - sqrt(delta))/(2*a);

  // DEBUG
  // printf("delta = %f, t1 = %f, t2 = %f\n", delta, t1, t2);

  if ((t1 < 1)&&(t1>0) &&(t2 < 1)&&(t2 > 0)) {
    At1 = v001iso + (v000iso - v001iso) * t1;
    Bt1 = v101iso + (v100iso - v101iso) * t1;
    Ct1 = v111iso + (v110iso - v111iso) * t1;
    Dt1 = v011iso + (v010iso - v011iso) * t1;

    double x1 = (At1 - Dt1)/(At1 + Ct1 - Bt1 - Dt1);
    double y1 = (At1 - Bt1)/(At1 + Ct1 - Bt1 - Dt1);

    At2 = v001iso + (v000iso - v001iso) * t2;
    Bt2 = v101iso + (v100iso - v101iso) * t2;
    Ct2 = v111iso + (v110iso - v111iso) * t2;
    Dt2 = v011iso + (v010iso - v011iso) * t2;
    
    double x2 = (At2 - Dt2)/(At2 + Ct2 - Bt2 - Dt2);
    double y2 = (At2 - Bt2)/(At2 + Ct2 - Bt2 - Dt2);

    if ((x1 < 1)&&(x1>0) &&(x2 < 1)&&(x2 > 0) && 
        (y1 < 1)&&(y1>0) &&(y2 < 1)&&(y2 > 0)) return false;
  }

  return true;
}

/* ----------------------------------------------------------------------
   clean up issues that marching cubes (MC) occasionally generates
     that cause problems for SPARTA
   what MC does:
     may generate 0 or 2 triangles on the face of a cell
     the cell sharing the face may also generate 0 or 2 triangles
     the normals for the 2 triangles may be into or out of the cell
   what SPARTA needs:
     let cell1 and cell2 be two cells that share a face
     if cell1 has 2 tris on face and cell2 has none:
       if norm is into cell1: keep them in cell1
       if norm is into cell2: assign both tris to cell2
     if both cell1 and cell2 have 2 tris on face: delete all 4 tris
   algorithm to do this:
     loop over all my cells with implicit tris:
       tally how many surfs on each face
     loop over all my cells with implicit tris:
       loop over faces with 2 tris:
         if I own adjoining cell:
           check its tally on shared face
           reassign and/or delete triangles as necessary
         if I do not own adjoining cell:
           add 2 tris to send list for this proc
     irregular comm of send list to nearby procs (share faces of my cells)
     each proc loops over its recv list:
       if my cell face has 2 tris: delete them
       if my cell face has 0 tris: skip or add 2 tris depending on norm
 ------------------------------------------------------------------------- */

void ReadISurf::cleanup_MC()
{
  int i,j,k,m,icell,iface,nsurf,idim,nflag,inwardnorm;
  int ntri_other,othercell,otherface,otherproc,otherlocal,othernsurf;
  cellint *oldcsurfs;
  surfint *ptr,*csurfs_other;
  cellint cellID;
  double *lo,*hi;
  double *norm;

  Surf::Tri *tris = surf->tris;
  Grid::ChildCell *cells = grid->cells;
  MyPage<surfint> *csurfs = grid->csurfs;
  int nglocal = grid->nlocal;

  Surf::Tri *tlist = NULL;
  int nlist = 0;
  int maxlist = 0;

  // DEBUG

  //int nstotal;
  //MPI_Allreduce(&surf->nlocal,&nstotal,1,MPI_INT,MPI_SUM,world);
  //if (me == 0) printf("TOTAL TRI before count: %d\n",nstotal);

  // END of DEBUG

  // count # of tris on each face of every cell I own

  int **nfacetri;
  int ***facetris;
  memory->create(nfacetri,nglocal,6,"readisurf:nfacetri");
  memory->create(facetris,nglocal,6,2,"readisurf:facetris");

  for (icell = 0; icell < nglocal; icell++) {
    nfacetri[icell][0] = nfacetri[icell][1] = nfacetri[icell][2] = 
      nfacetri[icell][3] = nfacetri[icell][4] = nfacetri[icell][5] = 0;

    if (cells[icell].nsplit <= 0) continue;
    nsurf = cells[icell].nsurf;
    if (nsurf == 0) continue;

    lo = cells[icell].lo;
    hi = cells[icell].hi;

    for (j = 0; j < nsurf; j++) {
      m = cells[icell].csurfs[j];
      iface = Geometry::tri_on_hex_face(tris[m].p1,tris[m].p2,tris[m].p3,lo,hi);
      if (iface < 0) continue;
      if (nfacetri[icell][iface] < 2)
        facetris[icell][iface][nfacetri[icell][iface]] = m;
      nfacetri[icell][iface]++;
    }
  }

  // check that every face has 0 or 2 tris

  int flag = 0;
  for (icell = 0; icell < nglocal; icell++)
    for (iface = 0; iface < 6; iface++)
      if (nfacetri[icell][iface] != 0 && nfacetri[icell][iface] != 2)
        flag++;

  int flagall;
  MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,world);
  if (flagall) 
    error->all(FLERR,"Some cell faces do not have zero or 2 triangles");

  // loop over all cell faces
  // check tri count for that face for both adjoining cells

  int *proclist = NULL;
  SendDatum *bufsend = NULL;
  int nsend = 0;
  int maxsend = 0;

  int *dellist = NULL;
  int ndelete = 0;
  int maxdelete = 0;

  // DEBUG
  //int ntotal = 0;
  //int nadd = 0;
  //int ndel = 0;

  for (icell = 0; icell < nglocal; icell++) {
    if (cells[icell].nsplit <= 0) continue;
    nsurf = cells[icell].nsurf;
    if (nsurf == 0) continue;

    for (iface = 0; iface < 6; iface++) {
      if (nfacetri[icell][iface] != 2) continue;
      //ntotal += 2;

      // other cell/face/proc = info for matching face in adjacent cell

      nflag = grid->neigh_decode(cells[icell].nmask,iface);
      if (nflag != NCHILD && nflag != NPBCHILD) 
        error->one(FLERR,"Invalid neighbor cell in cleanup_MC()");
      othercell = (int) cells[icell].neigh[iface];
      if (othercell == icell)
        error->one(FLERR,"Invalid neighbor cell in cleanup_MC()");

      norm = tris[facetris[icell][iface][0]].norm;
      idim = iface/2;
      if (iface % 2 && norm[idim] < 0.0) inwardnorm = 1;
      else if (iface % 2 == 0 && norm[idim] > 0.0) inwardnorm = 1;
      else inwardnorm = 0;
      if (iface % 2) otherface = iface-1;
      else otherface = iface+1;
      otherproc = cells[othercell].proc;
      otherlocal = cells[othercell].ilocal;

      // if I own the adjacent cell, make decision about shared tris
      // if both cells have 2 tris on face, delete all of them
      // otherwise cell that matches inward normal is assigned the 2 tris

      if (otherproc == me) {
        ntri_other = nfacetri[othercell][otherface];

        // icell keeps the 2 tris

        if (ntri_other == 0 && inwardnorm) continue;

        // add 2 tris to othercell
        // reset tri IDs to new owning cell

        if (ntri_other == 0) { 
          othernsurf = cells[othercell].nsurf;
          oldcsurfs = cells[othercell].csurfs;
          ptr = csurfs->get(othernsurf+2);
          for (k = 0; k < othernsurf; k++)
            ptr[k] = oldcsurfs[k];
          ptr[othernsurf] = facetris[icell][iface][0];
          ptr[othernsurf+1] = facetris[icell][iface][1];
          cells[othercell].nsurf += 2;
          cells[othercell].csurfs = ptr;
          tris[facetris[icell][iface][0]].id = cells[othercell].id;
          tris[facetris[icell][iface][1]].id = cells[othercell].id;
          //nadd += 2;
        }

        // delete 2 tris from othercell
        // set nfacetri[othercell] = 0, so won't delete again when it is icell

        if (ntri_other == 2) {
          nfacetri[othercell][otherface] = 0;
          othernsurf = cells[othercell].nsurf;
          ptr = cells[othercell].csurfs;
          m = facetris[othercell][otherface][0];
          for (k = 0; k < othernsurf; k++)
            if (ptr[k] == m) break;
          if (k == othernsurf) 
            error->one(FLERR,"Could not find surf in cleanup_MC");
          cells[othercell].csurfs[k] = cells[othercell].csurfs[othernsurf-1];
          othernsurf--;
          m = facetris[othercell][otherface][1];
          for (k = 0; k < othernsurf; k++)
            if (ptr[k] == m) break;
          if (k == othernsurf) 
            error->one(FLERR,"Could not find surf in cleanup_MC");
          cells[othercell].csurfs[k] = cells[othercell].csurfs[othernsurf-1];
          othernsurf--;
          cells[othercell].nsurf -= 2;
          //ndel += 2;
        }

        // delete 2 tris from icell

        ptr = cells[icell].csurfs;
        m = facetris[icell][iface][0];
        for (k = 0; k < nsurf; k++)
          if (ptr[k] == m) break;
        if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
        cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
        nsurf--;
        m = facetris[icell][iface][1];
        for (k = 0; k < nsurf; k++)
          if (ptr[k] == m) break;
        if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
        cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
        nsurf--;
        cells[icell].nsurf -= 2;
        //ndel += 2;

        // add 4 tris to delete list if both cells deleted them

        if (ntri_other == 2) {
          if (ndelete+4 > maxdelete) {
            maxdelete += DELTA;
            memory->grow(dellist,maxdelete,"readisurf:dellist");
          }
          dellist[ndelete++] = facetris[icell][iface][0];
          dellist[ndelete++] = facetris[icell][iface][1];
          dellist[ndelete++] = facetris[othercell][otherface][0];
          dellist[ndelete++] = facetris[othercell][otherface][1];
        }

      // cell face is shared with another proc
      // send it the cell/face indices and the 2 tris,
      //   in case they need to be assigned to the other cell based on norm

      } else {
        if (nsend == maxsend) {
          maxsend += DELTA;
          proclist = (int *) 
            memory->srealloc(proclist,maxsend*sizeof(int),
                             "readisurf:proclist");
          bufsend = (SendDatum *) 
            memory->srealloc(bufsend,maxsend*sizeof(SendDatum),
                             "readisurf:bufsend");
        }
        proclist[nsend] = otherproc;
        bufsend[nsend].sendcell = icell;
        bufsend[nsend].sendface = iface;
        bufsend[nsend].othercell = otherlocal;
        bufsend[nsend].otherface = otherface;
        bufsend[nsend].inwardnorm = inwardnorm;
        memcpy(&bufsend[nsend].tri1,&tris[facetris[icell][iface][0]],
               sizeof(Surf::Tri));
        memcpy(&bufsend[nsend].tri2,&tris[facetris[icell][iface][1]],
               sizeof(Surf::Tri));
        nsend++;

        // if not inwardnorm, delete 2 tris from this cell
        // also add them to delete list

        if (!inwardnorm) {
          ptr = cells[icell].csurfs;
          m = facetris[icell][iface][0];
          for (k = 0; k < nsurf; k++)
            if (ptr[k] == m) break;
          if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
          cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
          nsurf--;
          m = facetris[icell][iface][1];
          for (k = 0; k < nsurf; k++)
            if (ptr[k] == m) break;
          if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
          cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
          nsurf--;
          cells[icell].nsurf -= 2;
          //ndel += 2;

          if (ndelete+2 > maxdelete) {
            maxdelete += DELTA;
            memory->grow(dellist,maxdelete,"readisurf:dellist");
          }
          dellist[ndelete++] = facetris[icell][iface][0];
          dellist[ndelete++] = facetris[icell][iface][1];
        }
      }
    }
  }

  // perform irregular communication of list of cell faces and tri pairs

  Irregular *irregular = new Irregular(sparta);
  int nrecv = irregular->create_data_uniform(nsend,proclist,1);

  SendDatum *bufrecv = (SendDatum *) 
    memory->smalloc(nrecv*sizeof(SendDatum),"readisurf:bufrecv");

  irregular->exchange_uniform((char *) bufsend,sizeof(SendDatum),
                              (char *) bufrecv);
  delete irregular;
  memory->sfree(proclist);
  memory->sfree(bufsend);

  // loop over list of received face/tri info
  // if my matching face has 2 tris, delete them
  // if my matching face has 0 tris, skip or add 2 tris depending on norm

  for (i = 0; i < nrecv; i++) {
    icell = bufrecv[i].othercell;
    iface = bufrecv[i].otherface;

    // my icell is not affected, sender cell keeps its 2 tris

    if (nfacetri[icell][iface] == 0 && bufrecv[i].inwardnorm) continue;

    // add 2 tris to icell and this processor's Surf::tris list
    // reset tri IDs to new owning cell

    if (nfacetri[icell][iface] == 0) {
      int nslocal = surf->nlocal;
      surf->add_tri(1,bufrecv[i].tri1.p1,bufrecv[i].tri1.p2,bufrecv[i].tri1.p3);  
      memcpy(&surf->tris[nslocal],&bufrecv[i].tri1,sizeof(Surf::Tri));
      surf->tris[nslocal].id = cells[icell].id;
      surf->add_tri(1,bufrecv[i].tri2.p1,bufrecv[i].tri2.p2,bufrecv[i].tri2.p3);
      memcpy(&surf->tris[nslocal+1],&bufrecv[i].tri2,sizeof(Surf::Tri));
      surf->tris[nslocal+1].id = cells[icell].id;
      
      nsurf = cells[icell].nsurf;
      oldcsurfs = cells[icell].csurfs;
      ptr = csurfs->get(nsurf+2);
      for (k = 0; k < nsurf; k++)
        ptr[k] = oldcsurfs[k];
      ptr[nsurf] = nslocal;
      ptr[nsurf+1] = nslocal+1;
      cells[icell].nsurf += 2;
      cells[icell].csurfs = ptr;
      //nadd += 2;
    }

    // both cells have 2 tris on common face
    // need to delete my 2 tris from icell
    // sender will get similar message from me and delete
    // inwardnorm check to see if I already deleted when sent a message,
    // else delete now and add 2 tris to delete list

    if (nfacetri[icell][iface] == 2) {
      norm = tris[facetris[icell][iface][0]].norm;
      idim = iface/2;
      if (iface % 2 && norm[idim] < 0.0) inwardnorm = 1;
      else if (iface % 2 == 0 && norm[idim] > 0.0) inwardnorm = 1;
      else inwardnorm = 0;
      if (!inwardnorm) continue;

      nsurf = cells[icell].nsurf;
      ptr = cells[icell].csurfs;
      m = facetris[icell][iface][0];
      for (k = 0; k < nsurf; k++)
        if (ptr[k] == m) break;
      if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
      cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
      nsurf--;
      m = facetris[icell][iface][1];
      for (k = 0; k < nsurf; k++)
        if (ptr[k] == m) break;
      if (k == nsurf) error->one(FLERR,"Could not find surf in cleanup_MC");
      cells[icell].csurfs[k] = cells[icell].csurfs[nsurf-1];
      nsurf--;
      cells[icell].nsurf -= 2;
      //ndel += 2;

      if (ndelete+2 > maxdelete) {
        maxdelete += DELTA;
        memory->grow(dellist,maxdelete,"readisurf:dellist");
      }
      dellist[ndelete++] = facetris[icell][iface][0];
      dellist[ndelete++] = facetris[icell][iface][1];
    }
  }    

  memory->sfree(bufrecv);
  memory->destroy(nfacetri);
  memory->destroy(facetris);

  // compress Surf::tris list to remove deleted tris
  // must sort dellist, so as to compress tris in DESCENDING index order
  // descending, not ascending, so that a surf is not moved from end-of-list
  //   that is flagged for later deletion
  // must repoint one location in cells->csurfs to moved surf

  qsort(dellist,ndelete,sizeof(int),compare_indices);

  tris = surf->tris;
  int nslocal = surf->nlocal;
  cellint celllID;
  for (i = 0; i < ndelete; i++) {
    m = dellist[i];
    memcpy(&tris[m],&tris[nslocal-1],sizeof(Surf::Tri));
    nslocal--;

    icell = (*grid->hash)[tris[m].id] - 1;
    nsurf = cells[icell].nsurf;
    ptr = cells[icell].csurfs;
    for (k = 0; k < nsurf; k++)
      if (ptr[k] == nslocal) {
        ptr[k] = m;
        break;
      }
    if (k == nsurf) error->one(FLERR,"Did not find moved tri in cleanup_MC()");
  }

  surf->nlocal = nslocal;
  memory->destroy(dellist);

  // DEBUG

  /*
  MPI_Allreduce(&surf->nlocal,&nstotal,1,MPI_INT,MPI_SUM,world);
  if (me == 0) printf("TOTAL TRI after count: %d\n",nstotal);

  int alltotal,alladd,alldel,allsend,allrecv;
  MPI_Allreduce(&ntotal,&alltotal,1,MPI_INT,MPI_SUM,world);
  MPI_Allreduce(&nadd,&alladd,1,MPI_INT,MPI_SUM,world);
  MPI_Allreduce(&ndel,&alldel,1,MPI_INT,MPI_SUM,world);
  MPI_Allreduce(&nsend,&allsend,1,MPI_INT,MPI_SUM,world);
  MPI_Allreduce(&nrecv,&allrecv,1,MPI_INT,MPI_SUM,world);
  if (me == 0)
    printf("CLEANUP counts: total %d add %d del %d send %d recv %d\n",
           alltotal,alladd,alldel,allsend,allrecv);

  ntotal = 0;
  int nbad = 0;
  int nonface = 0;

  for (icell = 0; icell < nglocal; icell++) {
    if (cells[icell].nsplit <= 0) continue;
    nsurf = cells[icell].nsurf;
    if (nsurf == 0) continue;
    ntotal += nsurf;

    lo = cells[icell].lo;
    hi = cells[icell].hi;

    for (j = 0; j < nsurf; j++) {
      m = cells[icell].csurfs[j];
      iface = Geometry::tri_on_hex_face(tris[m].p1,tris[m].p2,tris[m].p3,lo,hi);
      if (iface < 0) continue;

      norm = tris[m].norm;
      idim = iface/2;
      if (iface % 2 && norm[idim] < 0.0) inwardnorm = 1;
      else if (iface % 2 == 0 && norm[idim] > 0.0) inwardnorm = 1;
      else inwardnorm = 0;

      nonface++;
      if (!inwardnorm) nbad++;
    }
  }

  int nbadall;
  MPI_Allreduce(&nbad,&nbadall,1,MPI_INT,MPI_SUM,world);
  if (me == 0) printf("BAD NORM %d\n",nbadall);

  int nonfaceall;
  MPI_Allreduce(&nonface,&nonfaceall,1,MPI_INT,MPI_SUM,world);
  if (me == 0) printf("Total onface %d\n",nonfaceall);

  if (ntotal != surf->nlocal) error->one(FLERR,"Bad surf total");
  */

  // END of DEBUG
}

/* ----------------------------------------------------------------------
   comparison function invoked by qsort() called by cleanup_MC()
   used to sort the dellist of removed tris into DESCENDING order
   this is not a class method
------------------------------------------------------------------------- */

int compare_indices(const void *iptr, const void *jptr)
{
  int i = *((int *) iptr);
  int j = *((int *) jptr);
  if (i < j) return 1;
  if (i > j) return -1;
  return 0;
}

/* ----------------------------------------------------------------------
   print cube for debugging
------------------------------------------------------------------------- */

void ReadISurf::print_cube()
{ 
  fprintf(screen,"\t %d %d %d %d %d %d %d %d\n",
         v000,v001,v011,v010,v100,v101,v111,v110);
}
