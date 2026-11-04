#pragma once

#include "fvm/SIMPLETypes.h"
#include "mesh/Mesh.h"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstddef>

namespace fvm {

struct BCSpec {
    std::vector<VelocityBC> velBcs;
    std::vector<VelocityPatch> velPatches;
    std::unordered_map<std::size_t, FaceVelocityBC> faceVelBCs;
};

// A single face's boundary condition: replaces the five parallel arrays
// (faceBC_, faceHasBC_, isPressureDirichletFace_, pressureOutletFaces_,
// isPressureNeumannFace_) that had to stay aligned by convention.
//
// We store FaceVelocityBC directly (it carries type, u, v, w, pressureValue,
// suppressBackflow, ...) and derive the pressure flags on the fly:
//   - pressureDirichlet  <=> type == PressureOutlet
//   - pressureNeumann    <=> type != PressureOutlet
struct BCDiscretization {
    // Cell-level data (kept as per-dimension arrays for now).
    std::vector<std::vector<char>> dirichletMask_;
    std::vector<std::vector<double>> dirichletVals_;
    std::vector<std::vector<char>> slipWallMask_;
    std::vector<char> isPressureNeumann_;
    std::vector<std::pair<std::size_t, double>> pressureOutletCells_;

    // Face-level typed view: FaceVelocityBC per face replaces faceBC_.
    // faceHasBC_ is kept as a separate flag because no-slip walls set it to 1
    // even though their type is NoSlip — the two concepts are not equivalent.
    std::vector<FaceVelocityBC> faceBCs_;
    std::vector<char> faceHasBC_;

    const std::vector<std::vector<char>>& dirichletMask() const { return dirichletMask_; }
    const std::vector<std::vector<double>>& dirichletVals() const { return dirichletVals_; }
    const std::vector<std::vector<char>>& slipWallMask() const { return slipWallMask_; }
    const std::vector<char>& isPressureNeumann() const { return isPressureNeumann_; }
    const std::vector<std::pair<std::size_t, double>>& pressureOutletCells() const { return pressureOutletCells_; }

    // Per-dimension cell-level accessors.
    const std::vector<char>& dirichletMask(std::size_t d) const { return dirichletMask_[d]; }
    const std::vector<double>& dirichletVals(std::size_t d) const { return dirichletVals_[d]; }
    const std::vector<char>& slipWallMask(std::size_t d) const { return slipWallMask_[d]; }

    // Face-level typed-view accessors.
    const std::vector<FaceVelocityBC>& faceBCs() const { return faceBCs_; }
    const std::vector<char>& faceHasBCs() const { return faceHasBC_; }
    VelocityBCType faceBC(std::size_t f) const { return f < faceBCs_.size() ? faceBCs_[f].type : VelocityBCType::NoSlip; }
    bool hasFaceBC(std::size_t f) const { return f < faceHasBC_.size() && faceHasBC_[f] != 0; }
    bool isPressureDirichletFace(std::size_t f) const { return f < faceBCs_.size() && faceBCs_[f].type == VelocityBCType::PressureOutlet; }
    bool isPressureNeumannFace(std::size_t f) const { return f < faceBCs_.size() && faceBCs_[f].type != VelocityBCType::PressureOutlet; }
};

template<std::size_t Dim>
class SIMPLEBoundaryConditions {
public:
    explicit SIMPLEBoundaryConditions(const Mesh<Dim>& mesh);

    void setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                       double u = 0.0, double v = 0.0, double w = 0.0);
    void setPressureOutlet(std::size_t d, int sign, double pressure);

    void addVelocityPatch(const std::string& name, std::size_t d, int sign,
                          VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);
    const VelocityPatch* findVelocityPatch(const std::string& name) const;
    const std::vector<VelocityBC>& velBCs() const { return velBcs_; }
    const std::unordered_map<std::size_t, FaceVelocityBC>& faceVelBCs() const { return faceVelBCs_; }

    const std::vector<VelocityPatch>& velocityPatches() const { return velPatches_; }

    void setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                              VelocityBCType type,
                              double u = 0.0, double v = 0.0, double w = 0.0);
    void setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                      const std::string& typeStr,
                                      double u, double v, double w,
                                      bool slip,
                                      double pressure = 0.0);
    void setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices, double pressure);
    void setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices,
                                              double pressure,
                                              bool suppressBackflow,
                                              double backflowU, double backflowV, double backflowW,
                                              double backflowK, double backflowOmega);

    bool isDirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    BCDiscretization build();
    void ensureBuilt() { if (dirty_) build(); }

    BCSpec spec() const { return {velBcs_, velPatches_, faceVelBCs_}; }

    double maxInletSpeed(double fallback = 1.0) const;

    // Largest *prescribed* velocity magnitude anywhere in the domain, counting
    // moving walls as well as inlets. This is the correct scale for the
    // velocity-correction clamp and the p' limiter: a lid-driven cavity has no
    // inlet at all, so maxInletSpeed() would fall back to an arbitrary 1.0 and
    // the clamp could then override the lid velocity itself.
    double maxPrescribedSpeed(double fallback = 1.0) const;

    void debugPrint(const BCDiscretization& disc) const;

private:
    const Mesh<Dim>& mesh_;

    std::vector<VelocityBC> velBcs_;
    std::vector<VelocityPatch> velPatches_;
    std::unordered_map<std::size_t, FaceVelocityBC> faceVelBCs_;

    mutable bool dirty_ = true;
    mutable bool firstBuild_ = true;
};

extern template class SIMPLEBoundaryConditions<2>;
extern template class SIMPLEBoundaryConditions<3>;

} // namespace fvm
