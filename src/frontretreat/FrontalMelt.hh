/* Copyright (C) 2016, 2017, 2018, 2019 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef FRONTALMELT_H
#define FRONTALMELT_H

#include "FrontRetreat.hh"
#include "pism/hydrology/Hydrology.hh"

namespace pism {

class FrontalMelt : public FrontRetreat {
public:
  FrontalMelt(IceGrid::ConstPtr grid);
  virtual ~FrontalMelt();

  void init();

protected:
  virtual DiagnosticList diagnostics_impl() const;

  void compute_retreat_rate(const FrontRetreatInputs &inputs,
                            IceModelVec2S &result) const;
};

} // end of namespace pism


#endif /* FRONTALMELT_H */
