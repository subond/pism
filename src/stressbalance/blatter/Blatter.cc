/* Copyright (C) 2020, 2021 PISM Authors
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

#include <cassert>              // assert
#include <cmath>                // std::pow, std::fabs, std::log10
#include <algorithm>            // std::max
#include <cstring>              // memset

#include "Blatter.hh"
#include "pism/util/fem/FEM.hh"
#include "pism/util/error_handling.hh"
#include "pism/util/Vector2.hh"

#include "util/DataAccess.hh"
#include "util/grid_hierarchy.hh"
#include "pism/util/node_types.hh"

#include "pism/rheology/FlowLawFactory.hh"

#include "pism/stressbalance/StressBalance.hh"
#include "pism/geometry/Geometry.hh"
#include "pism/util/pism_options.hh"

namespace pism {
namespace stressbalance {

/*!
 * Compute node type using domain thickness and the thickness threshold `min_thickness`.
 *
 * An element contains ice if ice thickness at all its nodes equal or exceeds the
 * `min_thickness` threshold.
 *
 * A node is *interior* if all four elements it belongs to contain ice.
 *
 * A node is *exterior* if it belongs to zero icy elements.
 *
 * A node that is neither interior nor exterior is a *boundary* node.
 */
void Blatter::compute_node_type(double min_thickness) {

  IceModelVec2S node_type(m_grid, "node_type", WITH_GHOSTS);
  node_type.set(0.0);

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(m_da, &info); PISM_CHK(ierr, "DMDAGetLocalInfo");
  info = grid_transpose(info);

  // Note that dx, dy, and quadrature don't matter here.
  fem::Q1Element2 E(info, 1.0, 1.0, fem::Q1Quadrature1());

  Parameters p[fem::q1::n_chi];

  IceModelVec::AccessList l{&node_type, &m_parameters};

  // Loop over all the elements with at least one owned node and compute the number of icy
  // elements each node belongs to.
  for (int j = info.gys; j < info.gys + info.gym - 1; j++) {
    for (int i = info.gxs; i < info.gxs + info.gxm - 1; i++) {
      E.reset(i, j);

      E.nodal_values(m_parameters.array(), p);

      // An element is "interior" (contains ice) if all of its nodes have thickness above
      // the threshold
      bool interior = true;
      for (int k = 0; k < fem::q1::n_chi; ++k) {
        if (p[k].thickness < min_thickness) {
          interior = false;
          break;
        }
      }

      for (int k = 0; k < fem::q1::n_chi; ++k) {
        int ii, jj;
        E.local_to_global(k, ii, jj);
        node_type(ii, jj) += interior;
      }
    }
  }

  node_type.update_ghosts();

  // Loop over all the owned nodes and turn the number of "icy" elements this node belongs
  // to into node type.
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {

      switch ((int)node_type(i, j)) {
      case 4:
        m_parameters(i, j).node_type = NODE_INTERIOR;
        break;
      case 0:
        m_parameters(i, j).node_type = NODE_EXTERIOR;
        break;
      default:
        m_parameters(i, j).node_type = NODE_BOUNDARY;
      }
    }
  }
}

/*!
 * Returns true if a node is in the Dirichlet part of the boundary, false otherwise.
 *
 * Used by verification tests.
 */
bool Blatter::dirichlet_node(const DMDALocalInfo &info, const fem::Element3::GlobalIndex& I) {
  (void) info;
  (void) I;
  return false;
}

/*! Dirichlet BC
*/
Vector2 Blatter::u_bc(double x, double y, double z) const {
  (void) x;
  (void) y;
  (void) z;
  return {0.0, 0.0};
}

/*!
 * Return true if an element does not contain ice, i.e. is a part of the "exterior" of the
 * ice mass.
 *
 * @param[in] node_type node type at the nodes of an element (an array of 8 integers; only
 *                      4 are used)
 */
bool Blatter::exterior_element(const int *node_type) {
  // number of nodes per map-plane cell
  int N = 4;
  for (int n = 0; n < N; ++n) {
    if (node_type[n] == NODE_EXTERIOR) {
      return true;
    }
  }
  return false;
}

/*!
 * Return true if the current map-plane cell contains the grounding line, false otherwise.
 *
 * This is used to determine whether to use more quadrature points to estimate integrals
 * over the bottom face of the basal element.
 *
 * The code takes advantage of the ordering of element nodes: lower 4 first, then upper 4.
 * This means that we can loop over the first 4 nodes and ignore the other 4.
 */
bool Blatter::grounding_line(const double *F) {

  // number of nodes per map-plane cell
  int N = 4;

  bool
    grounded = false,
    floating = false;

  for (int n = 0; n < N; ++n) {
    if (F[n] <= 0.0) {
      grounded = true;
    } else {
      floating = true;
    }
  }

  return grounded and floating;
}

/*!
 * Return true if the current vertical face is partially submerged.
 *
 * This is used to determine whether to use more quadrature points to estimate integrals
 * over this face when computing lateral boundary conditions.
 */
bool Blatter::partially_submerged_face(int face, const double *z, const double *sea_level) {
  auto nodes = fem::q13d::incident_nodes[face];

  // number of nodes per face
  int N = 4;

  bool
    above = false,
    below = false;

  for (int n = 0; n < N; ++n) {
    int k = nodes[n];
    if (z[k] > sea_level[k]) {
      above = true;
    } else {
      below = true;
    }
  }

  return above and below;
}

/*!
 * Return true if the current face is a part of the marine ice boundary (i.e. at a
 * partially-submerged vertical cliff), false otherwise.
 *
 * A face is a part of the marine boundary if all four nodes are boundary nodes *and* at
 * least one map-plane location has bottom elevation below sea level (floatation level).
 *
 * If a node is *both* a boundary and a Dirichlet node (this may happen), then we treat it
 * as a boundary node here: element.add_contribution() will do the right thing in this
 * case.
 */
bool Blatter::marine_boundary(int face,
                              const int *node_type,
                              const double *ice_bottom,
                              const double *sea_level) {
  auto nodes = fem::q13d::incident_nodes[face];

  // number of nodes per face
  int N = 4;

  // exclude faces that contain at least one node that is not a part of the boundary
  for (int n = 0; n < N; ++n) {
    if (not (node_type[nodes[n]] == NODE_BOUNDARY)) {
      return false;
    }
  }

  // This face is a part of the lateral boundary. Now we need to check if ice_bottom is
  // below sea_level at one of the nodes of this face.
  for (int n = 0; n < N; ++n) {
    if (ice_bottom[nodes[n]] < sea_level[nodes[n]]) {
      return true;
    }
  }
  return false;
}

/*!
 * Allocate the Blatter-Pattyn stress balance solver
 *
 * @param[in] grid PISM's grid.
 * @param[in] Mz number of vertical levels
 * @param[in] n_levels maximum number of grid levels to use
 * @param[in] coarsening_factor grid coarsening factor
 */
Blatter::Blatter(IceGrid::ConstPtr grid, int Mz, int coarsening_factor)
  : ShallowStressBalance(grid),
    m_parameters(grid, "bp_input_parameters", WITH_GHOSTS),
    m_face4(grid->dx(), grid->dy(), fem::Q1Quadrature4()),    // 4-point Gaussian quadrature
    m_face100(grid->dx(), grid->dy(), fem::Q1QuadratureN(10)) // 100-point quadrature for grounding lines
{

  assert(m_face4.n_pts() <= m_Nq);
  assert(m_face100.n_pts() <= m_Nq);

  auto pism_da = grid->get_dm(1, 0);

  int ierr = setup(*pism_da, grid->periodicity(), Mz, coarsening_factor, "bp_");
  if (ierr != 0) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "Failed to allocate a Blatter solver instance");
  }

  {
    std::vector<double> sigma(Mz);
    double dz = 1.0 / (Mz - 1.0);
    for (int i = 0; i < Mz; ++i) {
      sigma[i] = i * dz;
    }
    sigma.back() = 1.0;

    std::map<std::string,std::string> z_attrs =
      {{"axis", "Z"},
       {"long_name", "scaled Z-coordinate in the ice (z_base=0, z_surface=1)"},
       {"units", "1"},
       {"positive", "up"}};

    m_u_sigma.reset(new IceModelVec3(grid, "uvel_sigma", "z_sigma", sigma, z_attrs));
    m_u_sigma->set_attrs("diagnostic", "u velocity component on the sigma grid", "m s-1", "m s-1", "", 0);

    m_v_sigma.reset(new IceModelVec3(grid, "vvel_sigma", "z_sigma", sigma, z_attrs));
    m_v_sigma->set_attrs("diagnostic", "v velocity component on the sigma grid", "m s-1", "m s-1", "", 0);
  }

  {
    rheology::FlowLawFactory ice_factory("stress_balance.blatter.", m_config, m_EC);
    ice_factory.remove(ICE_GOLDSBY_KOHLSTEDT);
    m_flow_law = ice_factory.create();
  }

  double g = m_config->get_number("constants.standard_gravity");

  m_rho_ice_g = m_config->get_number("constants.ice.density") * g;
  m_rho_ocean_g = m_config->get_number("constants.sea_water.density") * g;

  m_eta_transform = m_config->get_flag("stress_balance.blatter.use_eta_transform");

  m_glen_exponent = m_flow_law->exponent();
}

/*!
 * Restrict 2D and 3D model parameters from a fine grid to a coarse grid.
 *
 * Re-compute node types from geometry.
 *
 * This hook is called every time SNES needs to update coarse-grid data.
 *
 * FIXME: parameters restricted by this hook do not change from one SNES iteration to the
 * next, so we can return early after the first one.
 */
static PetscErrorCode blatter_restriction_hook(DM fine,
                                               Mat mrestrict, Vec rscale, Mat inject,
                                               DM coarse, void *ctx) {
  // Get rid of warnings about unused arguments
  (void) mrestrict;
  (void) rscale;
  (void) inject;
  (void) ctx;

  PetscErrorCode ierr = restrict_data(fine, coarse, "3D_DM"); CHKERRQ(ierr);

  return 0;
}

static PetscErrorCode blatter_coarsening_hook(DM dm_fine, DM dm_coarse, void *ctx) {
  PetscErrorCode ierr;

  int mg_levels = 1;
  {
    const char *prefix;
    ierr = DMGetOptionsPrefix(dm_fine, &prefix); CHKERRQ(ierr);
    auto option = pism::printf("-%spc_mg_levels", prefix);
    mg_levels = options::Integer(option, "", 1);
  }

  ierr = setup_level(dm_coarse, mg_levels); CHKERRQ(ierr);

  ierr = DMCoarsenHookAdd(dm_coarse, blatter_coarsening_hook, blatter_restriction_hook, ctx); CHKERRQ(ierr);

  // 3D
  ierr = create_restriction(dm_fine, dm_coarse, "3D_DM"); CHKERRQ(ierr);

  return 0;
}

/*!
 * Allocates the 3D DM, the corresponding solution vector, and the SNES solver.
 */
PetscErrorCode Blatter::setup(DM pism_da, Periodicity periodicity, int Mz,
                              int coarsening_factor,
                              const std::string &prefix) {
  MPI_Comm comm;
  PetscErrorCode ierr = PetscObjectGetComm((PetscObject)pism_da, &comm); CHKERRQ(ierr);

  // FIXME: add the ability to add a prefix to the option prefix. We need this to be able
  // to run more than one instance of PISM in parallel.
  auto option = pism::printf("-%spc_mg_levels", prefix.c_str());
  int mg_levels = options::Integer(option, "", 1);

  // Check compatibility of Mz, mg_levels, and the coarsening_factor and stop if they are
  // not compatible.
  //
  // We assume that the user also set "-bp_pc_type mg".
  {
    int c = coarsening_factor;
    int M = mg_levels;
    int mz = Mz;
    while (M > 1) {
      // Note: integer division
      if (((mz - 1) / c) * c != mz - 1) {
        int N = std::pow(c, (int)mg_levels - 1);
        auto message = pism::printf("Blatter stress balance solver: settings\n"
                                    "stress_balance.blatter.Mz = %d,\n"
                                    "stress_balance.blatter.coarsening_factor = %d,\n"
                                    "and '%s %d' are not compatible.\n"
                                    "To use N = %d multigrid levels with the coarsening factor C = %d\n"
                                    "stress_balance.blatter.Mz has to be equal to A * C^(N - 1) + 1\n"
                                    "for some positive integer A, e.g. %d, %d, %d, ...",
                                    Mz, c, option.c_str(), mg_levels, mg_levels, c,
                                    N + 1, 2*N + 1, 3*N + 1);
        throw RuntimeError(PISM_ERROR_LOCATION, message);
      }
      mz = (mz - 1) / c + 1;
      M -= 1;
    }
  }

  // DM
  //
  // Note: in the PISM's DA pism_da PETSc's and PISM's meaning of x and y are the same.
  {

    DMInfo info(pism_da);
    assert(info.dims == 2);

    // pad the vertical grid to allow for n_levels multigrid levels
    info.Mz  = Mz;
    info.mz  = 1;
    info.dof = 2;
    info.stencil_width = 1;

    info.bx = periodicity & X_PERIODIC ? DM_BOUNDARY_PERIODIC : DM_BOUNDARY_NONE;
    info.by = periodicity & Y_PERIODIC ? DM_BOUNDARY_PERIODIC : DM_BOUNDARY_NONE;
    info.bz = DM_BOUNDARY_NONE;

    ierr = DMDACreate3d(comm,
                        info.bz, info.bx, info.by, // STORAGE_ORDER
                        DMDA_STENCIL_BOX,
                        info.Mz, info.Mx, info.My, // STORAGE_ORDER
                        info.mz, info.mx, info.my, // STORAGE_ORDER
                        info.dof,                  // dof
                        info.stencil_width,        // stencil width
                        NULL, info.lx, info.ly,    // STORAGE_ORDER
                        m_da.rawptr()); CHKERRQ(ierr);

    ierr = DMSetOptionsPrefix(m_da, prefix.c_str()); CHKERRQ(ierr);

    // semi-coarsening: coarsen in the vertical direction only
    ierr = DMDASetRefinementFactor(m_da, coarsening_factor, 1, 1); CHKERRQ(ierr); // STORAGE_ORDER

    ierr = DMSetFromOptions(m_da); CHKERRQ(ierr);

    ierr = DMSetUp(m_da); CHKERRQ(ierr);

    // set up 3D parameter storage
    ierr = setup_level(m_da, mg_levels); CHKERRQ(ierr);

    // tell PETSc how to coarsen this grid and how to restrict data to a coarser grid
    ierr = DMCoarsenHookAdd(m_da, blatter_coarsening_hook, blatter_restriction_hook, NULL);
    CHKERRQ(ierr);
  }

  // Vec
  {
    ierr = DMCreateGlobalVector(m_da, m_x.rawptr()); CHKERRQ(ierr);

    ierr = VecSetOptionsPrefix(m_x, prefix.c_str()); CHKERRQ(ierr);

    ierr = VecSetFromOptions(m_x); CHKERRQ(ierr);

    ierr = VecDuplicate(m_x, m_x_old.rawptr()); CHKERRQ(ierr);
  }

  // SNES
  {
    ierr = SNESCreate(comm, m_snes.rawptr()); CHKERRQ(ierr);

    ierr = SNESSetOptionsPrefix(m_snes, prefix.c_str()); CHKERRQ(ierr);

    ierr = SNESSetDM(m_snes, m_da); CHKERRQ(ierr);

    ierr = DMDASNESSetFunctionLocal(m_da, INSERT_VALUES,
                                    (DMDASNESFunction)function_callback,
                                    this); CHKERRQ(ierr);

    ierr = DMDASNESSetJacobianLocal(m_da,
                                    (DMDASNESJacobian)jacobian_callback,
                                    this); CHKERRQ(ierr);

    ierr = SNESSetFromOptions(m_snes); CHKERRQ(ierr);
  }

  return 0;
}

/*!
 * Set 2D parameters on the finest grid.
 */
void Blatter::init_2d_parameters(const Inputs &inputs) {

  double
    ice_density   = m_config->get_number("constants.ice.density"),
    water_density = m_config->get_number("constants.sea_water.density"),
    alpha         = ice_density / water_density;

  const IceModelVec2S
    &tauc      = *inputs.basal_yield_stress,
    &H         = inputs.geometry->ice_thickness,
    &b         = inputs.geometry->bed_elevation,
    &sea_level = inputs.geometry->sea_level_elevation;

  {
    IceModelVec::AccessList list{&tauc, &H, &b, &sea_level, &m_parameters};

    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      double
        b_grounded = b(i, j),
        b_floating = sea_level(i, j) - alpha * H(i, j),
        s_grounded = b(i, j) + H(i, j),
        s_floating = sea_level(i, j) + (1.0 - alpha) * H(i, j);

      m_parameters(i, j).tauc       = tauc(i, j);
      m_parameters(i, j).thickness  = H(i, j);
      m_parameters(i, j).sea_level  = sea_level(i, j);
      m_parameters(i, j).bed        = std::max(b_grounded, b_floating);
      m_parameters(i, j).node_type  = NODE_EXTERIOR;
      m_parameters(i, j).floatation = s_floating - s_grounded;
    }
  }

  compute_node_type(m_config->get_number("stress_balance.ice_free_thickness_standard"));

  m_parameters.update_ghosts();
}

/*!
 * Set 3D parameters on the finest grid.
 */
void Blatter::init_ice_hardness(const Inputs &inputs, const petsc::DM &da) {

  auto enthalpy = inputs.enthalpy;
  // PISM's vertical grid:
  const auto &zlevels = enthalpy->levels();
  auto Mz = zlevels.size();

  // solver's vertical grid:
  int Mz_sigma = 0;
  {
    DMDALocalInfo info;
    int ierr = DMDAGetLocalInfo(da, &info); PISM_CHK(ierr, "DMDAGetLocalInfo");
    info = grid_transpose(info);
    Mz_sigma = info.mz;
  }

  const auto &ice_thickness = inputs.geometry->ice_thickness;
  DataAccess<double***> hardness(da, 3, NOT_GHOSTED);

  IceModelVec::AccessList list{enthalpy, &ice_thickness};

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    double H = ice_thickness(i, j);

    const double *E = enthalpy->get_column(i, j);

    for (int k = 0; k < Mz_sigma; ++k) {
      double
        z        = grid_z(0.0, H, Mz_sigma, k),
        depth    = H - z,
        pressure = m_EC->pressure(depth),
        E_local  = 0.0;

      auto k0 = m_grid->kBelowHeight(z);

      if (k0 + 1 < Mz) {
        double lambda = (z - zlevels[k0]) / (zlevels[k0 + 1] - zlevels[k0]);

        E_local = (1.0 - lambda) * E[k0] + lambda * E[k0 + 1];
      } else {
        E_local = E[Mz - 1];
      }

      hardness[j][i][k] = m_flow_law->hardness(E_local, pressure); // STORAGE_ORDER

    } // end of the loop over sigma levels
  } // end of the loop over grid points
}

/*!
 * Get values of 2D parameters at element nodes.
 *
 * This method is re-implemented by derived classes that use periodic boundary conditions.
 */
void Blatter::nodal_parameter_values(const fem::Q1Element3 &element,
                                     Parameters **P,
                                     int i,
                                     int j,
                                     int *node_type,
                                     double *bottom_elevation,
                                     double *ice_thickness,
                                     double *surface_elevation,
                                     double *sea_level) const {

  for (int n = 0; n < fem::q13d::n_chi; ++n) {
    auto I = element.local_to_global(i, j, 0, n);

    auto p = P[I.j][I.i];

    node_type[n]        = p.node_type;
    bottom_elevation[n] = p.bed;
    ice_thickness[n]    = p.thickness;

    if (surface_elevation) {
      surface_elevation[n] = p.bed + p.thickness;
    }

    if (sea_level) {
      sea_level[n] = p.sea_level;
    }
  }
}

Blatter::~Blatter() {
  // empty
}

void Blatter::init_impl() {
  m_log->message(2, "* Initializing the Blatter stress balance...\n");

  InputOptions opts = process_input_options(m_grid->com, m_config);

  if (opts.type == INIT_RESTART) {
    File input_file(m_grid->com, opts.filename, PISM_GUESS, PISM_READONLY);
    bool u_sigma_found = input_file.find_variable("uvel_sigma");
    bool v_sigma_found = input_file.find_variable("vvel_sigma");
    unsigned int start = input_file.nrecords() - 1;

    if (u_sigma_found and v_sigma_found) {
      m_log->message(3, "Reading uvel_sigma and vvel_sigma...\n");

      m_u_sigma->read(input_file, start);
      m_v_sigma->read(input_file, start);

      set_initial_guess(*m_u_sigma, *m_v_sigma);
    } else {
      throw RuntimeError(PISM_ERROR_LOCATION,
                         "uvel_sigma and vvel_sigma not found");
    }
  } else {
    int ierr = VecSet(m_x, 0.0); PISM_CHK(ierr, "VecSet");
  }
}

void Blatter::define_model_state_impl(const File &output) const {
  m_u_sigma->define(output);
  m_v_sigma->define(output);
}

void Blatter::write_model_state_impl(const File &output) const {
  m_u_sigma->write(output);
  m_v_sigma->write(output);
}

void Blatter::update(const Inputs &inputs, bool full_update) {
  (void) inputs;
  (void) full_update;

  PetscErrorCode ierr;

  init_2d_parameters(inputs);
  init_ice_hardness(inputs, m_da);

  // maximum number of continuation steps
  int Nc = 20;

  double
    schoofLen = m_config->get_number("flow_law.Schoof_regularizing_length", "m"),
    schoofVel = m_config->get_number("flow_law.Schoof_regularizing_velocity", "m second-1"),
    // desired regularization parameter
    eps       = pow(schoofVel / schoofLen, 2.0),
    // gamma is a number such that 10^gamma <= eps. It is used to convert lambda in [0, 1] to eps_n
    gamma     = std::floor(std::log10(eps)),
    // starting value of lambda (input)
    lambda_min = 0.75,
    // final value of lambda (fixed)
    lambda_max = 1.0,
    // minimum step length (input)
    delta_min = 0.01,
    // maximum step length (input)
    delta_max = 0.2,
    // initial increment of lambda (input)
    delta0    = 0.05,
    // "aggressiveness" of the step increase, a non-negative number (input)
    A         = 1.0;

  // set lambda and delta to solve the desired (not overregularized) problem first
  double
    lambda = lambda_max,
    delta  = delta0;

  // total number of SNES and KSP iterations
  int snes_total_it = 0;
  int ksp_total_it  = 0;

  PetscInt snes_max_it = 0;
  ierr = SNESGetTolerances(m_snes, NULL, NULL, NULL, &snes_max_it, NULL);
  PISM_CHK(ierr, "SNESGetTolerances");

  // store the "old" initial guess
  ierr = VecCopy(m_x, m_x_old); PISM_CHK(ierr, "VecCopy");

  for (int N = 0; N < Nc + 1; ++N) {
    // Set the regularization parameter:
    m_viscosity_eps = std::max(std::pow(10.0, lambda * gamma), eps);

    if (N > 0) {
      m_log->message(2, "Blatter solver: step %d with lambda = %f, eps = %e\n",
                     N, lambda, m_viscosity_eps);
    } else {
      m_log->message(2, "Blatter solver: start with eps = %e\n", m_viscosity_eps);
    }

    // Solve the system:
    ierr = SNESSolve(m_snes, NULL, m_x); PISM_CHK(ierr, "SNESSolve");

    SNESConvergedReason reason;
    ierr = SNESGetConvergedReason(m_snes, &reason);
    PISM_CHK(ierr, "SNESGetConvergedReason");
    PetscInt snes_it, ksp_it;
    {
      ierr = SNESGetIterationNumber(m_snes, &snes_it);
      PISM_CHK(ierr, "SNESGetIterationNumber");

      ierr = SNESGetLinearSolveIterations(m_snes, &ksp_it);
      PISM_CHK(ierr, "SNESGetLinearSolveIterations");
    }

    // report number of iterations for this continuation step
    if (N > 0) {
      m_log->message(2, "Blatter solver: %s step %d with lambda = %f,"
                     " eps = %e: SNES: %d, KSP: %d\n",
                     SNESConvergedReasons[reason], N, lambda, m_viscosity_eps,
                     (int)snes_it, (int)ksp_it);
    }

    snes_total_it += snes_it;
    ksp_total_it  += ksp_it;

    if (reason > 0) {
      // converged

      if (m_viscosity_eps <= eps) {
        // ... while solving the desired (not overregularized) problem
        m_log->message(2, "Blatter solver: done. SNES: %d, KSP: %d\n",
                       snes_total_it, ksp_total_it);
        goto bp_done;
      }

      // store solution as the "old" initial guess we may need to revert to
      ierr = VecCopy(m_x, m_x_old); PISM_CHK(ierr, "VecCopy");

      if (N > 1) {
        // adjust delta using the formula from LOCA (equation 2.8 in Salinger2002
        // corrected using the code in Trilinos).
        double F = (snes_max_it - snes_it) / (double)snes_max_it;
        delta *= 1.0 + A * F * F;
      }

      delta = std::min(delta, delta_max);

      // ensure that delta does not take us past lambda_max
      if (lambda + delta > lambda_max) {
        delta = lambda_max - lambda;
      }

      m_log->message(2, "  Using delta = %f\n", delta);

      lambda += delta;
    } else if (reason == SNES_DIVERGED_LINE_SEARCH or
               reason == SNES_DIVERGED_MAX_IT) {

      if (N == 0) {
        lambda = lambda_min;
        delta  = delta0;

        ierr = VecSet(m_x, 0.0); PISM_CHK(ierr, "VecSet");
        ierr = VecSet(m_x_old, 0.0); PISM_CHK(ierr, "VecSet");

        m_log->message(2, "  Starting parameter continuation with lambda = %f\n",
                       lambda);
      } else {

        ierr = VecCopy(m_x_old, m_x); PISM_CHK(ierr, "VecCopy");

        // revert lambda to the previous value
        lambda -= delta;

        if (lambda < lambda_min) {
          throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                        "Blatter solver: Parameter continuation failed");
        }

        if (std::fabs(delta - delta_min) < 1e-6) {
          throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                        "Blatter solver: cannot reduce the continuation step");
        }

        // reduce the step size
        delta *= 0.5;

        delta = pism::clip(delta, delta_min, delta_max);

        lambda += delta;
        // Note that this delta will not take us past lambda_max because the original
        // delta satisfies lambda + delta <= lambda_max.

        m_log->message(2, "  Back-tracking to lambda = %f using delta = %f\n",
                       lambda, delta);
      }
    } else {
      // Other kinds of failures
      if (reason == SNES_DIVERGED_LINEAR_SOLVE) {
        KSP ksp;
        KSPConvergedReason ksp_reason;
        ierr = SNESGetKSP(m_snes, &ksp); PISM_CHK(ierr, "SNESGetKSP");

        ierr = KSPGetConvergedReason(ksp, &ksp_reason);
        PISM_CHK(ierr, "KSPGetConvergedReason");

        m_log->message(2, "  Linear solver: %s\n",
                       KSPConvergedReasons[ksp_reason]);
      }

      throw RuntimeError(PISM_ERROR_LOCATION, "Blatter solver failed");
    }
  }

  throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                "Blatter solver failed after %d parameter continuation steps", Nc);

 bp_done:
  // put basal velocity in m_velocity to use it in the next call
  get_basal_velocity(m_velocity);
  compute_basal_frictional_heating(m_velocity, *inputs.basal_yield_stress,
                                   inputs.geometry->cell_type,
                                   m_basal_frictional_heating);

  compute_averaged_velocity(m_velocity);

  // copy the solution from m_x to m_u_sigma, m_v_sigma for re-starting
  copy_solution();
}

void Blatter::copy_solution() {
  Vector2 ***x = nullptr;
  int ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

  int Mz = m_u_sigma->levels().size();

  IceModelVec::AccessList list{m_u_sigma.get(), m_v_sigma.get()};

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    auto u = m_u_sigma->get_column(i, j);
    auto v = m_v_sigma->get_column(i, j);

    for (int k = 0; k < Mz; ++k) {
      u[k] = x[j][i][k].u;      // STORAGE_ORDER
      v[k] = x[j][i][k].v;      // STORAGE_ORDER
    }
  }

  ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");
}

void Blatter::get_basal_velocity(IceModelVec2V &result) {
  Vector2 ***x = nullptr;
  int ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

  IceModelVec::AccessList list{&result};

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    result(i, j) = x[j][i][0];      // STORAGE_ORDER
  }

  ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");
}


void Blatter::set_initial_guess(const IceModelVec3 &u_sigma,
                                const IceModelVec3 &v_sigma) {
  Vector2 ***x = nullptr;
  int ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

  int Mz = m_u_sigma->levels().size();

  IceModelVec::AccessList list{&u_sigma, &v_sigma};

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    auto u = u_sigma.get_column(i, j);
    auto v = v_sigma.get_column(i, j);

    for (int k = 0; k < Mz; ++k) {
      x[j][i][k].u = u[k];      // STORAGE_ORDER
      x[j][i][k].v = v[k];      // STORAGE_ORDER
    }
  }

  ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");
}

void Blatter::compute_averaged_velocity(IceModelVec2V &result) {
  PetscErrorCode ierr;

  Vector2 ***x = nullptr;
  ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

  int Mz = m_u_sigma->levels().size();

  IceModelVec::AccessList list{&result, &m_parameters};

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    double H = m_parameters(i, j).thickness;

    Vector2 V(0.0, 0.0);

    if (H > 0.0) {
      // use trapezoid rule to compute the column average
      double dz = H / (Mz - 1);
      for (int k = 0; k < Mz - 1; ++k) {
        V += x[j][i][k] + x[j][i][k + 1]; // STORAGE_ORDER
      }
      V *= (0.5 * dz) / H;
    }

    result(i, j) = V;
  }

  ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");

  result.update_ghosts();
}


IceModelVec3::Ptr Blatter::velocity_u_sigma() const {
  return m_u_sigma;
}

IceModelVec3::Ptr Blatter::velocity_v_sigma() const {
  return m_v_sigma;
}

} // end of namespace stressbalance
} // end of namespace pism
