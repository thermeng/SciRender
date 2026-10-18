#include "fvm/BoundaryConditions.h"
#include "mesh/BoundaryUtils.h"
#include <algorithm>
#include <iostream>

namespace fvm {

template<std::size_t Dim>
BoundaryConditions<Dim>::BoundaryConditions(const Mesh<Dim>& mesh, double k)
    : mesh_(mesh), k_(k), bc_(mesh) {
    dirichletMask_.assign(mesh.nCells(), 0);
    dirichletValues_.assign(mesh.nCells(), 0.0);
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setWall(std::size_t d, int sign, BCType type, double value) {
    if (type == BCType::Dirichlet) {
        bc_.setVelocityBC(d, sign, VelocityBCType::Inlet, value, value, value);
    } else {
        bc_.setVelocityBC(d, sign, VelocityBCType::Outlet, 0.0, 0.0, 0.0);
        neumannWalls_.push_back({d, sign, value});
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setMin(std::size_t d, BCType type, double value) {
    if (type == BCType::Dirichlet) {
        bc_.setVelocityBC(d, -1, VelocityBCType::Inlet, value, value, value);
    } else {
        bc_.setVelocityBC(d, -1, VelocityBCType::Outlet, 0.0, 0.0, 0.0);
        neumannWalls_.push_back({d, -1, value});
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setMax(std::size_t d, BCType type, double value) {
    if (type == BCType::Dirichlet) {
        bc_.setVelocityBC(d, +1, VelocityBCType::Inlet, value, value, value);
    } else {
        bc_.setVelocityBC(d, +1, VelocityBCType::Outlet, 0.0, 0.0, 0.0);
        neumannWalls_.push_back({d, +1, value});
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::addPatch(const std::string& name,
                                       std::size_t d, int sign,
                                       BCType type, double value) {
    if (type == BCType::Dirichlet) {
        bc_.addVelocityPatch(name, d, sign, VelocityBCType::Inlet, value, value, value);
    } else {
        bc_.addVelocityPatch(name, d, sign, VelocityBCType::Outlet, 0.0, 0.0, 0.0);
        neumannWalls_.push_back({d, sign, value});
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
bool BoundaryConditions<Dim>::bcsConfigured() const { return bcsConfigured_; }

template<std::size_t Dim>
const Patch* BoundaryConditions<Dim>::findPatch(const std::string& name) const {
    const VelocityPatch* vp = bc_.findVelocityPatch(name);
    if (!vp) return nullptr;
    patchesCache_.push_back(toPatch(*vp));
    return &patchesCache_.back();
}

template<std::size_t Dim>
BCType BoundaryConditions<Dim>::patchType(const std::string& name) const {
    const VelocityPatch* p = bc_.findVelocityPatch(name);
    if (!p) return BCType::Dirichlet;
    return (p->type == VelocityBCType::Inlet || p->type == VelocityBCType::MovingWall)
           ? BCType::Dirichlet : BCType::Neumann;
}

template<std::size_t Dim>
double BoundaryConditions<Dim>::patchValue(const std::string& name) const {
    const VelocityPatch* p = bc_.findVelocityPatch(name);
    return p ? p->u : 0.0;
}

template<std::size_t Dim>
const std::vector<Patch>& BoundaryConditions<Dim>::patches() const {
    patchesCache_.clear();
    for (const auto& vp : bc_.velocityPatches()) {
        patchesCache_.push_back(toPatch(vp));
    }
    return patchesCache_;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::buildDirichletMask() {
    disc_ = bc_.build();

    std::fill(dirichletMask_.begin(), dirichletMask_.end(), char(0));
    std::fill(dirichletValues_.begin(), dirichletValues_.end(), 0.0);

    for (std::size_t d = 0; d < Dim; ++d) {
        const auto& mask = disc_.dirichletMask(d);
        const auto& vals = disc_.dirichletVals(d);
        for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
            if (mask[i]) {
                dirichletMask_[i] = 1;
                dirichletValues_[i] = vals[i];
            }
        }
    }

    for (const auto& [f, value] : faceDirichlet_) {
        std::size_t cell = mesh_.owner(f);
        dirichletMask_[cell] = 1;
        dirichletValues_[cell] = value;
    }

    bcsConfigured_ = true;
}

template<std::size_t Dim>
const std::vector<char>& BoundaryConditions<Dim>::dirichletMask() const {
    return dirichletMask_;
}

template<std::size_t Dim>
const std::vector<double>& BoundaryConditions<Dim>::dirichletValues() const {
    return dirichletValues_;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setDirichletOnFaces(const std::vector<std::size_t>& faceIndices,
                                                  double value) {
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) {
            std::cerr << "BoundaryConditions: face index " << f
                      << " exceeds nFaces (" << mesh_.nFaces() << ")\n";
            continue;
        }
        if (mesh_.neighbor(f) != std::size_t(-1)) {
            std::cerr << "BoundaryConditions: face " << f
                      << " is not a boundary face, skipping Dirichlet BC\n";
            continue;
        }
        faceDirichlet_.emplace_back(f, value);
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setNeumannOnFaces(const std::vector<std::size_t>& faceIndices,
                                                double flux) {
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) {
            std::cerr << "BoundaryConditions: face index " << f
                      << " exceeds nFaces (" << mesh_.nFaces() << ")\n";
            continue;
        }
        if (mesh_.neighbor(f) != std::size_t(-1)) {
            std::cerr << "BoundaryConditions: face " << f
                      << " is not a boundary face, skipping Neumann BC\n";
            continue;
        }
        faceNeumann_.emplace_back(f, flux);
    }
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::addNeumannContributions(Eigen::VectorXd& b) const {
    for (const auto& [f, flux] : faceNeumann_) {
        std::size_t cell = mesh_.owner(f);
        b(cell) += k_ * flux * mesh_.faceArea(f);
    }
    for (const auto& nw : neumannWalls_) {
        if (nw.sign == -1) addNeumannMin_(nw.d, nw.flux, b);
        else addNeumannMax_(nw.d, nw.flux, b);
    }
}

template<std::size_t Dim>
const Mesh<Dim>& BoundaryConditions<Dim>::mesh() const { return mesh_; }

template<std::size_t Dim>
void BoundaryConditions<Dim>::markDirichletMin_(std::size_t d, double value) {
    bc_.setVelocityBC(d, -1, VelocityBCType::Inlet, value, 0.0, 0.0);
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::markDirichletMax_(std::size_t d, double value) {
    bc_.setVelocityBC(d, +1, VelocityBCType::Inlet, value, 0.0, 0.0);
    bcsConfigured_ = false;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::addNeumannMin_(std::size_t d, double flux, Eigen::VectorXd& b) const {
    forEachBoundaryFace(mesh_, d, -1, [&](std::size_t f, std::size_t) {
        b(mesh_.owner(f)) += k_ * flux * mesh_.faceArea(f);
    });
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::addNeumannMax_(std::size_t d, double flux, Eigen::VectorXd& b) const {
    forEachBoundaryFace(mesh_, d, +1, [&](std::size_t f, std::size_t) {
        b(mesh_.owner(f)) += k_ * flux * mesh_.faceArea(f);
    });
}

template class BoundaryConditions<2>;
template class BoundaryConditions<3>;

} // namespace fvm
