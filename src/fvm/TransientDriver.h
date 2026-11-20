#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Momentum.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class CFDSolver;

// TransientDriver runs the transient SIMPLE/PISO/PIMPLE path on a CFDSolver
// (the steady driver): the time loop, adaptive dt, the BDF2 history, the
// PISO predictor/correctors, and the checkpoint/PVD output. All of the
// time-dependent half of the solver lives in this module; bug fixes to the
// transient path land here instead of in the steady loop.
template<std::size_t Dim>
class TransientDriver {
public:
    explicit TransientDriver(CFDSolver<Dim>& solver) : solver_(solver) {}

    void run(VectorField<Dim>& velocity, Field<Dim>& pressure);

    double time() const { return time_; }
    double dt() const { return dt_; }
    double dtPrev() const { return dtPrev_; }
    int step() const { return step_; }

    const VectorField<Dim>& velocityN() const { return velocityN_; }
    const VectorField<Dim>& velocityNM1() const { return velocityNM1_; }
    const Field<Dim>& kN() const { return kN_; }
    const Field<Dim>& omegaN() const { return omegaN_; }
    const Field<Dim>& kNM1() const { return kNM1_; }
    const Field<Dim>& omegaNM1() const { return omegaNM1_; }

private:
    // CFDSolver applies RestartState to driver-owned state in initializeSolve_.
    friend class CFDSolver<Dim>;

    // Driver methods, operating on the shared context through solver_.
    // One momentum solve for transient callers: SIMPLE::solveMomentum_ has
    // been absorbed into the shared CFDSolver::solveMomentumComponent_.
    void solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    // TransientParams for one momentum component, from driver state.
    TransientParams<Dim> momentumTp_(std::size_t d) const;
    void performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void updatePisoPredictor_();
    double computeAdaptiveDt_() const;
    void saveTransientCheckpoint_(int step, double time);

    CFDSolver<Dim>& solver_;
    double time_ = 0.0;
    double dt_ = 0.0;
    double dtPrev_ = 0.0;
    int step_ = 0;
    // BDF2 history.
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
