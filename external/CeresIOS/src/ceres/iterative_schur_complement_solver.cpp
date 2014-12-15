// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2010, 2011, 2012 Google Inc. All rights reserved.
// http://code.google.com/p/ceres-solver/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: sameeragarwal@google.com (Sameer Agarwal)

#include "ceres/iterative_schur_complement_solver.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Eigen/Dense"
#include "ceres/block_sparse_matrix.h"
#include "ceres/block_structure.h"
#include "ceres/conjugate_gradients_solver.h"
#include "ceres/implicit_schur_complement.h"
#include "ceres/internal/eigen.h"
#include "ceres/internal/scoped_ptr.h"
#include "ceres/linear_solver.h"
#include "ceres/preconditioner.h"
#include "ceres/schur_jacobi_preconditioner.h"
#include "ceres/triplet_sparse_matrix.h"
#include "ceres/types.h"
#include "ceres/visibility_based_preconditioner.h"
#include "ceres/wall_time.h"
#include "glog/logging.h"

namespace ceres {
namespace internal {

IterativeSchurComplementSolver::IterativeSchurComplementSolver(
    const LinearSolver::Options& options)
    : options_(options) {
}

IterativeSchurComplementSolver::~IterativeSchurComplementSolver() {
}

LinearSolver::Summary IterativeSchurComplementSolver::SolveImpl(
    BlockSparseMatrixBase* A,
    const double* b,
    const LinearSolver::PerSolveOptions& per_solve_options,
    double* x) {
  EventLogger event_logger("IterativeSchurComplementSolver::Solve");

  CHECK_NOTNULL(A->block_structure());

  // Initialize a ImplicitSchurComplement object.
  if (schur_complement_ == NULL) {
    schur_complement_.reset(
        new ImplicitSchurComplement(options_.elimination_groups[0],
                                    options_.preconditioner_type == JACOBI));
  }
  schur_complement_->Init(*A, per_solve_options.D, b);

  // Initialize the solution to the Schur complement system to zero.
  //
  // TODO(sameeragarwal): There maybe a better initialization than an
  // all zeros solution. Explore other cheap starting points.
  reduced_linear_system_solution_.resize(schur_complement_->num_rows());
  reduced_linear_system_solution_.setZero();

  // Instantiate a conjugate gradient solver that runs on the Schur complement
  // matrix with the block diagonal of the matrix F'F as the preconditioner.
  LinearSolver::Options cg_options;
  cg_options.max_num_iterations = options_.max_num_iterations;
  ConjugateGradientsSolver cg_solver(cg_options);
  LinearSolver::PerSolveOptions cg_per_solve_options;

  cg_per_solve_options.r_tolerance = per_solve_options.r_tolerance;
  cg_per_solve_options.q_tolerance = per_solve_options.q_tolerance;

  Preconditioner::Options preconditioner_options;
  preconditioner_options.type = options_.preconditioner_type;
  preconditioner_options.sparse_linear_algebra_library =
      options_.sparse_linear_algebra_library;
  preconditioner_options.use_block_amd = options_.use_block_amd;
  preconditioner_options.num_threads = options_.num_threads;
  preconditioner_options.row_block_size = options_.row_block_size;
  preconditioner_options.e_block_size = options_.e_block_size;
  preconditioner_options.f_block_size = options_.f_block_size;
  preconditioner_options.elimination_groups = options_.elimination_groups;

  switch (options_.preconditioner_type) {
    case IDENTITY:
      break;
    case JACOBI:
      preconditioner_.reset(
          new SparseMatrixPreconditionerWrapper(
              schur_complement_->block_diagonal_FtF_inverse()));
      break;
    case SCHUR_JACOBI:
      if (preconditioner_.get() == NULL) {
        preconditioner_.reset(
            new SchurJacobiPreconditioner(
                *A->block_structure(), preconditioner_options));
      }
      break;
    case CLUSTER_JACOBI:
    case CLUSTER_TRIDIAGONAL:
      if (preconditioner_.get() == NULL) {
        preconditioner_.reset(
            new VisibilityBasedPreconditioner(
                *A->block_structure(), preconditioner_options));
      }
      break;
    default:
      LOG(FATAL) << "Unknown Preconditioner Type";
  }

  bool preconditioner_update_was_successful = true;
  if (preconditioner_.get() != NULL) {
    preconditioner_update_was_successful =
        preconditioner_->Update(*A, per_solve_options.D);
    cg_per_solve_options.preconditioner = preconditioner_.get();
  }

  event_logger.AddEvent("Setup");

  LinearSolver::Summary cg_summary;
  cg_summary.num_iterations = 0;
  cg_summary.termination_type = FAILURE;

  if (preconditioner_update_was_successful) {
    cg_summary = cg_solver.Solve(schur_complement_.get(),
                                 schur_complement_->rhs().data(),
                                 cg_per_solve_options,
                                 reduced_linear_system_solution_.data());
    if (cg_summary.termination_type != FAILURE) {
      schur_complement_->BackSubstitute(
          reduced_linear_system_solution_.data(), x);
    }
  }

  VLOG(2) << "CG Iterations : " << cg_summary.num_iterations;

  event_logger.AddEvent("Solve");
  return cg_summary;
}

}  // namespace internal
}  // namespace ceres