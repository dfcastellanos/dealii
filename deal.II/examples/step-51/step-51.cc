/* ---------------------------------------------------------------------
 * $Id$
 *
 * Copyright (C) 2013 - 2013 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the deal.II distribution.
 *
 * ---------------------------------------------------------------------

 *
 * Author: Martin Kronbichler, TU Muenchen,
 *         Scott T. Miller, The Pennsylvania State University, 2013
 */

// @sect3{Include files}
//
// Most of the deal.II include files have already been covered in previous
// examples are are not commented on.
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/tensor_function.h>
#include <deal.II/base/exceptions.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/work_stream.h>
#include <deal.II/base/convergence_table.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/compressed_simple_sparsity_pattern.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/tria_boundary_lib.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>

// However, we do have a few new includes for the example.
// The first one defines finite element spaces on the faces
// of the triangulation, which we refer to as the 'skeleton'.
// These finite elements do not have any support on the element
// interior, and they represent polynomials that have a single
// value on each codimension-1 surface, but admit discontinuities
// on codimension-2 surfaces.
#include <deal.II/fe/fe_face.h>

// The second new file we include defines a new type of sparse matrix.
// The regular <code>SparseMatrix</code> type stores indices to all non-zero entries.
// The <code>ChunkSparseMatrix</code> takes advantage of the coupled nature of
// DG solutions.  It stores an index to a matrix sub-block of a specified
// size.  In the HDG context, this sub-block-size is actually the number
// of degrees of freedom per face defined by the skeleton solution field.
#include <deal.II/lac/chunk_sparse_matrix.h>

// The final new include for this example deals with data output.  Since
// we have a finite element field defined on the skeleton of the mesh,
// we would like to visualize what that solution actually is.
// DataOutFaces does exactly this; the interface is the almost the same
// as the familiar DataOut, but the output only has codimension-1 data for
// the simulation.
#include <deal.II/numerics/data_out_faces.h>

using namespace dealii;

// @sect3{Equation data}
//
// The structure of the analytic solution is the same as in step-7. There
// are two exceptions. Firstly, we also create a solution for the 3d case,
// and secondly, we take into account the convection velocity in the right
// hand side that is variable in this case.
template <int dim>
class SolutionBase
{
protected:
  static const unsigned int  n_source_centers = 3;
  static const Point<dim>    source_centers[n_source_centers];
  static const double        width;
};


template <>
const Point<1>
SolutionBase<1>::source_centers[SolutionBase<1>::n_source_centers]
= { Point<1>(-1.0 / 3.0),
    Point<1>(0.0),
    Point<1>(+1.0 / 3.0)
};


template <>
const Point<2>
SolutionBase<2>::source_centers[SolutionBase<2>::n_source_centers]
= { Point<2>(-0.5, +0.5),
    Point<2>(-0.5, -0.5),
    Point<2>(+0.5, -0.5)
};

template <>
const Point<3>
SolutionBase<3>::source_centers[SolutionBase<3>::n_source_centers]
= { Point<3>(-0.5, +0.5, 0.25),
    Point<3>(-0.6, -0.5, -0.125),
    Point<3>(+0.5, -0.5, 0.5)   };

template <int dim>
const double SolutionBase<dim>::width = 1./5.;



template <int dim>
class ConvectionVelocity : public TensorFunction<1,dim>
{
public:
  ConvectionVelocity() : TensorFunction<1,dim>() {}

  virtual Tensor<1,dim> value (const Point<dim> &p) const;
};



template <int dim>
Tensor<1,dim>
ConvectionVelocity<dim>::value(const Point<dim> &p) const
{
  Tensor<1,dim> convection;
  switch (dim)
    {
    case 1:
      convection[0] = 1;
      break;
    case 2:
      convection[0] = p[1];
      convection[1] = -p[0];
      break;
    case 3:
      convection[0] = p[1];
      convection[1] = -p[0];
      convection[2] = 1;
      break;
    default:
      Assert(false, ExcNotImplemented());
    }
  return convection;
}


template <int dim>
class Solution : public Function<dim>,
                 protected SolutionBase<dim>
{
public:
  Solution () : Function<dim>() {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;

  virtual Tensor<1,dim> gradient (const Point<dim>   &p,
                                  const unsigned int  component = 0) const;
};



template <int dim>
double Solution<dim>::value (const Point<dim>   &p,
                             const unsigned int) const
{
  double return_value = 0;
  for (unsigned int i=0; i<this->n_source_centers; ++i)
    {
      const Point<dim> x_minus_xi = p - this->source_centers[i];
      return_value += std::exp(-x_minus_xi.square() /
                               (this->width * this->width));
    }

  return return_value /
    Utilities::fixed_power<dim>(std::sqrt(2. * numbers::PI) * this->width);
}



template <int dim>
Tensor<1,dim> Solution<dim>::gradient (const Point<dim>   &p,
                                       const unsigned int) const
{
  Tensor<1,dim> return_value;

  for (unsigned int i=0; i<this->n_source_centers; ++i)
    {
      const Point<dim> x_minus_xi = p - this->source_centers[i];

      return_value += (-2 / (this->width * this->width) *
                       std::exp(-x_minus_xi.square() /
                                (this->width * this->width)) *
                       x_minus_xi);
    }

  return return_value / Utilities::fixed_power<dim>(std::sqrt(2 * numbers::PI) *
                                                    this->width);
}



template <int dim>
class SolutionAndGradient : public Function<dim>,
                            protected SolutionBase<dim>
{
public:
  SolutionAndGradient () : Function<dim>(dim) {}

  virtual void vector_value (const Point<dim>   &p,
                             Vector<double>     &v) const
  {
    AssertDimension(v.size(), dim+1);
    Solution<dim> solution;
    Tensor<1,dim> grad = solution.gradient(p);
    for (unsigned int d=0; d<dim; ++d)
      v[d] = -grad[d];
    v[dim] = solution.value(p);
  }
};



template <int dim>
class RightHandSide : public Function<dim>,
                      protected SolutionBase<dim>
{
public:
  RightHandSide () : Function<dim>() {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;

private:
  const ConvectionVelocity<dim> convection_velocity;
};


template <int dim>
double RightHandSide<dim>::value (const Point<dim>   &p,
                                  const unsigned int) const
{
  Tensor<1,dim> convection = convection_velocity.value(p);
  double return_value = 0;
  for (unsigned int i=0; i<this->n_source_centers; ++i)
    {
      const Point<dim> x_minus_xi = p - this->source_centers[i];

      return_value +=
        ((2*dim - 2*convection*x_minus_xi - 4*x_minus_xi.square()/
          (this->width * this->width)) /
         (this->width * this->width) *
         std::exp(-x_minus_xi.square() /
                  (this->width * this->width)));
    }

  return return_value / Utilities::fixed_power<dim>(std::sqrt(2 * numbers::PI)
                                                    * this->width);
}

// @sect3{The Step51 HDG solver class}

// The HDG solution procedure follows closely that of step-7.  The major
// difference is the use of 3 different sets of <code>DoFHandler</code> and FE objects,
// along with the <code>ChunkSparseMatrix</code> and the corresponding solutions vectors.

template <int dim>
class Step51
{
public:
  enum RefinementMode
    {
      global_refinement, adaptive_refinement
    };

  Step51 (const unsigned int degree,
          const RefinementMode refinement_mode);
  void run ();

private:
    
  struct PerTaskData;
  struct ScratchData;
    
  void setup_system ();
  void assemble_system (const bool reconstruct_trace = false);
  void assemble_system_one_cell (const typename DoFHandler<dim>::active_cell_iterator &cell,
                                   ScratchData &scratch,
                                   PerTaskData &task_data);
  void copy_local_to_global(const PerTaskData &data);
  void solve ();
  void postprocess ();
  void refine_grid (const unsigned int cylce);
  void output_results (const unsigned int cycle);

  Triangulation<dim>   triangulation;
  
// The 'local' solutions are interior to each element.  These
// represent the primal solution field $u$ as well as the auxiliary
// field $\mathbf{q} = \nabla u$. 
  FESystem<dim>        fe_local;
  DoFHandler<dim>      dof_handler_local;

// The new finite element type and corresponding <code>DoFHandler</code>
// are used for the global solution that couples the element-level local
// solution.
  FE_FaceQ<dim>        fe;
  DoFHandler<dim>      dof_handler;

// As stated in the introduction, HDG solutions can be post-processed to
// attain superconvegence rates of $\mathcal{O}(h^{p+2})$.
// The post-processed solution is a discontinuous finite element solution
// representing the primal variable on the interior of each cell.
// We define a FE type to represent this post-processed solution, which we
// only use for output after constructing it.
  FE_DGQ<dim>          fe_u_post;
  DoFHandler<dim>      dof_handler_u_post;

// The degrees of freedom corresponding to the skeleton strongly enforce
// Dirichlet boundary conditions, just as in a continuous Galerkin finite
// element method.  We can enforce the boundary conditions in an analogous
// manner through the use of <code>ConstrainMatrix</code> constructs.
  ConstraintMatrix     constraints;
  
  // Comment on chunk.
  ChunkSparsityPattern sparsity_pattern;
  ChunkSparseMatrix<double> system_matrix;

  // Global/skeleton solution/rhs
  Vector<double>       solution;
  Vector<double>       system_rhs;

  // Local elementwise solution
  Vector<double>       solution_local;
  
  // HDG solutions can be post-processed
  // to gain one order of accuracy.
  // <code>solution_u_post</code> will be
  // our post-processed DG solution on the
  // interior of cells represented by a 
  // DG solution of order (degree+1)
  Vector<double>       solution_u_post;

  // Same as step-7:
  const RefinementMode refinement_mode;

  ConvergenceTable     convergence_table;
};



template <int dim>
Step51<dim>::Step51 (const unsigned int degree,
                     const RefinementMode refinement_mode) :
  fe_local (FE_DGQ<dim>(degree), dim,
            FE_DGQ<dim>(degree), 1),
  dof_handler_local (triangulation),
  fe (degree),
  dof_handler (triangulation),
  fe_u_post (degree+1),
  dof_handler_u_post (triangulation),
  refinement_mode (refinement_mode)
{}

template <int dim>
struct Step51<dim>::PerTaskData
{
    FullMatrix<double> cell_matrix;
    Vector<double>     cell_vector;
    std::vector<types::global_dof_index> dof_indices;
    
    bool trace_reconstruct;
    
    PerTaskData(const unsigned int n_dofs, const bool trace_reconstruct)
    : cell_matrix(n_dofs, n_dofs),
      cell_vector(n_dofs),
      dof_indices(n_dofs),
      trace_reconstruct(trace_reconstruct)
    {}
    
    void reset(){
        cell_matrix = 0.0;
        cell_vector = 0.0;
    }
};

template <int dim>
struct Step51<dim>::ScratchData
{
    const Vector<double> &solution;
    Vector<double> &solution_local;
    
    FEValues<dim>     fe_values_local;
    FEFaceValues<dim> fe_face_values_local;
    FEFaceValues<dim> fe_face_values;
    
    FullMatrix<double> ll_matrix;
    FullMatrix<double> lf_matrix;
    FullMatrix<double> fl_matrix;
    FullMatrix<double> tmp_matrix;
    FullMatrix<double> ff_matrix;
    Vector<double>     l_rhs;
    Vector<double>     f_rhs;
    Vector<double>     tmp_rhs;
    
    std::vector<Tensor<1,dim> > q_phi;
    std::vector<double>         q_phi_div;
    std::vector<double>         u_phi;
    std::vector<Tensor<1,dim> > u_phi_grad;
    std::vector<double>         tr_phi;
    std::vector<double>         trace_values;
    
    std::vector<std::vector<unsigned int> > fe_local_support_on_face;
    std::vector<std::vector<unsigned int> > fe_support_on_face;
    
    bool trace_reconstruct;
    
    ConvectionVelocity<dim> convection_velocity;
    RightHandSide<dim> right_hand_side;
    const Solution<dim> exact_solution;
    
        // Full constructor
    ScratchData(const Vector<double> &solution,
                Vector<double> &solution_local,
                const FiniteElement<dim> &fe,
                const FiniteElement<dim> &fe_local,
                const QGauss<dim>   &quadrature_formula,
                const QGauss<dim-1> &face_quadrature_formula,
                const UpdateFlags local_flags,
                const UpdateFlags local_face_flags,
                const UpdateFlags flags,
                const bool trace_reconstruct)
      :
      solution(solution),
      solution_local(solution_local),
      fe_values_local (fe_local, quadrature_formula, local_flags),
      fe_face_values_local (fe_local, face_quadrature_formula, local_face_flags),
      fe_face_values (fe, face_quadrature_formula, flags),
      ll_matrix (fe_local.dofs_per_cell, fe_local.dofs_per_cell),
      lf_matrix (fe_local.dofs_per_cell, fe.dofs_per_cell),
      fl_matrix (fe.dofs_per_cell, fe_local.dofs_per_cell),
      tmp_matrix (fe.dofs_per_cell, fe_local.dofs_per_cell),
      ff_matrix (fe.dofs_per_cell, fe.dofs_per_cell),
      l_rhs (fe_local.dofs_per_cell),
      f_rhs (fe.dofs_per_cell),
      tmp_rhs (fe_local.dofs_per_cell),
      q_phi (fe_local.dofs_per_cell),
      q_phi_div (fe_local.dofs_per_cell),
      u_phi (fe_local.dofs_per_cell),
      u_phi_grad (fe_local.dofs_per_cell),
      tr_phi (fe.dofs_per_cell),
      trace_values(face_quadrature_formula.size()),
      fe_local_support_on_face(GeometryInfo<dim>::faces_per_cell),
      fe_support_on_face(GeometryInfo<dim>::faces_per_cell),
      trace_reconstruct (trace_reconstruct)
    {
        for (unsigned int face=0; face<GeometryInfo<dim>::faces_per_cell; ++face)
            for (unsigned int i=0; i<fe_local.dofs_per_cell; ++i) {
                if (fe_local.has_support_on_face(i,face))
                    fe_local_support_on_face[face].push_back(i);
            }
        
        for (unsigned int face=0; face<GeometryInfo<dim>::faces_per_cell; ++face)
            for (unsigned int i=0; i<fe.dofs_per_cell; ++i) {
                if (fe.has_support_on_face(i,face))
                    fe_support_on_face[face].push_back(i);
            }
    }
    
        // Copy constructor
    ScratchData(const ScratchData &sd)
      :
      solution(sd.solution),
      solution_local(sd.solution_local),
      fe_values_local (sd.fe_values_local.get_fe(),
                       sd.fe_values_local.get_quadrature(),
                       sd.fe_values_local.get_update_flags()),
      fe_face_values_local (sd.fe_face_values_local.get_fe(),
                            sd.fe_face_values_local.get_quadrature(),
                            sd.fe_face_values_local.get_update_flags()),
      fe_face_values (sd.fe_face_values.get_fe(),
                      sd.fe_face_values.get_quadrature(),
                      sd.fe_face_values.get_update_flags()),
      ll_matrix (sd.ll_matrix),
      lf_matrix (sd.lf_matrix),
      fl_matrix (sd.fl_matrix),
      tmp_matrix (sd.tmp_matrix),
      ff_matrix (sd.ff_matrix),
      l_rhs (sd.l_rhs),
      f_rhs (sd.f_rhs),
      tmp_rhs (sd.tmp_rhs),
      q_phi (sd.q_phi),
      q_phi_div (sd.q_phi_div),
      u_phi (sd.u_phi),
      u_phi_grad (sd.u_phi_grad),
      tr_phi (sd.tr_phi),
      trace_values(sd.trace_values),
      fe_local_support_on_face(sd.fe_local_support_on_face),
      fe_support_on_face(sd.fe_support_on_face),
      trace_reconstruct(sd.trace_reconstruct)
    {}
    
    void reset(){
        ll_matrix  = 0.0;
        lf_matrix  = 0.0;
        fl_matrix  = 0.0;
        tmp_matrix = 0.0;
        ff_matrix  = 0.0;
        l_rhs      = 0.0;
        f_rhs      = 0.0;
        tmp_rhs    = 0.0;
        
        for (int i=0; i<q_phi.size(); ++i){
            q_phi[i]   = 0.0;
            q_phi_div  = 0.0;
            u_phi      = 0.0;
            u_phi_grad = 0.0;
        }
        
        for (int i=0; i<tr_phi.size(); ++i)
            tr_phi[i] = 0.0;
        
        for (int i=0; i<trace_values.size(); ++i)
            trace_values[i] = 0.0;
    }
};

template <int dim>
void Step51<dim>::copy_local_to_global(const PerTaskData &data)
{
    if(data.trace_reconstruct == false)
      constraints.distribute_local_to_global (data.cell_matrix,
                                              data.cell_vector,
                                              data.dof_indices,
                                              system_matrix, system_rhs);
}

template <int dim>
void
Step51<dim>::setup_system ()
{
  dof_handler_local.distribute_dofs(fe_local);
  dof_handler.distribute_dofs(fe);
  dof_handler_u_post.distribute_dofs(fe_u_post);

  std::cout << "   Number of degrees of freedom: "
            << dof_handler.n_dofs()
            << std::endl;

  solution.reinit (dof_handler.n_dofs());
  system_rhs.reinit (dof_handler.n_dofs());

  solution_local.reinit (dof_handler_local.n_dofs());
  solution_u_post.reinit (dof_handler_u_post.n_dofs());

  constraints.clear ();
  DoFTools::make_hanging_node_constraints (dof_handler, constraints);
  typename FunctionMap<dim>::type boundary_functions;
  Solution<dim> solution_function;
  boundary_functions[0] = &solution_function;
  VectorTools::project_boundary_values (dof_handler,
                                        boundary_functions,
                                        QGauss<dim-1>(fe.degree+1),
                                        constraints);
  constraints.close ();

  {
    CompressedSimpleSparsityPattern csp (dof_handler.n_dofs());
    DoFTools::make_sparsity_pattern (dof_handler, csp,
                                     constraints, false);
    sparsity_pattern.copy_from(csp, fe.dofs_per_face);
  }
  system_matrix.reinit (sparsity_pattern);
}

template <int dim>
void
Step51<dim>::assemble_system (const bool trace_reconstruct)
{
    const QGauss<dim>   quadrature_formula(fe.degree+1);
    const QGauss<dim-1> face_quadrature_formula(fe.degree+1);
    
    const UpdateFlags local_flags (update_values | update_gradients |
                                   update_JxW_values | update_quadrature_points);
    
    const UpdateFlags local_face_flags (update_values);
    
    const UpdateFlags flags ( update_values | update_normal_vectors |
                             update_quadrature_points |
                             update_JxW_values);
    
    PerTaskData task_data (fe.dofs_per_cell,
                           trace_reconstruct);
    ScratchData scratch (solution,
                         solution_local,
                         fe, fe_local,
                         quadrature_formula,
                         face_quadrature_formula,
                         local_flags,
                         local_face_flags,
                         flags,
                         trace_reconstruct);
    
    WorkStream::run(dof_handler.begin_active(),
                    dof_handler.end(),
                    *this,
                    &Step51<dim>::assemble_system_one_cell,
                    &Step51<dim>::copy_local_to_global,
                    scratch,
                    task_data);
}


template <int dim>
void
Step51<dim>::assemble_system_one_cell (const typename DoFHandler<dim>::active_cell_iterator &cell,
                                       ScratchData &scratch,
                                       PerTaskData &task_data)
{
        // Construct iterator for dof_handler_local
  typename DoFHandler<dim>::active_cell_iterator
    loc_cell (&triangulation,
              cell->level(),
              cell->index(),
              &dof_handler_local);

  const unsigned int n_q_points    = scratch.fe_values_local.get_quadrature().size();
  const unsigned int n_face_q_points = scratch.fe_face_values_local.get_quadrature().size();

        //  const unsigned int dofs_per_cell = scratch.fe_face_values.get_fe().dofs_per_cell;
  const unsigned int loc_dofs_per_cell = scratch.fe_values_local.get_fe().dofs_per_cell;

  // Choose stabilization parameter to be 5 * diffusion = 5
  const double tau_stab_diffusion = 5.;

  const FEValuesExtractors::Vector fluxes (0);
  const FEValuesExtractors::Scalar scalar (dim);

      scratch.ll_matrix = 0;
      scratch.l_rhs = 0;
      if (!scratch.trace_reconstruct)
        {
          scratch.lf_matrix = 0;
          scratch.fl_matrix = 0;
          scratch.ff_matrix = 0;
          scratch.f_rhs = 0;
        }
      scratch.fe_values_local.reinit (loc_cell);

      for (unsigned int q=0; q<n_q_points; ++q)
        {
          const double rhs_value
            = scratch.right_hand_side.value(scratch.fe_values_local.quadrature_point(q));
          const Tensor<1,dim> convection
            = scratch.convection_velocity.value(scratch.fe_values_local.quadrature_point(q));
          const double JxW = scratch.fe_values_local.JxW(q);
          for (unsigned int k=0; k<loc_dofs_per_cell; ++k)
            {
              scratch.q_phi[k] = scratch.fe_values_local[fluxes].value(k,q);
              scratch.q_phi_div[k] = scratch.fe_values_local[fluxes].divergence(k,q);
              scratch.u_phi[k] = scratch.fe_values_local[scalar].value(k,q);
              scratch.u_phi_grad[k] = scratch.fe_values_local[scalar].gradient(k,q);
            }
          for (unsigned int i=0; i<loc_dofs_per_cell; ++i)
            {
              for (unsigned int j=0; j<loc_dofs_per_cell; ++j)
                scratch.ll_matrix(i,j) += (
                                   scratch.q_phi[i] * scratch.q_phi[j]
                                   -
                                   scratch.q_phi_div[i] * scratch.u_phi[j]
                                   +
                                   scratch.u_phi[i] * scratch.q_phi_div[j]
                                   -
                                   (scratch.u_phi_grad[i] * convection) * scratch.u_phi[j]
                                   ) * JxW;
              scratch.l_rhs(i) += scratch.u_phi[i] * rhs_value * JxW;
            }
        }

      for (unsigned int face=0; face<GeometryInfo<dim>::faces_per_cell; ++face)
        {
          scratch.fe_face_values_local.reinit(loc_cell, face);
          scratch.fe_face_values.reinit(cell, face);
          if (scratch.trace_reconstruct)
            scratch.fe_face_values.get_function_values (scratch.solution, scratch.trace_values);

          for (unsigned int q=0; q<n_face_q_points; ++q)
            {
              const double JxW = scratch.fe_face_values.JxW(q);
              const Point<dim> normal = scratch.fe_face_values.normal_vector(q);
              const Tensor<1,dim> convection
                = scratch.convection_velocity.value(scratch.fe_face_values.quadrature_point(q));
              const double tau_stab = (tau_stab_diffusion +
                                       std::abs(convection * normal));

              for (unsigned int k=0; k<scratch.fe_local_support_on_face[face].size(); ++k)
                {
                  const unsigned int kk=scratch.fe_local_support_on_face[face][k];
                  scratch.q_phi[k] = scratch.fe_face_values_local[fluxes].value(kk,q);
                  scratch.u_phi[k] = scratch.fe_face_values_local[scalar].value(kk,q);
                }

              if (!scratch.trace_reconstruct)
                {
                  for (unsigned int k=0; k<scratch.fe_support_on_face[face].size(); ++k)
                    scratch.tr_phi[k] =
                      scratch.fe_face_values.shape_value(scratch.fe_support_on_face[face][k],q);
                  for (unsigned int i=0; i<scratch.fe_local_support_on_face[face].size(); ++i)
                    for (unsigned int j=0; j<scratch.fe_support_on_face[face].size(); ++j)
                      {
                        const unsigned int ii=scratch.fe_local_support_on_face[face][i];
                        const unsigned int jj=scratch.fe_support_on_face[face][j];
                        scratch.lf_matrix(ii,jj) += (
                                             (scratch.q_phi[i] * normal
                                              +
                                              (convection * normal -
                                               tau_stab) * scratch.u_phi[i])
                                             * scratch.tr_phi[j]
                                             ) * JxW;
                        scratch.fl_matrix(jj,ii) -= (
                                             (scratch.q_phi[i] * normal
                                              +
                                              tau_stab * scratch.u_phi[i])
                                             * scratch.tr_phi[j]
                                             ) * JxW;
                      }

                  for (unsigned int i=0; i<scratch.fe_support_on_face[face].size(); ++i)
                    for (unsigned int j=0; j<scratch.fe_support_on_face[face].size(); ++j)
                      {
                        const unsigned int ii=scratch.fe_support_on_face[face][i];
                        const unsigned int jj=scratch.fe_support_on_face[face][j];
                        scratch.ff_matrix(ii,jj) += (
                                             (convection * normal - tau_stab) *
                                             scratch.tr_phi[i] * scratch.tr_phi[j]
                                             ) * JxW;
                      }

                  if (cell->face(face)->at_boundary()
                      &&
                      (cell->face(face)->boundary_indicator() == 1))
                    {
                      const double neumann_value =
                        scratch.exact_solution.value(scratch.fe_face_values.quadrature_point(q));
                      for (unsigned int i=0; i<scratch.fe_support_on_face[face].size(); ++i)
                        {
                          const unsigned int ii=scratch.fe_support_on_face[face][i];
                          scratch.f_rhs(ii) -= scratch.tr_phi[i] * neumann_value * JxW;
                        }
                    }
                }

              for (unsigned int i=0; i<scratch.fe_local_support_on_face[face].size(); ++i)
                for (unsigned int j=0; j<scratch.fe_local_support_on_face[face].size(); ++j)
                  {
                    const unsigned int ii=scratch.fe_local_support_on_face[face][i];
                    const unsigned int jj=scratch.fe_local_support_on_face[face][j];
                    scratch.ll_matrix(ii,jj) += tau_stab * scratch.u_phi[i] * scratch.u_phi[j] * JxW;
                  }

              // compute the local right hand side contributions from trace
              if (scratch.trace_reconstruct)
                for (unsigned int i=0; i<scratch.fe_local_support_on_face[face].size(); ++i)
                  {
                    const unsigned int ii=scratch.fe_local_support_on_face[face][i];
                    scratch.l_rhs(ii) -= (scratch.q_phi[i] * normal
                                  +
                                  scratch.u_phi[i] * (convection * normal - tau_stab)
                                  ) * scratch.trace_values[q] * JxW;
                  }
            }
        }

      scratch.ll_matrix.gauss_jordan();
      if (scratch.trace_reconstruct == false)
        {
          scratch.fl_matrix.mmult(scratch.tmp_matrix, scratch.ll_matrix);
          scratch.tmp_matrix.vmult_add(scratch.f_rhs, scratch.l_rhs);
          scratch.tmp_matrix.mmult(scratch.ff_matrix, scratch.lf_matrix, true);
          cell->get_dof_indices(task_data.dof_indices);
          task_data.cell_matrix = scratch.ff_matrix;
          task_data.cell_vector = scratch.f_rhs;
        }
      else
        {
          scratch.ll_matrix.vmult(scratch.tmp_rhs, scratch.l_rhs);
          loc_cell->set_dof_values(scratch.tmp_rhs, scratch.solution_local);
        }
}



template <int dim>
void Step51<dim>::solve ()
{
  SolverControl solver_control (system_matrix.m()*10,
                                1e-10*system_rhs.l2_norm());
  SolverGMRES<> solver (solver_control, 50);
  solver.solve (system_matrix, solution, system_rhs,
                PreconditionIdentity());

  std::cout << "   Number of GMRES iterations: " << solver_control.last_step()
            << std::endl;

  system_matrix.clear();
  sparsity_pattern.reinit(0,0,0,1);
    
  constraints.distribute(solution);

  // update local values
  assemble_system(true);
}



template <int dim>
void
Step51<dim>::postprocess()
{
  const unsigned int n_active_cells=triangulation.n_active_cells();
  Vector<float> difference_per_cell (triangulation.n_active_cells());

  ComponentSelectFunction<dim> value_select (dim, dim+1);
  VectorTools::integrate_difference (dof_handler_local,
                                     solution_local,
                                     SolutionAndGradient<dim>(),
                                     difference_per_cell,
                                     QGauss<dim>(fe.degree+2),
                                     VectorTools::L2_norm,
                                     &value_select);
  const double L2_error = difference_per_cell.l2_norm();

  ComponentSelectFunction<dim> gradient_select (std::pair<unsigned int,unsigned int>(0, dim),
                                                dim+1);
  VectorTools::integrate_difference (dof_handler_local,
                                     solution_local,
                                     SolutionAndGradient<dim>(),
                                     difference_per_cell,
                                     QGauss<dim>(fe.degree+2),
                                     VectorTools::L2_norm,
                                     &gradient_select);
  const double grad_error = difference_per_cell.l2_norm();

  convergence_table.add_value("cells", n_active_cells);
  convergence_table.add_value("dofs", dof_handler.n_dofs());
  convergence_table.add_value("val L2", L2_error);
  convergence_table.add_value("grad L2", grad_error);

  // construct post-processed solution with (hopefully) higher order of
  // accuracy
  QGauss<dim> quadrature(fe_u_post.degree+1);
  FEValues<dim> fe_values(fe_u_post, quadrature,
                          update_values | update_JxW_values |
                          update_gradients);

  const unsigned int n_q_points = quadrature.size();
  std::vector<double> u_values(n_q_points);
  std::vector<Tensor<1,dim> > u_gradients(n_q_points);
  FEValuesExtractors::Vector fluxes(0);
  FEValuesExtractors::Scalar scalar(dim);
  FEValues<dim> fe_values_local(fe_local, quadrature, update_values);
  FullMatrix<double> cell_matrix(fe_u_post.dofs_per_cell,
                                 fe_u_post.dofs_per_cell);
  Vector<double> cell_rhs(fe_u_post.dofs_per_cell);
  Vector<double> cell_sol(fe_u_post.dofs_per_cell);

  typename DoFHandler<dim>::active_cell_iterator
    cell_loc = dof_handler_local.begin_active(),
    cell = dof_handler_u_post.begin_active(),
    endc = dof_handler_u_post.end();
  for ( ; cell != endc; ++cell, ++cell_loc)
    {
      fe_values.reinit(cell);
      fe_values_local.reinit(cell_loc);

      fe_values_local[scalar].get_function_values(solution_local, u_values);
      fe_values_local[fluxes].get_function_values(solution_local, u_gradients);
      for (unsigned int i=1; i<fe_u_post.dofs_per_cell; ++i)
        {
          for (unsigned int j=0; j<fe_u_post.dofs_per_cell; ++j)
            {
              double sum = 0;
              for (unsigned int q=0; q<quadrature.size(); ++q)
                sum += (fe_values.shape_grad(i,q) *
                        fe_values.shape_grad(j,q)
                        ) * fe_values.JxW(q);
              cell_matrix(i,j) = sum;
            }
          double sum = 0;
          for (unsigned int q=0; q<quadrature.size(); ++q)
            sum -= (fe_values.shape_grad(i,q) * u_gradients[q]
                    ) * fe_values.JxW(q);
          cell_rhs(i) = sum;
        }
      for (unsigned int j=0; j<fe_u_post.dofs_per_cell; ++j)
        {
          double sum = 0;
          for (unsigned int q=0; q<quadrature.size(); ++q)
            sum += fe_values.shape_value(j,q) * fe_values.JxW(q);
          cell_matrix(0,j) = sum;
        }
      {
        double sum = 0;
        for (unsigned int q=0; q<quadrature.size(); ++q)
          sum += u_values[q] * fe_values.JxW(q);
        cell_rhs(0) = sum;
      }

      cell_matrix.gauss_jordan();
      cell_matrix.vmult(cell_sol, cell_rhs);
      cell->distribute_local_to_global(cell_sol, solution_u_post);
    }

  VectorTools::integrate_difference (dof_handler_u_post,
                                     solution_u_post,
                                     Solution<dim>(),
                                     difference_per_cell,
                                     QGauss<dim>(fe.degree+3),
                                     VectorTools::L2_norm);
  double post_error = difference_per_cell.l2_norm();
  convergence_table.add_value("val L2-post", post_error);
}



template <int dim>
void Step51<dim>::output_results (const unsigned int cycle)
{
  std::string filename;
  switch (refinement_mode)
    {
    case global_refinement:
      filename = "solution-global";
      break;
    case adaptive_refinement:
      filename = "solution-adaptive";
      break;
    default:
      Assert (false, ExcNotImplemented());
    }
    
  std::string face_out(filename);
  face_out += "-face";

  filename += "-q" + Utilities::int_to_string(fe.degree,1);
  filename += "-" + Utilities::int_to_string(cycle,2);
  filename += ".vtk";
  std::ofstream output (filename.c_str());

  DataOut<dim> data_out;
  std::vector<std::string> names (dim, "gradient");
  names.push_back ("solution");
  std::vector<DataComponentInterpretation::DataComponentInterpretation>
    component_interpretation
    (dim+1, DataComponentInterpretation::component_is_part_of_vector);
  component_interpretation[dim]
    = DataComponentInterpretation::component_is_scalar;
  data_out.add_data_vector (dof_handler_local, solution_local,
                            names, component_interpretation);
                            
  // Post-processed solution:  can now add more than 1 dof_handler to 
  // the DataOut object!
  std::vector<std::string> post_name(1,"u_post");
  std::vector<DataComponentInterpretation::DataComponentInterpretation> 
			 post_comp_type(1, DataComponentInterpretation::component_is_scalar);
  data_out.add_data_vector (dof_handler_u_post, solution_u_post,
						  post_name, post_comp_type);

  data_out.build_patches (fe.degree);
  data_out.write_vtk (output);
    
  face_out += "-q" + Utilities::int_to_string(fe.degree,1);
  face_out += "-" + Utilities::int_to_string(cycle,2);
  face_out += ".vtk";
  std::ofstream face_output (face_out.c_str());

  DataOutFaces<dim> data_out_face(false);
  std::vector<std::string> face_name(1,"lambda");
  std::vector<DataComponentInterpretation::DataComponentInterpretation> 
			 face_component_type(1, DataComponentInterpretation::component_is_scalar);

  data_out_face.add_data_vector (dof_handler, 
							  	solution, 
								face_name,
								face_component_type);
								
  data_out_face.build_patches (fe.degree);
  data_out_face.write_vtk (face_output);
}



template <int dim>
void Step51<dim>::refine_grid (const unsigned int cycle)
{
  if (cycle == 0)
    {
      GridGenerator::subdivided_hyper_cube (triangulation, 2, -1, 1);
    }
  else
    switch (refinement_mode)
      {
      case global_refinement:
        {
            triangulation.clear();
            GridGenerator::subdivided_hyper_cube (triangulation, 2+(cycle%2), -1, 1);
            triangulation.refine_global(3-dim+cycle/2);
          break;
        }

      case adaptive_refinement:
      {
        Vector<float> estimated_error_per_cell (triangulation.n_active_cells());

        FEValuesExtractors::Scalar scalar(dim);
        typename FunctionMap<dim>::type neumann_boundary;
        KellyErrorEstimator<dim>::estimate (dof_handler_local,
                                            QGauss<dim-1>(3),
                                            neumann_boundary,
                                            solution_local,
                                            estimated_error_per_cell,
                                            fe_local.component_mask(scalar));

        GridRefinement::refine_and_coarsen_fixed_number (triangulation,
                                                         estimated_error_per_cell,
                                                         0.3, 0.);

        triangulation.execute_coarsening_and_refinement ();

        break;
      }

      default:
      {
        Assert (false, ExcNotImplemented());
      }
      }
  }





template <int dim>
void Step51<dim>::run ()
{
  for (unsigned int cycle=0; cycle<10; ++cycle)
    {
      std::cout << "Cycle " << cycle << ':' << std::endl;
      
      refine_grid (cycle);
      setup_system ();
      assemble_system (false);
      solve ();
      postprocess();
      output_results (cycle);
    }



  convergence_table.set_precision("val L2", 3);
  convergence_table.set_scientific("val L2", true);
  convergence_table.set_precision("grad L2", 3);
  convergence_table.set_scientific("grad L2", true);
  convergence_table.set_precision("val L2-post", 3);
  convergence_table.set_scientific("val L2-post", true);

  convergence_table
    .evaluate_convergence_rates("val L2", "cells", ConvergenceTable::reduction_rate_log2, dim);
  convergence_table
    .evaluate_convergence_rates("grad L2", "cells", ConvergenceTable::reduction_rate_log2, dim);
  convergence_table
    .evaluate_convergence_rates("val L2-post", "cells", ConvergenceTable::reduction_rate_log2, dim);
  convergence_table.write_text(std::cout);
}


int main (int argc, char** argv)
{
  const unsigned int dim = 2;

  try
    {
      using namespace dealii;

      deallog.depth_console (0);

      // Now for the three calls to the main class in complete analogy to
      // step-7.
      {
        std::cout << "Solving with Q1 elements, adaptive refinement" << std::endl
                  << "=============================================" << std::endl
                  << std::endl;

        Step51<dim> hdg_problem (1, Step51<dim>::adaptive_refinement);
        hdg_problem.run ();

        std::cout << std::endl;
      }

      {
        std::cout << "Solving with Q1 elements, global refinement" << std::endl
                  << "===========================================" << std::endl
                  << std::endl;

        Step51<dim> hdg_problem (1, Step51<dim>::global_refinement);
        hdg_problem.run ();

        std::cout << std::endl;
      }

      {
        std::cout << "Solving with Q3 elements, global refinement" << std::endl
                  << "===========================================" << std::endl
                  << std::endl;

        Step51<dim> hdg_problem (3, Step51<dim>::global_refinement);
        hdg_problem.run ();

        std::cout << std::endl;
      }

    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }

  return 0;
}
