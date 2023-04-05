/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.sandia.gov
   Steve Plimpton, sjplimp@gmail.com, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(temp/rescale/kk,FixTempRescaleKokkos)

#else

#ifndef SPARTA_FIX_TEMP_RESCALE_KOKKOS_H
#define SPARTA_FIX_TEMP_RESCALE_KOKKOS_H

#include "fix_temp_rescale.h"
#include "kokkos_type.h"

namespace SPARTA_NS {

struct TagFixTempRescale_end_of_step_no_average{};
struct TagFixTempRescale_end_of_step_average1{};
struct TagFixTempRescale_end_of_step_average2{};

class FixTempRescaleKokkos : public FixTempRescale {
 public:

  struct REDUCE {
    bigint n;
    double t;
    KOKKOS_INLINE_FUNCTION
    REDUCE() {
      n = 0;
      t = 0.0;
    }
    KOKKOS_INLINE_FUNCTION
    REDUCE& operator+=(const REDUCE &rhs) {
      n += rhs.n;
      t += rhs.t;
      return *this;
    }

    KOKKOS_INLINE_FUNCTION
    void operator+=(const volatile REDUCE &rhs) volatile {
      n += rhs.n;
      t += rhs.t;
    }
  };

  FixTempRescaleKokkos(class SPARTA *, int, char **);
  virtual ~FixTempRescaleKokkos() {}

  KOKKOS_INLINE_FUNCTION
  void operator()(TagFixTempRescale_end_of_step_no_average, const int&) const;

  KOKKOS_INLINE_FUNCTION
  void operator()(TagFixTempRescale_end_of_step_average1, const int&, REDUCE&) const;

  KOKKOS_INLINE_FUNCTION
  void operator()(TagFixTempRescale_end_of_step_average2, const int&) const;

 private:
  double t_target,vscale;

  DAT::t_float_1d_3 d_vcom;

  t_particle_1d d_particles;
  t_species_1d d_species;

  DAT::t_int_1d d_cellcount;
  DAT::t_int_2d d_plist;
  t_cell_1d d_cells;

  void end_of_step_no_average(double);
  void end_of_step_average(double);
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running SPARTA to see the offending line.

E: Cannot open fix print file %s

The output file generated by the fix print command cannot be opened

*/
