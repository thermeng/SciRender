#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLETypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class SIMPLE;

// TransientDriver owns the transient SIMPLE/PISO/PIMPLE path: the time loop,
// adaptive dt, the BDF2 history, the PISO predictor/correctors, and the
// checkpoint/PVD output. SIMPLE is the steady-algorithm facade and delegates
// here from solveTransient(); bug fixes to the transient path now live in
// this module.
template<std::size_t Dim>
class TransientDriver {
public:
    explicit TransientDriver(SIMPLE<Dim>& simple) : simple_(simple) {}

    // Runs the transient path until the stopping policy is met.
    void run(VectorField<Dim>& velocity, Field<Dim>& pressure);

    // Current simulated time; 0 until the first step commits.
    double time() const { return time_; }
    double dt() const { return dt_; }
    double dtPrev() const { return dtPrev_; }

private:
    friend class SIMPLE<Dim>;

    // The transient momentum solves, moved here from SIMPLE — this module
    // owns the whole transient path, so its momentum assembly lives with it.
    void solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    void solveMomentumComponentTransient_(std::size_t d, VectorField<Dim>& uStar,
                                          const VectorField<Dim>& velocityOld,
                                          const std::array<Field<Dim>, Dim>& pressureGrad);

    void performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void updatePisoPredictor_();
    double computeAdaptiveDt_() const;
    void saveTransientCheckpoint_(int step, double time);
    void printTransientOutput_(int step, double time);

    SIMPLE<Dim>& simple_;
    double time_ = 0.0;
    double dt_ = 0.0;
    // Previous step size. BDF2 coefficients are derived from dt_/dtPrev_; 0
    // means "no usable history" and forces explicit Euler for this step.
    double dtPrev_ = 0.0;
    // BDF2 history (roll-forward happens at the end of every step).
    VectorField<Dim> velocityN_;
    VectorField<Dim> velocityNM1_;
    Field<Dim> pressureN_;
    Field<Dim> kN_;
    Field<Dim> omegaN_;
    Field<Dim> kNM1_;
    Field<Dim> omegaNM1_;
    // PISO residual-reduction state.
    double pisoInitialResidual_ = 0.0;
    std::vector<std::string> pvdEntries_;
    std::string pvdBaseName_;
};

extern template class TransientDriver<2>;
extern template class TransientDriver<3>;

} // namespace fvm
