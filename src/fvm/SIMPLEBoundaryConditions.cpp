#include "fvm/SIMPLEBoundaryConditions.h"
#include "fvm/FaceState.h"
#include "mesh/BoundaryUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

namespace fvm {

template<std::size_t Dim>
void applyWallBCToMasks(const Mesh<Dim>& mesh,
                        std::size_t wall_d, int wall_sign, const VelocityBC& bc,
                        std::vector<std::vector<char>>& dirichletMask,
                        std::vector<std::vector<double>>& dirichletVals,
                        std::vector<std::vector<char>>& slipWallMask) {
    if (bc.type == VelocityBCType::NoSlip || bc.type == VelocityBCType::MovingWall) {
        for (std::size_t d = 0; d < Dim; ++d) {
            double val = (d == 0) ? bc.u : (d == 1) ? bc.v : bc.w;
            forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
                (void)f;
                dirichletMask[d][idx] = true;
                dirichletVals[d][idx] = val;
            });
        }
    } else if (bc.type == VelocityBCType::Inlet) {
        if (mesh.isUnstructured()) {
            // Unstructured/non-cartesian: full-vector Dirichlet (exact face value via faceBC later)
            for (std::size_t d = 0; d < Dim; ++d) {
                double val = (d == 0) ? bc.u : (d == 1) ? bc.v : bc.w;
                forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
                    (void)f;
                    dirichletMask[d][idx] = true;
                    dirichletVals[d][idx] = val;
                });
            }
        } else {
            double normalVal = (wall_d == 0) ? bc.u : (wall_d == 1) ? bc.v : bc.w;
            forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
                (void)f;
                dirichletMask[wall_d][idx] = true;
                dirichletVals[wall_d][idx] = normalVal;
                for (std::size_t d = 0; d < Dim; ++d) {
                    if (d != wall_d) {
                        slipWallMask[d][idx] = true;
                    }
                }
            });
        }
    } else if (bc.type == VelocityBCType::Symmetry || bc.type == VelocityBCType::SlipWall) {
        forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
            (void)f;
            for (std::size_t d = 0; d < Dim; ++d) {
                if (d != wall_d) {
                    slipWallMask[d][idx] = true;
                }
            }
        });
    }
}

template<std::size_t Dim>
void buildFaceToWallMap(const Mesh<Dim>& mesh,
                        std::vector<std::pair<std::size_t, int>>& faceToWallMap) {
    faceToWallMap.assign(mesh.nFaces(), {static_cast<std::size_t>(-1), 0});
    if (mesh.isUnstructured()) return;

    for (std::size_t wall_d = 0; wall_d < Dim; ++wall_d) {
        for (int sign : {-1, +1}) {
            const auto& bFaces = mesh.boundaryFaces(wall_d, sign);
            for (std::size_t f : bFaces) {
                if (f < faceToWallMap.size()) {
                    faceToWallMap[f] = {wall_d, sign};
                }
            }
        }
    }
}

template<std::size_t Dim>
BCDiscretization buildBCMasks(const Mesh<Dim>& mesh, const BCSpec& spec) {
    BCDiscretization disc;
    disc.dirichletMask_.resize(Dim);
    disc.dirichletVals_.resize(Dim);
    disc.slipWallMask_.resize(Dim);
    for (std::size_t d = 0; d < Dim; ++d) {
        disc.dirichletMask_[d].assign(mesh.nCells(), false);
        disc.dirichletVals_[d].assign(mesh.nCells(), 0.0);
        disc.slipWallMask_[d].assign(mesh.nCells(), false);
    }
    disc.isPressureNeumann_.assign(mesh.nCells(), false);
    disc.isPressureNeumannFace_.assign(mesh.nFaces(), false);
    disc.faceBC_.assign(mesh.nFaces(), FaceVelocityBC{});
    disc.faceHasBC_.assign(mesh.nFaces(), 0);
    disc.isPressureDirichletFace_.assign(mesh.nFaces(), 0);

    std::vector<std::pair<std::size_t, int>> faceToWallMap;
    buildFaceToWallMap(mesh, faceToWallMap);

    if (!mesh.isUnstructured()) {
        // Structured meshes use a cell based boundary treatment: the wall
        // conditions are imposed on the boundary cells. An unstructured mesh
        // is treated face by face (see the face loop below), so the cells are
        // deliberately NOT frozen here - a cell that only owns a boundary face
        // must stay a free unknown, otherwise the pressure correction loses
        // the degree of freedom it needs to balance that cell (see
        // VelocityCorrection.h).
        for (std::size_t wall_d = 0; wall_d < Dim; ++wall_d) {
            applyWallBCToMasks(mesh, wall_d, -1, spec.velBcs[wall_d * 2],
                               disc.dirichletMask_, disc.dirichletVals_, disc.slipWallMask_);
            applyWallBCToMasks(mesh, wall_d, +1, spec.velBcs[wall_d * 2 + 1],
                               disc.dirichletMask_, disc.dirichletVals_, disc.slipWallMask_);
        }

        // A cell that owns a boundary face with an explicit FACE condition is
        // NOT frozen by the cell mask. The condition lives on the face, and
        // the momentum assembly skips the boundary faces of a frozen cell - so
        // freezing it would silently ignore the face condition altogether (a
        // moving wall prescribed on its faces would leave the domain at rest).
        // The cell stays a free unknown instead, exactly as on an unstructured
        // mesh, and the face condition drives it.
        for (const auto& [faceIdx, fbc] : spec.faceVelBCs) {
            (void)fbc;
            if (faceIdx >= mesh.nFaces() || mesh.neighbor(faceIdx) != std::size_t(-1)) continue;
            const std::size_t cell = mesh.owner(faceIdx);
            for (std::size_t d = 0; d < Dim; ++d) {
                disc.dirichletMask_[d][cell] = 0;
                disc.dirichletVals_[d][cell] = 0.0;
            }
        }
    }

    if (!mesh.isUnstructured()) {
        for (std::size_t corner_d = 0; corner_d < Dim; ++corner_d) {
            for (int corner_sign : {-1, +1}) {
                for (std::size_t other_d = 0; other_d < Dim; ++other_d) {
                    if (other_d == corner_d) continue;
                    for (int other_sign : {-1, +1}) {
                        const auto& cornerBC = spec.velBcs[corner_d * 2 + (corner_sign < 0 ? 0 : 1)];
                        const auto& otherBC  = spec.velBcs[other_d  * 2 + (other_sign < 0 ? 0 : 1)];
                        auto isDirichletWall = [](VelocityBCType t) {
                            return t == VelocityBCType::NoSlip ||
                                   t == VelocityBCType::MovingWall ||
                                   t == VelocityBCType::Inlet;
                        };
                        if (!isDirichletWall(cornerBC.type) || !isDirichletWall(otherBC.type)) {
                            continue;
                        }
                        bool cMoving = (cornerBC.type == VelocityBCType::MovingWall);
                        bool oMoving = (otherBC.type  == VelocityBCType::MovingWall);
                        bool mixedMovingStationary = (cMoving != oMoving);

                        double cornerVals[Dim]{};
                        bool   cornerValid[Dim]{};
                        for (std::size_t d = 0; d < Dim; ++d) {
                            double vC = (d == 0) ? cornerBC.u : (d == 1) ? cornerBC.v : cornerBC.w;
                            double vO = (d == 0) ? otherBC.u  : (d == 1) ? otherBC.v  : otherBC.w;
                            if (mixedMovingStationary) {
                                // Let mixed corners float: do not force a Dirichlet value
                                // for either velocity component; the momentum equations
                                // will determine the cell value from face fluxes.
                                cornerValid[d] = false;
                            } else if (cMoving && oMoving) {
                                if (std::abs(vC - vO) < 1e-12) {
                                    cornerVals[d] = vC;
                                    cornerValid[d] = true;
                                } else {
                                    cornerVals[d] = 0.0;
                                    cornerValid[d] = true;
                                }
                            } else {
                                // Both stationary: no override needed.
                                cornerValid[d] = false;
                            }
                        }
                        forEachBoundaryCell(mesh, corner_d, corner_sign, [&](std::size_t cell) {
                            auto c = mesh.cellCoords(cell);
                            if (c[other_d] == (other_sign < 0 ? 0 : mesh.n(other_d) - 1)) {
                                if (mixedMovingStationary) {
                                    for (std::size_t d = 0; d < Dim; ++d) {
                                        disc.dirichletMask_[d][cell] = 0;
                                    }
                                } else {
                                    for (std::size_t d = 0; d < Dim; ++d) {
                                        if (cornerValid[d]) {
                                            disc.dirichletMask_[d][cell] = 1;
                                            disc.dirichletVals_[d][cell] = cornerVals[d];
                                        }
                                    }
                                }
                            }
                        });
                    }
                }
            }
        }
    }

    if (!mesh.isUnstructured()) {
        for (std::size_t wall_d = 0; wall_d < Dim; ++wall_d) {
            for (int sign : {-1, +1}) {
                std::size_t idx = wall_d * 2 + (sign < 0 ? 0 : 1);
                const auto& bc = spec.velBcs[idx];
                if (bc.type == VelocityBCType::Outlet ||
                    bc.type == VelocityBCType::Symmetry ||
                    bc.type == VelocityBCType::SlipWall ||
                    bc.type == VelocityBCType::Inlet ||
                    bc.type == VelocityBCType::NoSlip ||
                    bc.type == VelocityBCType::MovingWall) {
                    forEachBoundaryFace(mesh, wall_d, sign, [&](std::size_t f, std::size_t) {
                        disc.isPressureNeumannFace_[f] = 1;
                    });
                } else if (bc.type == VelocityBCType::PressureOutlet) {
                    forEachBoundaryFace(mesh, wall_d, sign, [&](std::size_t f, std::size_t cell) {
                        (void)f;
                        disc.pressureOutletCells_.emplace_back(cell, bc.pressureValue);
                    });
                }
            }
        }
    }

    for (const auto& [faceIdx, fbc] : spec.faceVelBCs) {
        if (faceIdx >= mesh.nFaces()) continue;
        std::size_t cell = mesh.owner(faceIdx);
        if (fbc.type == VelocityBCType::Inlet) {
            disc.isPressureNeumannFace_[faceIdx] = 1;
        } else if (fbc.type == VelocityBCType::NoSlip || fbc.type == VelocityBCType::MovingWall) {
            disc.isPressureNeumannFace_[faceIdx] = 1;
        } else if (fbc.type == VelocityBCType::Symmetry || fbc.type == VelocityBCType::SlipWall) {
            if (!mesh.isUnstructured()) {
                const auto& fn = mesh.faceNormal(faceIdx);
                // Same "dominant normal component" rule the momentum assembly
                // and the pressure correction apply to a slip face.
                const std::size_t maxD = dominantNormalComponent<Dim>(fn.data());
                for (std::size_t d = 0; d < Dim; ++d) {
                    if (d != maxD) disc.slipWallMask_[d][cell] = 1;
                }
            }
            disc.isPressureNeumannFace_[faceIdx] = 1;
        } else if (fbc.type == VelocityBCType::Outlet) {
            disc.isPressureNeumannFace_[faceIdx] = 1;
        } else if (fbc.type == VelocityBCType::PressureOutlet) {
            disc.pressureOutletCells_.emplace_back(cell, fbc.pressureValue);
        }
    }

    std::sort(disc.pressureOutletCells_.begin(), disc.pressureOutletCells_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    disc.pressureOutletCells_.erase(
        std::unique(disc.pressureOutletCells_.begin(), disc.pressureOutletCells_.end(),
                    [](const auto& a, const auto& b) { return a.first == b.first; }),
        disc.pressureOutletCells_.end());

    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        auto it = spec.faceVelBCs.find(f);
        if (it != spec.faceVelBCs.end()) {
            disc.faceBC_[f] = it->second;
            disc.faceHasBC_[f] = 1;
            if (it->second.type == VelocityBCType::PressureOutlet) {
                disc.isPressureDirichletFace_[f] = 1;
                disc.pressureOutletFaces_.emplace_back(f, it->second.pressureValue);
            }
            continue;
        }
        if (!mesh.isUnstructured()) {
            const auto& wallMap = faceToWallMap[f];
            if (wallMap.first != static_cast<std::size_t>(-1)) {
                std::size_t wall_d = wallMap.first;
                int sign = wallMap.second;
                std::size_t idx = wall_d * 2 + (sign < 0 ? 0 : 1);
                const auto& bc = spec.velBcs[idx];
                FaceVelocityBC fbc;
                fbc.type = bc.type;
                fbc.u = bc.u; fbc.v = bc.v; fbc.w = bc.w;
                fbc.pressureValue = bc.pressureValue;
                disc.faceBC_[f] = fbc;
                disc.faceHasBC_[f] = 1;
                if (bc.type == VelocityBCType::PressureOutlet) {
                    disc.isPressureDirichletFace_[f] = 1;
                    disc.pressureOutletFaces_.emplace_back(f, bc.pressureValue);
                }
            } else {
                disc.faceBC_[f].type = VelocityBCType::NoSlip;
                disc.faceHasBC_[f] = 1;
                disc.isPressureNeumannFace_[f] = 1;
            }
        } else {
            // Unstructured mesh: deduce the wall direction from the face
            // normal so that a direction based BC (inlet, outlet, no-slip
            // wall, moving wall, ...) still applies to this face. The
            // previous code applied the direction based BCs only when NO face
            // based BC existed at all, so on an unstructured mesh a single
            // face based BC silently discarded every direction based BC and
            // those walls fell back to the default no-slip.
            const auto& fn = mesh.faceNormal(f);
            std::size_t wall_d = 0;
            double maxA = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                const double a = std::abs(fn[d]);
                if (a > maxA) { maxA = a; wall_d = d; }
            }
            const int sign = (fn[wall_d] < 0.0) ? -1 : 1;
            if (maxA >= 0.5) {
                // The face is aligned with a coordinate direction: use the
                // direction based boundary condition.
                const auto& bc = spec.velBcs[wall_d * 2 + (sign < 0 ? 0 : 1)];
                FaceVelocityBC fbc;
                fbc.type = bc.type;
                fbc.u = bc.u; fbc.v = bc.v; fbc.w = bc.w;
                fbc.pressureValue = bc.pressureValue;
                disc.faceBC_[f] = fbc;
                disc.faceHasBC_[f] = 1;
                if (bc.type == VelocityBCType::PressureOutlet) {
                    disc.isPressureDirichletFace_[f] = 1;
                    disc.pressureOutletFaces_.emplace_back(f, bc.pressureValue);
                } else {
                    disc.isPressureNeumannFace_[f] = 1;
                }
            } else {
                // Oblique face: no direction based condition applies.
                disc.faceBC_[f].type = VelocityBCType::NoSlip;
                disc.faceHasBC_[f] = 1;
                // A no-slip wall is a pressure-Neumann boundary. Without this flag the
                // owning cell fails the "every boundary face is Neumann" test below,
                // and PressureCorrection then drops that cell from the Poisson system
                // as a p' = 0 Dirichlet cell - i.e. it invents a p = 0 constraint on a
                // wall that the momentum equation treats as a solid no-slip surface.
                // The two must agree, otherwise the pressure field that drives the
                // recirculation is corrupted exactly at the boundary.
                disc.isPressureNeumannFace_[f] = 1;
            }
        }
    }

    // Cell-level pressure condition: a boundary cell is pressure-Neumann only
    // when EVERY boundary face it owns is Neumann. This must run after the
    // face BC assignment above, because that is where the fallback no-slip wall
    // is installed - and the fallback has to contribute its Neumann flag.
    {
        std::vector<int> boundaryFaceCount(mesh.nCells(), 0);
        std::vector<int> pressureNeumannCount(mesh.nCells(), 0);
        for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            std::size_t P = mesh.owner(f);
            ++boundaryFaceCount[P];
            if (disc.isPressureNeumannFace_[f]) ++pressureNeumannCount[P];
        }
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            disc.isPressureNeumann_[i] = (boundaryFaceCount[i] > 0 &&
                                         pressureNeumannCount[i] == boundaryFaceCount[i]);
        }
    }

    std::sort(disc.pressureOutletFaces_.begin(), disc.pressureOutletFaces_.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    disc.pressureOutletFaces_.erase(
        std::unique(disc.pressureOutletFaces_.begin(), disc.pressureOutletFaces_.end(),
                    [](const auto& a, const auto& b){ return a.first == b.first; }),
        disc.pressureOutletFaces_.end());

    return disc;
}

template<std::size_t Dim>
SIMPLEBoundaryConditions<Dim>::SIMPLEBoundaryConditions(const Mesh<Dim>& mesh)
    : mesh_(mesh),
      velBcs_(2 * Dim),
      velPatches_(),
      faceVelBCs_() {
    for (auto& bc : velBcs_) bc = VelocityBC{};
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                                                   double val1, double val2, double val3) {
    std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
    velBcs_[idx] = type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall
                       ? VelocityBC{type, val1, val2, val3}
                       : VelocityBC{type, 0.0, 0.0, 0.0};
    dirty_ = true;
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setPressureOutlet(std::size_t d, int sign, double pressure) {
    std::size_t idx = d * 2 + (sign < 0 ? 0 : 1);
    velBcs_[idx].type = VelocityBCType::PressureOutlet;
    velBcs_[idx].u = 0.0;
    velBcs_[idx].v = 0.0;
    velBcs_[idx].w = 0.0;
    velBcs_[idx].pressureValue = pressure;
    dirty_ = true;
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::addVelocityPatch(const std::string& name, std::size_t d, int sign,
                                                      VelocityBCType type, double u, double v, double w) {
    velPatches_.push_back({name, d, sign, type, u, v, w});
    setVelocityBC(d, sign, type, u, v, w);
}

template<std::size_t Dim>
const VelocityPatch* SIMPLEBoundaryConditions<Dim>::findVelocityPatch(const std::string& name) const {
    for (const auto& p : velPatches_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                                                          VelocityBCType type,
                                                          double u, double v, double w) {
    FaceVelocityBC fbc;
    fbc.type = type;
    fbc.u = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? u : 0.0;
    fbc.v = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? v : 0.0;
    fbc.w = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? w : 0.0;
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) continue;
        faceVelBCs_[f] = fbc;
    }
    dirty_ = true;
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                                                  const std::string& typeStr,
                                                                  double u, double v, double w,
                                                                  bool slip,
                                                                  double pressure) {
    std::string t;
    t.reserve(typeStr.size());
    for (char c : typeStr) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (t == "inlet" || t == "velocity_inlet") {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::Inlet, u, v, w);
    } else if (t == "outlet") {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::Outlet);
    } else if (t == "pressure_outlet") {
        setPressureOutletOnFaces(faceIndices, pressure);
    } else if (t == "moving_wall") {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::MovingWall, u, v, w);
    } else if (t == "symmetry") {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::Symmetry);
    } else if (slip || t == "slip" || t == "slip_wall" || t == "slipwall") {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::SlipWall);
    } else {
        setVelocityBCOnFaces(faceIndices, VelocityBCType::NoSlip);
    }
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices,
                                                              double pressure) {
    FaceVelocityBC fbc;
    fbc.type = VelocityBCType::PressureOutlet;
    fbc.u = 0.0;
    fbc.v = 0.0;
    fbc.w = 0.0;
    fbc.pressureValue = pressure;
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) continue;
        faceVelBCs_[f] = fbc;
    }
    dirty_ = true;
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices,
                                                                          double pressure,
                                                                          bool suppressBackflow,
                                                                          double backflowU, double backflowV, double backflowW,
                                                                          double backflowK, double backflowOmega) {
    FaceVelocityBC fbc;
    fbc.type = VelocityBCType::PressureOutlet;
    fbc.u = 0.0;
    fbc.v = 0.0;
    fbc.w = 0.0;
    fbc.pressureValue = pressure;
    fbc.suppressBackflow = suppressBackflow;
    fbc.backflowU = backflowU;
    fbc.backflowV = backflowV;
    fbc.backflowW = backflowW;
    fbc.backflowK = backflowK;
    fbc.backflowOmega = backflowOmega;
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) continue;
        faceVelBCs_[f] = fbc;
    }
    dirty_ = true;
}

template<std::size_t Dim>
double SIMPLEBoundaryConditions<Dim>::maxInletSpeed(double fallback) const {
    double maxS = 0.0;
    for (std::size_t i = 0; i < 2 * Dim; ++i) {
        if (velBcs_[i].type == VelocityBCType::Inlet) {
            double spd = std::sqrt(velBcs_[i].u * velBcs_[i].u
                                 + velBcs_[i].v * velBcs_[i].v
                                 + velBcs_[i].w * velBcs_[i].w);
            maxS = std::max(maxS, spd);
        }
    }
    for (const auto& [faceIdx, fbc] : faceVelBCs_) {
        if (fbc.type == VelocityBCType::Inlet) {
            double spd = std::sqrt(fbc.u * fbc.u + fbc.v * fbc.v + fbc.w * fbc.w);
            maxS = std::max(maxS, spd);
        }
    }
    if (maxS < 1e-12) maxS = fallback;
    return maxS;
}

template<std::size_t Dim>
BCDiscretization SIMPLEBoundaryConditions<Dim>::build() {
    BCDiscretization disc = buildBCMasks(mesh_, {velBcs_, velPatches_, faceVelBCs_});
    if (firstBuild_) {
        firstBuild_ = false;
        debugPrint(disc);
    }
    dirty_ = false;
    return disc;
}

template <std::size_t Dim>
double SIMPLEBoundaryConditions<Dim>::maxPrescribedSpeed(double fallback) const {
    double maxS = 0.0;
    auto consider = [&](double u, double v, double w) {
        double s = u * u + v * v;
        if constexpr (Dim >= 3) s += w * w;
        if (s > maxS) maxS = s;
    };
    for (const auto& bc : velBcs_) {
        if (bc.type != VelocityBCType::Inlet && bc.type != VelocityBCType::MovingWall) continue;
        consider(bc.u, bc.v, bc.w);
    }
    for (const auto& bc : faceVelBCs_) {
        if (bc.second.type != VelocityBCType::Inlet && bc.second.type != VelocityBCType::MovingWall) continue;
        consider(bc.second.u, bc.second.v, bc.second.w);
    }
    if (maxS <= 0.0) return fallback;
    return std::sqrt(maxS);
}

template<std::size_t Dim>
void SIMPLEBoundaryConditions<Dim>::debugPrint(const BCDiscretization& disc) const {
    std::size_t faceVelBCCount = faceVelBCs_.size();
    std::size_t dirichletCount[Dim]{};
    std::size_t neumannCount = 0;
    std::size_t neumannFaceCount = 0;
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        if (disc.isPressureNeumann_[i]) ++neumannCount;
        for (std::size_t d = 0; d < Dim; ++d)
            if (disc.dirichletMask_[d][i]) dirichletCount[d]++;
    }
    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (disc.isPressureNeumannFace_[f]) ++neumannFaceCount;
    }
    std::cout << "  [BC] faceVelBCs=" << faceVelBCCount
              << " dirichlet U=" << dirichletCount[0] << " V=" << dirichletCount[1];
    if constexpr (Dim == 3) std::cout << " W=" << dirichletCount[2];
    std::cout << " pressureNeumann cells=" << neumannCount
              << " faces=" << neumannFaceCount
              << " pressureOutlet cells=" << disc.pressureOutletCells_.size()
              << " faces=" << disc.pressureOutletFaces_.size() << "\n";
}

template class SIMPLEBoundaryConditions<2>;
template class SIMPLEBoundaryConditions<3>;

} // namespace fvm
