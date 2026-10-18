#pragma once

#include "fvm/SIMPLETypes.h"
#include "fvm/SIMPLEBoundaryConditions.h"
#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "fvm/Types.h"
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace fvm {

enum class BCType { Dirichlet, Neumann };

inline VelocityBCType toVelocityBCType(BCType type, double value) {
    (void)value;
    return (type == BCType::Dirichlet) ? VelocityBCType::Inlet : VelocityBCType::Outlet;
}

struct Patch {
    std::string name;
    std::size_t wallDim;
    int wallSign;
    BCType type;
    double value;
};

inline Patch toPatch(const VelocityPatch& vp) {
    return Patch{vp.name, vp.wallDim, vp.wallSign,
                 (vp.type == VelocityBCType::Inlet || vp.type == VelocityBCType::MovingWall)
                     ? BCType::Dirichlet
                     : BCType::Neumann,
                 vp.u};
}

inline VelocityPatch toVelocityPatch(const Patch& p, double value) {
    return VelocityPatch{p.name, p.wallDim, p.wallSign,
                         toVelocityBCType(p.type, value), value, 0.0, 0.0};
}

template<std::size_t Dim>
class BoundaryConditions {
public:
    BoundaryConditions(const Mesh<Dim>& mesh, double k = 1.0);

    void setWall(std::size_t d, int sign, BCType type, double value);
    void setMin(std::size_t d, BCType type, double value);
    void setMax(std::size_t d, BCType type, double value);

    void addPatch(const std::string& name, std::size_t d, int sign, BCType type, double value);

    bool bcsConfigured() const;

    const Patch* findPatch(const std::string& name) const;
    BCType patchType(const std::string& name) const;
    double patchValue(const std::string& name) const;
    const std::vector<Patch>& patches() const;

    void buildDirichletMask();

    const std::vector<char>& dirichletMask() const;
    const std::vector<double>& dirichletValues() const;

    void setDirichletOnFaces(const std::vector<std::size_t>& faceIndices, double value);
    void setNeumannOnFaces(const std::vector<std::size_t>& faceIndices, double flux);

    void addNeumannContributions(Eigen::VectorXd& b) const;

    const Mesh<Dim>& mesh() const;

private:
    void markDirichletMin_(std::size_t d, double value);
    void markDirichletMax_(std::size_t d, double value);
    void addNeumannMin_(std::size_t d, double flux, Eigen::VectorXd& b) const;
    void addNeumannMax_(std::size_t d, double flux, Eigen::VectorXd& b) const;

    const Mesh<Dim>& mesh_;
    double k_;
    SIMPLEBoundaryConditions<Dim> bc_;
    BCDiscretization disc_;

    std::vector<char> dirichletMask_;
    std::vector<double> dirichletValues_;

    std::vector<std::pair<std::size_t, double>> faceDirichlet_;
    std::vector<std::pair<std::size_t, double>> faceNeumann_;
    struct NeumannWall { std::size_t d; int sign; double flux; };
    std::vector<NeumannWall> neumannWalls_;
    mutable std::vector<Patch> patchesCache_;
    bool bcsConfigured_ = false;
};

extern template class BoundaryConditions<2>;
extern template class BoundaryConditions<3>;

} // namespace fvm
