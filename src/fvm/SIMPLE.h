#pragma once

#include "fvm/CFDSolver.h"

#include <memory>

namespace fvm {

// Steady driver of the CFD solver: the SIMPLE/PIMPLE algorithm on the
// CFDSolver context. All transient paths (time loop, PISO, BDF2, adaptive
// dt, PVD output) live in TransientDriver, driven from solveTransient().
template<std::size_t Dim>
class SIMPLE : public CFDSolver<Dim> {
public:
    // The config/status/restart types live on the base; re-export them for
    // API compatibility.
    using Config = CFDSolver<Dim>::Config;
    using Status = CFDSolver<Dim>::Status;
    using Failure = CFDSolver<Dim>::Failure;
    using RestartState = CFDSolver<Dim>::RestartState;
    using VelocityBCType = CFDSolver<Dim>::VelocityBCType;
    using VelocityBC = CFDSolver<Dim>::VelocityBC;
    using VelocityPatch = CFDSolver<Dim>::VelocityPatch;
    using FaceVelocityBC = CFDSolver<Dim>::FaceVelocityBC;

    // --- Public API (shape unchanged from the previous SIMPLE) ---
    SIMPLE(const Mesh<Dim>& mesh, double rho, double nu, const Config& config = Config{});

    void setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                      const std::string& typeStr,
                                      double u, double v, double w,
                                      bool slip,
                                      double pressure = 0.0);

    void setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                       double val1 = 0.0, double val2 = 0.0, double val3 = 0.0);
    void setMin(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);
    void setMax(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    void addVelocityPatch(const std::string& name, std::size_t d, int sign,
                          VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    const VelocityPatch* findVelocityPatch(const std::string& name) const;
    const std::vector<VelocityPatch>& velocityPatches() const;

    void setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                              VelocityBCType type,
                              double u = 0.0, double v = 0.0, double w = 0.0);

    void setPressureOutlet(std::size_t d, int sign, double pressure);
    void setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices, double pressure);
    void setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices, double pressure,
                                              bool suppressBackflow, double u, double v, double w,
                                              double k, double omega);

    void enableTurbulence(double kInf = 1e-3, double omegaInf = 1.0,
                          double alphaK = 0.8, double alphaOmega = 0.8,
                          bool wallFunction = false);

    void solve(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void solveTransient(VectorField<Dim>& velocity, Field<Dim>& pressure);

    // --- Transient-side read access — the driver owns this state. ---
    double currentTime() const { return this->transientDriver_->time(); }
    double currentDt() const { return this->transientDriver_->dt(); }
    double currentDtPrev() const { return this->transientDriver_->dtPrev(); }
    int currentStep() const { return this->transientDriver_->step(); }
    bool hasTransientState() const {
        return this->hasRestoredTransientState_ || this->transientDriver_->step() > 0 ||
               this->transientDriver_->time() > 0.0;
    }
    const VectorField<Dim>& velocityNM1() const { return this->transientDriver_->velocityNM1(); }
    const Field<Dim>& kNM1() const { return this->transientDriver_->kN(); }
    const Field<Dim>& omegaNM1() const { return this->transientDriver_->omegaN(); }
};

extern template class SIMPLE<2>;
extern template class SIMPLE<3>;

} // namespace fvm
