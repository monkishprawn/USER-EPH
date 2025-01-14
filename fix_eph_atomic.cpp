/*
 * Authors of the extension Artur Tamm, Alfredo Correa
 * e-mail: artur.tamm.work@gmail.com
 */

// external headers
#include <iostream>
#include <cstring> // TODO: remove
#include <string>
#include <cstdlib>
#include <limits>
#include <algorithm>
#include <cassert>

// lammps headers
#include "error.h"
#include "domain.h"
#include "neighbor.h"
#include "neigh_request.h"
#include "neigh_list.h"
#include "atom.h"
#include "memory.h"
#include "random_mars.h"
#include "force.h"
#include "update.h"
#include "comm.h"

// internal headers
#include "fix_eph_atomic.h"
#include "eph_beta.h"
#include "eph_kappa.h"

using namespace LAMMPS_NS;
using namespace FixConst;

/*
 * TODO: implement write restart
 */

/**
   * FixEPH arguments
   * arg[ 0] <- fix ID
   * arg[ 1] <- group
   * arg[ 2] <- name
   * arg[ 3] <- rng seed
   * arg[ 4] <- eph parameter; 0 disable all terms; 1 enable friction term; 2 enable random force; 4 enable fde;
   * arg[ 5] <- initial electronic temperature
   * arg[ 6] <- input file for initial temperatures; how do we load temperatures?
   * arg[ 7] <- number of inner loops n < 1 -> automatic selection
   * arg[ 8] <- output file for temperatures
   * arg[ 9] <- input file for eph model functions
   * arg[10] <- input file for kappa model functions
   * arg[11] <- element name for type 0
   * arg[12] <- element name for type 1
   * ...
   **/

// constructor
FixEPHAtomic::FixEPHAtomic(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg) {

  if (narg < 12) error->all(FLERR, "fix_eph_atomic: too few arguments");
  if (atom->natoms < 1) error->all(FLERR, "fix_eph_atomic: error no atoms in simulation");
  MPI_Comm_rank(world, &my_id);
  MPI_Comm_size(world, &nr_ps);
  
  if(my_id == 0) {
    std::cout << "!!! WARNING WARNING WARNING !!!\n";
    std::cout << "This code is under development.\n";
    std::cout << "Use at your own risk.\n";
    std::cout << "!!! WARNING WARNING WARNING !!!\n";
  }
  
  state = FixState::NONE;
  { // setup fix properties
    vector_flag = 1; // fix is able to output a vector compute
    size_vector = 2; // 2 elements in the vector
    global_freq = 1; // frequency for vector data
    extvector = 1; // external vector allocated by this fix???
    nevery = 1; // call end_of_step every step
    peratom_flag = 1; // fix provides per atom values
    size_peratom_cols = 12; // per atom has 12 dimensions
    peratom_freq = 1; // per atom values are provided every step
    //ghostneigh = 1; // neighbours of neighbours

    comm_forward = 3; // forward communication is needed
    //~ comm_forward = 1; // forward communication is needed
    comm->ghost_velocity = 1; // special: fix requires velocities for ghost atoms
  }

  { // setup arrays
    f_EPH = nullptr;
    f_RNG = nullptr;

    w_i = nullptr;

    rho_i = nullptr;
    array = nullptr;

    xi_i = nullptr;

    rho_a_i = nullptr;
    E_a_i = nullptr;
    dE_a_i = nullptr;
    T_a_i = nullptr;

    list = nullptr;

    // NO ARRAYS BEFORE THIS
    grow_arrays(atom->nmax);
    atom->add_callback(0);

    // zero arrays, so they would not contain garbage
    size_t nlocal = atom->nlocal;
    size_t ntotal = atom->nghost + nlocal;

    std::fill_n(&(rho_i[0]), ntotal, 0);
    std::fill_n(&(xi_i[0][0]), 3 * ntotal, 0);
    std::fill_n(&(w_i[0][0]), 3 * ntotal, 0);

    std::fill_n(&(f_EPH[0][0]), 3 * ntotal, 0);
    std::fill_n(&(f_RNG[0][0]), 3 * ntotal, 0);

    std::fill_n(&(array[0][0]), size_peratom_cols * ntotal, 0);

    std::fill_n(&(rho_a_i[0]), ntotal, 0);
    std::fill_n(&(E_a_i[0][0]), 2 * ntotal, 0);
    std::fill_n(&(dE_a_i[0]), ntotal, 0);
  }

  { // some other variables
    types = atom->ntypes;
    eta_factor = sqrt(2.0 * force->boltz / update->dt);
    kB = force->boltz;
  }

  { /** integrator functionality **/
    dtv = update->dt;
    dtf = 0.5 * update->dt * force->ftm2v;
  }

  // initialise rng
  seed = atoi(arg[3]);
  random = new RanMars(lmp, seed + my_id);

  // read model behaviour parameters
  eph_flag = strtol(arg[4], NULL, 0);

  // print enabled fix functionality
  if(my_id == 0) {
    std::cout << '\n';
    std::cout << "Flag read: " << arg[4] << " -> " << eph_flag << '\n';
    if(eph_flag & Flag::FRICTION) { std::cout << "Friction evaluation: ON\n"; }
    if(eph_flag & Flag::RANDOM) { std::cout << "Random evaluation: ON\n"; }
    if(eph_flag & Flag::HEAT) { std::cout << "Heat diffusion solving: ON\n"; }
    if(eph_flag & Flag::NOINT) { std::cout << "No integration: ON\n"; }
    if(eph_flag & Flag::NOFRICTION) { std::cout << "No friction application: ON\n"; }
    if(eph_flag & Flag::NORANDOM) { std::cout << "No random application: ON\n"; }
    std::cout << '\n';
  }

  // argument 5  and 6 are handled below

  { // setup automagic inner loops or not
    inner_loops = atoi(arg[7]);

    if(inner_loops < 1) { inner_loops = 0; }
  }

  { // setup output
    if(strcmp("NULL" , arg[8]) == 0) { } // do nothing for now
  }

  int n_elem = 11; // location where element names start

  if(types > (narg - n_elem)) {
    error->all(FLERR, "fix_eph_atomic: number of types larger than provided in fix");
  }

  type_map_beta.resize(types);
  type_map_kappa.resize(types);

  beta = Beta(arg[9]);
  kappa = Kappa(arg[10]);

  if(beta.get_n_elements() < 1) {
    error->all(FLERR, "fix_eph_atomic: no elements found in beta file");
  }

  if(kappa.n_elements < 1) {
    error->all(FLERR, "fix_eph_atomic: no elements found in kappa file");
  }

  r_cutoff = beta.get_r_cutoff();
  r_cutoff_sq = beta.get_r_cutoff_sq();
  rho_cutoff = beta.get_rho_cutoff();

  // do element mapping for beta
  for(size_t i = 0; i < types; ++i) {
    type_map_beta[i] = std::numeric_limits<int>::max();
    type_map_kappa[i] = std::numeric_limits<int>::max();

    for(size_t j = 0; j < beta.get_n_elements(); ++j) {
      if((beta.get_element_name(j)).compare(arg[n_elem + i]) == 0) {
        type_map_beta[i] = j;
        break;
      }
    }

    for(size_t j = 0; j < kappa.n_elements; ++j) {
      if((kappa.element_name[j]).compare(arg[n_elem + i]) == 0) {
        type_map_kappa[i] = j;
        break;
      }
    }

    if(type_map_beta[i] > types || type_map_kappa[i] > types) {
      error->all(FLERR, "fix_eph_atomic: elements not found in input file");
    }
  }

  { // setup temperatures per atom
    double v_Te = atof(arg[5]);

    if(strcmp("NULL" , arg[6]) == 0) {
      for(size_t i = 0; i < atom->nlocal; ++i) {
        if(atom->mask[i] & groupbit) {
          E_a_i[i][0] = kappa.E_T_atomic[type_map_kappa[atom->type[i] - 1]](v_Te);
        }
      }
    }
    //~ else {
      //~ std::fill_n(&(T_a_i[0]), atom->nlocal, v_Te); // TODO: placeholder
    //~ }
  }

  { // create initial value for total electronic energy
    Ee = 0.;
    for(size_t i = 0; i < atom->nlocal; ++i) {
      if(atom->mask[i] & groupbit) {
        Ee += E_a_i[i][0];
      }
    }
    MPI_Allreduce(MPI_IN_PLACE, &Ee, 1, MPI_DOUBLE, MPI_SUM, world);
  }
  
  { // calculate the local temperatures
    Te = 0.0;
    int atom_counter = 0;
    
    for(size_t i = 0; i < atom->nlocal; ++i) {
      if(atom->mask[i] & groupbit) {
        T_a_i[i] = kappa.E_T_atomic[type_map_kappa[atom->type[i] - 1]].reverse(E_a_i[i][0]);
        Te += T_a_i[i];
        atom_counter++;
      }
    }
    if(atom_counter > 0) { Te /= static_cast<double>(atom_counter); }
    int proc_counter = (atom_counter > 0) ? 1 : 0;
    MPI_Allreduce(MPI_IN_PLACE, &Te, 1, MPI_DOUBLE, MPI_SUM, world);
    MPI_Allreduce(MPI_IN_PLACE, &proc_counter, 1, MPI_INT, MPI_SUM, world);

    Te /= static_cast<double>(proc_counter);
  }
  
  { // put initial values into array
    populate_array();
  }
}

// destructor
FixEPHAtomic::~FixEPHAtomic() {
  delete random;

  atom->delete_callback(id, 0);
  memory->destroy(rho_i);

  memory->destroy(array);

  memory->destroy(f_EPH);
  memory->destroy(f_RNG);
  memory->destroy(xi_i);
  memory->destroy(w_i);

  memory->destroy(rho_a_i);
  memory->destroy(E_a_i);
  memory->destroy(dE_a_i);
  memory->destroy(T_a_i);
}

void FixEPHAtomic::init() {
  if (domain->dimension == 2) {
    error->all(FLERR,"Cannot use fix eph with 2d simulation");
  }
  if (domain->nonperiodic != 0) {
    error->all(FLERR,"Cannot use nonperiodic boundares with fix eph");
  }
  if (domain->triclinic) {
    error->all(FLERR,"Cannot use fix eph with triclinic box");
  }

  /* copy paste from vcsgc */
  /** we are a fix and we need full neighbour list **/
  int request_style = NeighConst::REQ_FULL | NeighConst::REQ_GHOST;
  auto req = neighbor->add_request(this, request_style);
  req->set_cutoff(r_cutoff);
  //int irequest = neighbor->request((void*)this, this->instance_me);
  //neighbor->requests[irequest]->pair = 0;
  //neighbor->requests[irequest]->fix = 1;
  //neighbor->requests[irequest]->half = 0;
  //neighbor->requests[irequest]->full = 1;
  //neighbor->requests[irequest]->ghost = 1;
  //neighbor->requests[irequest]->cutoff = r_cutoff;

  reset_dt();
}

void FixEPHAtomic::init_list(int id, NeighList *ptr) {
  this->list = ptr;
}

int FixEPHAtomic::setmask() {
  int mask = 0;
  mask |= POST_FORCE;
  mask |= END_OF_STEP;
  /* integrator functionality */
  mask |= INITIAL_INTEGRATE;
  mask |= FINAL_INTEGRATE;

  return mask;
}

/* integrator functionality */
void FixEPHAtomic::initial_integrate(int) {
  if(eph_flag & Flag::NOINT) return;

  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double *mass = atom->mass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  for (size_t i = 0; i < nlocal; ++i) {
    if (mask[i] & groupbit) {
      double dtfm = dtf / mass[type[i]];
      v[i][0] += dtfm * f[i][0];
      v[i][1] += dtfm * f[i][1];
      v[i][2] += dtfm * f[i][2];

      x[i][0] += dtv * v[i][0];
      x[i][1] += dtv * v[i][1];
      x[i][2] += dtv * v[i][2];
    }
  }
}

void FixEPHAtomic::final_integrate() {
  if(eph_flag & Flag::NOINT) return;

  double **v = atom->v;
  double **f = atom->f;
  double *mass = atom->mass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  for (size_t i = 0; i < nlocal; ++i) {
    if (mask[i] & groupbit) {
      double dtfm = dtf / mass[type[i]];
      v[i][0] += dtfm * f[i][0];
      v[i][1] += dtfm * f[i][1];
      v[i][2] += dtfm * f[i][2];
    }
  }
}

void FixEPHAtomic::end_of_step() {
  double **x = atom->x;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  double E_local = 0.0;
  double T_local = 0.0;
  
  if(eph_flag & Flag::HEAT) { heat_solve(); }  
  
  { // average temperature calculation
    int atom_counter = 0;
    for(size_t i = 0; i < nlocal; ++i) { // calculate the total temperature
      if(mask[i] & groupbit) {
        E_local += E_a_i[i][0];
        T_a_i[i] = kappa.E_T_atomic[type_map_kappa[type[i] - 1]].reverse(E_a_i[i][0]);
        T_local += T_a_i[i];
        atom_counter++;
      }
    }
    
    if(atom_counter > 0) { T_local /= static_cast<double>(atom_counter); }
    int proc_counter = (atom_counter > 0) ? 1 : 0;
    
    // this is for checking energy conservation
    MPI_Allreduce(MPI_IN_PLACE, &E_local, 1, MPI_DOUBLE, MPI_SUM, world); // make this into a vector?
    MPI_Allreduce(MPI_IN_PLACE, &T_local, 1, MPI_DOUBLE, MPI_SUM, world);
    MPI_Allreduce(MPI_IN_PLACE, &proc_counter, 1, MPI_INT, MPI_SUM, world);
    
    Ee = E_local;
    Te = T_local / static_cast<double>(proc_counter);
  }
  
  populate_array();
}

void FixEPHAtomic::populate_array() {
  for(size_t i = 0; i < atom->nlocal; ++i) {
    if(atom->mask[i] & groupbit) {
      int itype = atom->type[i];
      array[i][ 0] = rho_i[i];
      array[i][ 1] = beta.get_beta(type_map_beta[itype - 1], rho_i[i]);
      array[i][ 2] = f_EPH[i][0];
      array[i][ 3] = f_EPH[i][1];
      array[i][ 4] = f_EPH[i][2];
      array[i][ 5] = f_RNG[i][0];
      array[i][ 6] = f_RNG[i][1];
      array[i][ 7] = f_RNG[i][2];
      array[i][ 8] = rho_a_i[i];
      array[i][ 9] = E_a_i[i][0];
      array[i][10] = dE_a_i[i];
      array[i][11] = T_a_i[i];
    }
    else {
      array[i][ 0] = 0.0;
      array[i][ 1] = 0.0;
      array[i][ 2] = 0.0;
      array[i][ 3] = 0.0;
      array[i][ 4] = 0.0;
      array[i][ 5] = 0.0;
      array[i][ 6] = 0.0;
      array[i][ 7] = 0.0;
      array[i][ 8] = 0.0;
      array[i][ 9] = 0.0;
      array[i][10] = 0.0;
      array[i][11] = 0.0;
    }
  }
}

void FixEPHAtomic::calculate_environment() {
  double **x = atom->x;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  int *numneigh = list->numneigh;
  int **firstneigh = list->firstneigh;

  // loop over atoms and their neighbours and calculate rho and beta(rho)
  for(size_t i = 0; i != nlocal; ++i) {
    rho_i[i] = 0;
    rho_a_i[i] = 0;

    // check if current atom belongs to fix group and if an atom is local
    if(mask[i] & groupbit) {
      int itype = type[i];
      int *jlist = firstneigh[i];
      int jnum = numneigh[i];

      for(size_t j = 0; j != jnum; ++j) {
        int jj = jlist[j];
        jj &= NEIGHMASK;
        
        // this is new behaviour. In the past we did not check about the neighbours group status
        if(!(mask[jj] & groupbit)) { continue; } // neighbour is not in the group 
        
        int jtype = type[jj];
        double r_sq = get_distance_sq(x[jj], x[i]);

        if(r_sq < r_cutoff_sq) {
          rho_i[i] += beta.get_rho_r_sq(type_map_beta[jtype - 1], r_sq);
        }

        if(r_sq < kappa.r_cutoff_sq) {
          rho_a_i[i] += kappa.rho_r_sq[type_map_kappa[jtype - 1]](r_sq);
        }
      }
    }
  }
}

void FixEPHAtomic::force_prl() {
  double **x = atom->x;
  double **v = atom->v;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  int *numneigh = list->numneigh;
  int **firstneigh = list->firstneigh;
  
  double const l_dt = update->dt;
  
  // create friction forces
  if(eph_flag & Flag::FRICTION) {
    // w_i = W_ij^T v_j
    for(size_t i = 0; i != nlocal; ++i) {
      if(mask[i] & groupbit) {
        int itype = type[i];
        int *jlist = firstneigh[i];
        int jnum = numneigh[i];

        if(!(rho_i[i] > 0)) { continue; }

        double alpha_i = beta.get_alpha(type_map_beta[itype - 1], rho_i[i]);

        for(size_t j = 0; j != jnum; ++j) {
          int jj = jlist[j];
          jj &= NEIGHMASK;
          int jtype = type[jj];
          
          if(!(mask[jj] & groupbit)) { continue; }
          
          // calculate the e_ij vector TODO: change these
          double e_ij[3];
          double e_r_sq = get_difference_sq(x[jj], x[i], e_ij);

          // first sum
          if(e_r_sq >= r_cutoff_sq) { continue; }

          double v_rho_ji = beta.get_rho_r_sq(type_map_beta[jtype - 1], e_r_sq);
          double prescaler = alpha_i * v_rho_ji / (rho_i[i] * e_r_sq);

          double e_v_v1 = get_scalar(e_ij, v[i]);
          double var1 = prescaler * e_v_v1;

          double e_v_v2 = get_scalar(e_ij, v[jj]);
          double var2 = prescaler * e_v_v2;

          double dvar = var1 - var2;
          w_i[i][0] += dvar * e_ij[0];
          w_i[i][1] += dvar * e_ij[1];
          w_i[i][2] += dvar * e_ij[2];
        }
      }
    }

    state = FixState::WI;
    comm->forward_comm(this);
    
    // now calculate the forces
    // f_i = W_ij w_j
    for(size_t i = 0; i != nlocal; ++i) {
      if(mask[i] & groupbit) {
        int itype = type[i];
        int *jlist = firstneigh[i];
        int jnum = numneigh[i];

        if( not(rho_i[i] > 0) ) { continue; }

        double alpha_i = beta.get_alpha(type_map_beta[itype - 1], rho_i[i]);

        for(size_t j = 0; j != jnum; ++j) {
          int jj = jlist[j];
          jj &= NEIGHMASK;
          int jtype = type[jj];
          
          if(!(mask[jj] & groupbit)) { continue; }
          
          // calculate the e_ij vector
          double e_ij[3];
          double e_r_sq = get_difference_sq(x[jj], x[i], e_ij);

          if(e_r_sq >= r_cutoff_sq or not(rho_i[jj] > 0)) { continue; }
          
          double alpha_j = beta.get_alpha(type_map_beta[jtype - 1], rho_i[jj]);

          double v_rho_ji = beta.get_rho_r_sq(type_map_beta[jtype - 1], e_r_sq);
          double e_v_v1 = get_scalar(e_ij, w_i[i]);
          double var1 = alpha_i * v_rho_ji * e_v_v1 / (rho_i[i] * e_r_sq);

          double v_rho_ij = beta.get_rho_r_sq(type_map_beta[itype - 1], e_r_sq);
          double e_v_v2 = get_scalar(e_ij, w_i[jj]);
          double var2 = alpha_j * v_rho_ij * e_v_v2 / (rho_i[jj] * e_r_sq);

          double dvar = var1 - var2;
          double const f_ij[3] = {dvar * e_ij[0], dvar * e_ij[1], dvar * e_ij[2]};
          
          // friction is negative!
          f_EPH[i][0] -= f_ij[0];
          f_EPH[i][1] -= f_ij[1];
          f_EPH[i][2] -= f_ij[2];
          
          if(!(eph_flag & Flag::NOFRICTION)) {
            dE_a_i[i] += 0.5 * f_ij[0] * (v[i][0] - v[jj][0]) * l_dt;
            dE_a_i[i] += 0.5 * f_ij[1] * (v[i][1] - v[jj][1]) * l_dt;
            dE_a_i[i] += 0.5 * f_ij[2] * (v[i][2] - v[jj][2]) * l_dt;
          }
        }
      }
    }
  }

  // create random forces
  if(eph_flag & Flag::RANDOM) {
    for(size_t i = 0; i != nlocal; i++) {
      if(mask[i] & groupbit) {
        int itype = type[i];
        int *jlist = firstneigh[i];
        int jnum = numneigh[i];

        if(!(rho_i[i] > 0)) { continue; }

        double alpha_i = beta.get_alpha(type_map_beta[itype - 1], rho_i[i]);
        double v_Ti = sqrt(kappa.E_T_atomic[type_map_kappa[itype - 1]].reverse(E_a_i[i][0]));

        for(size_t j = 0; j != jnum; ++j) {
          int jj = jlist[j];
          jj &= NEIGHMASK;
          int jtype = type[jj];
          
          if(!(mask[jj] & groupbit)) { continue; }
          
          // calculate the e_ij vector
          double e_ij[3];
          double e_r_sq = get_difference_sq(x[jj], x[i], e_ij);

          if((e_r_sq >= r_cutoff_sq) || !(rho_i[jj] > 0)) { continue; }

          double alpha_j = beta.get_alpha(type_map_beta[jtype - 1], rho_i[jj]);
          
          double v_Tj = sqrt(kappa.E_T_atomic[type_map_kappa[jtype - 1]].reverse(E_a_i[jj][0]));

          double v_rho_ji = beta.get_rho_r_sq(type_map_beta[jtype - 1], e_r_sq);
          double e_v_xi1 = get_scalar(e_ij, xi_i[i]);
          double var1 = v_Ti * alpha_i * v_rho_ji * e_v_xi1 / (rho_i[i] * e_r_sq);

          double v_rho_ij = beta.get_rho_r_sq(type_map_beta[itype - 1], e_r_sq);
          double e_v_xi2 = get_scalar(e_ij, xi_i[jj]);
          double var2 = v_Tj * alpha_j * v_rho_ij * e_v_xi2 / (rho_i[jj] * e_r_sq);
          
          double const dvar = eta_factor * (var1 - var2);
          
          double const f_ij[3] = {dvar * e_ij[0], dvar * e_ij[1], dvar * e_ij[2]};
          f_RNG[i][0] += f_ij[0];
          f_RNG[i][1] += f_ij[1];
          f_RNG[i][2] += f_ij[2];
          
          if(!(eph_flag & Flag::NORANDOM)) {
            dE_a_i[i] -= 0.5 * f_ij[0] * (v[i][0] - v[jj][0]) * l_dt;
            dE_a_i[i] -= 0.5 * f_ij[1] * (v[i][1] - v[jj][1]) * l_dt;
            dE_a_i[i] -= 0.5 * f_ij[2] * (v[i][2] - v[jj][2]) * l_dt;
          }
        }
      }
    }
  }
}

void FixEPHAtomic::heat_solve() {
  double **x = atom->x;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  int *numneigh = list->numneigh;
  int **firstneigh = list->firstneigh;

  int loops = 1;

  // test stability
  if(inner_loops > 0) { loops = inner_loops; } // user defined number of loops
  else { // automatic number of loops
    double stability = 1.0;
    loops = 1;
  }

  double scaling = 1.0 / static_cast<double>(loops);
  double dt = update->dt * scaling;

  for(size_t i = 0; i < loops; ++i) {
    { // add small portion of energy and redistribute temperatures
      for(size_t j = 0; j < nlocal; ++j) {
        if(mask[j] & groupbit) {
          E_a_i[j][0] += dE_a_i[j] * scaling;
          if(E_a_i[j][0] < 0.0) { 
            #ifndef NDEBUG
            std::cerr << "WARNING NEGATIVE TEMPERATURES FOR ATOM " 
              << j << ' ' << type[j]-1 << ' ' << E_a_i[j][0] << '\n';
            std::cerr << "DECREASE DT or add additional loops\n";
            #endif
            E_a_i[j][0] = 0.0; // energy cannot go negative
          }
        }
      }

      state = FixState::EI;
      comm->forward_comm(this);
    }

    { // solve diffusion a bit
      for(size_t j = 0; j < nlocal; ++j) {
        E_a_i[j][1] = E_a_i[j][0];
        
        if(mask[j] & groupbit) {
          int jtype = type[j];
          int *klist = firstneigh[j];
          int knum = numneigh[j];
          
          double l_dE_j {0.};
          double l_T_j = kappa.E_T_atomic[type_map_kappa[jtype - 1]].reverse(E_a_i[j][0]);
          double l_K_j = kappa.K_T_atomic[type_map_kappa[jtype - 1]](l_T_j);
          
          double const rho_j {rho_a_i[j]};
          double const rho_j_inv = {1. / rho_a_i[j]};
          
          for(size_t k = 0; k != knum; ++k) {
            int kk = klist[k];
            kk &= NEIGHMASK;
            int ktype = type[kk];
            
            if(!(mask[kk] & groupbit)) {continue;}
            
            double const rho_k {rho_a_i[kk]};
            double const rho_k_inv = {1. / rho_a_i[kk]};
            
            double l_T_k = kappa.E_T_atomic[type_map_kappa[ktype - 1]].reverse(E_a_i[kk][0]);
            double l_K_k = kappa.K_T_atomic[type_map_kappa[ktype - 1]](l_T_k);
            
            double const l_K {0.5 * (l_K_j + l_K_k)}; // we use average heat conduction
            double const v_dT {l_T_k - l_T_j};
            
            double e_jk[3];
            double e_r_sq = get_difference_sq(x[kk], x[j], e_jk);
            
            if(e_r_sq >= kappa.r_cutoff_sq) {continue;}
            
            double v_rho_j = kappa.rho_r_sq[type_map_kappa[jtype - 1]](e_r_sq);
            double v_rho_k = kappa.rho_r_sq[type_map_kappa[ktype - 1]](e_r_sq);
            
            if(rho_j > 0.) {l_dE_j += l_K * v_rho_k * rho_j_inv * v_dT;}
            if(rho_k > 0.) {l_dE_j += l_K * v_rho_j * rho_k_inv * v_dT;}
          }
          
          E_a_i[j][1] = E_a_i[j][0] + 0.5 * l_dE_j * dt;
          if(E_a_i[j][1] < 0) { 
            #ifndef NDEBUG
            std::cerr << "WARNING NEGATIVE TEMPERATURES FOR ATOM " 
              << j << ' ' << type[j]-1 << ' ' << E_a_i[j][0] << '\n';
            std::cerr << "DECREASE DT or add additional loops\n";
            #endif
            E_a_i[j][1] = 0.0; // energy cannot go negative
          }
        }
      }
    }
    
    for(size_t j = 0; j < nlocal; ++j) {
      E_a_i[j][0] = E_a_i[j][1]; // use some kind of memcpy instead
    }
  }
}

void FixEPHAtomic::post_force(int vflag) {
  double **f = atom->f;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  int *numneigh = list->numneigh;

  //zero all arrays
  std::fill_n(&(w_i[0][0]), 3 * nlocal, 0);
  std::fill_n(&(xi_i[0][0]), 3 * nlocal, 0);
  std::fill_n(&(f_EPH[0][0]), 3 * nlocal, 0);
  std::fill_n(&(f_RNG[0][0]), 3 * nlocal, 0);
  std::fill_n(&(dE_a_i[0]), nlocal, 0);

  // TODO: TEMPORARY
  state = FixState::EI;
  comm->forward_comm(this);
  // END TODO

  // generate random forces and distribute them
  if(eph_flag & Flag::RANDOM) {
    for(size_t i = 0; i < nlocal; ++i) {
      if(mask[i] & groupbit) {
        xi_i[i][0] = random->gaussian();
        xi_i[i][1] = random->gaussian();
        xi_i[i][2] = random->gaussian();
      }
    }

    state = FixState::XI;
    comm->forward_comm(this);
  }

  // calculate the site densities, gradients (future) and beta(rho)
  calculate_environment();

  state = FixState::RHO;
  comm->forward_comm(this);

  force_prl();

  // second loop over atoms if needed
  if((eph_flag & Flag::FRICTION) && !(eph_flag & Flag::NOFRICTION)) {
    for(int i = 0; i < nlocal; i++) {
      if(mask[i] & groupbit) {
        f[i][0] += f_EPH[i][0];
        f[i][1] += f_EPH[i][1];
        f[i][2] += f_EPH[i][2];
      }
    }
  }

  if((eph_flag & Flag::RANDOM) && !(eph_flag & Flag::NORANDOM)) {
    for(int i = 0; i < nlocal; i++) {
      if(mask[i] & groupbit) {
        f[i][0] += f_RNG[i][0];
        f[i][1] += f_RNG[i][1];
        f[i][2] += f_RNG[i][2];
      }
    }
  }
}

void FixEPHAtomic::reset_dt() {
  eta_factor = sqrt(2.0 * force->boltz / update->dt);

  dtv = update->dt;
  dtf = 0.5 * update->dt * force->ftm2v;
}

void FixEPHAtomic::grow_arrays(int ngrow) {
  n = ngrow;

  memory->grow(f_EPH, ngrow, 3,"eph_atomic:fEPH");
  memory->grow(f_RNG, ngrow, 3,"eph_atomic:fRNG");

  memory->grow(rho_i, ngrow, "eph:rho_i");

  memory->grow(w_i, ngrow, 3, "eph:w_i");
  memory->grow(xi_i, ngrow, 3, "eph:xi_i");

  memory->grow(rho_a_i, ngrow, "eph:rho_a_i");
  memory->grow(E_a_i, ngrow, 2, "eph:E_a_i"); // TODO: change to 2 dimensions for keeping old data
  memory->grow(dE_a_i, ngrow, "eph:dE_a_i");
  memory->grow(T_a_i, ngrow, "eph:T_a_i");

  // per atom values
  // we need only nlocal elements here
  memory->grow(array, ngrow, size_peratom_cols, "eph:array");
  array_atom = array;
}

double FixEPHAtomic::compute_vector(int i) {
  if(i == 0)
    return Ee;
  else if(i == 1) {
    return Te;
  }

  return Ee;
}

/** TODO: There might be synchronisation issues here; maybe should add barrier for sync **/
int FixEPHAtomic::pack_forward_comm(int n, int *list, double *data, int pbc_flag, int *pbc) {
  int m;
  m = 0;
  switch(state) {
    case FixState::RHO:
      for(size_t i = 0; i < n; ++i) { // TODO: things can break here in mpi
        data[m++] = rho_i[list[i]];
        data[m++] = rho_a_i[list[i]];
      }
      break;
    case FixState::XI:
      for(size_t i = 0; i < n; ++i) {
        data[m++] = xi_i[list[i]][0];
        data[m++] = xi_i[list[i]][1];
        data[m++] = xi_i[list[i]][2];
      }
      break;
    case FixState::WI:
      for(size_t i = 0; i < n; ++i) {
        data[m++] = w_i[list[i]][0];
        data[m++] = w_i[list[i]][1];
        data[m++] = w_i[list[i]][2];
      }
      break;
    case FixState::EI:
      for(size_t i = 0; i < n; ++i) {
        data[m++] = E_a_i[list[i]][0];
      }
      break;
    default:
      break;
  }

  return m;
}

void FixEPHAtomic::unpack_forward_comm(int n, int first, double *data) {
  int m, last;
  m = 0;
  last = first + n;

  switch(state) {
    case FixState::RHO:
      for(size_t i = first; i < last; ++i) {
        rho_i[i] = data[m++];
        rho_a_i[i] = data[m++];
      }
      break;
    case FixState::XI:
      for(size_t i = first; i < last; ++i) {
        xi_i[i][0] = data[m++];
        xi_i[i][1] = data[m++];
        xi_i[i][2] = data[m++];
      }
      break;
    case FixState::WI:
      for(size_t i = first; i < last; ++i) {
        w_i[i][0] = data[m++];
        w_i[i][1] = data[m++];
        w_i[i][2] = data[m++];
      }
      break;
    case FixState::EI:
      for(size_t i = first; i < last; ++i) {
        E_a_i[i][0] = data[m++];
      }
      break;
    default:
      break;
  }
}

/** TODO **/
double FixEPHAtomic::memory_usage() {
    double bytes = 0;

    return bytes;
}

/* save temperature state after run */
void FixEPHAtomic::post_run() {
  // save temperatures somehow (MPI maybe)
}

int FixEPHAtomic::pack_exchange(int i, double *buf) {
  int m = 0;
  buf[m++] = E_a_i[i][0];
  return m;
}

int FixEPHAtomic::unpack_exchange(int nlocal, double *buf) {
  int m = 0;
  E_a_i[nlocal][0] = buf[m++];
  return m;
}

void FixEPHAtomic::copy_arrays(int i, int j, int) {
  E_a_i[j][0] = E_a_i[i][0];
}
