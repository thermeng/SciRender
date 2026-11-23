#include "fvm/BoundaryConditions.h"
#include "fvm/FaceState.h"
#include "mesh/BoundaryUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace fvm {

// ── Helpers ──────────────────────────────────────────────────────────────

// Extract velocity component d (0=x, 1=y, 2=z) from a VelocityBC or
// FaceVelocityBC. Consolidates the (d == 0 ? u : d == 1 ? v : w) ternary
// chain that was repeated across 6 sites.
inline double componentAt(const VelocityBC& bc, std::size_t d) {
    return (d == 0) ? bc.u : (d == 1) ? bc.v : bc.w;
}
inline double componentAt(const FaceVelocityBC& fbc, std::size_t d) {
    return (d == 0) ? fbc.u : (d == 1) ? fbc.v : fbc.w;
}

// Wall index into velBcs_ for dimension d and sign (-1 or +1).
// Replaces the magic-number arithmetic d*2 + (sign < 0 ? 0 : 1).
inline std::size_t wallIndex(std::size_t d, int sign) {
    return d * 2 + static_cast<std::size_t>(sign < 0 ? 0 : 1);
}

// Speed magnitude from u/v/w components. Only Dim components are used.
inline double speedMagnitude(double u, double v, double w) {
    return std::sqrt(u * u + v * v + w * w);
}

// Squared speed magnitude — avoids sqrt when only comparing magnitudes.
inline double speedMagnitudeSquared(double u, double v, double w) {
    return u * u + v * v + w * w;
}

// Assign a VelocityBC into a FaceVelocityBC and mark the face as having a BC.
inline void assignFaceBC(FaceVelocityBC& fb, const VelocityBC& bc,
                         std::vector<char>& faceHasBC, std::size_t f) {
    fb.type = bc.type;
    fb.u = bc.u; fb.v = bc.v; fb.w = bc.w;
    fb.pressureValue = bc.pressureValue;
    faceHasBC[f] = 1;
}

// Register a pressure-outlet cell (cell index + prescribed pressure).
inline void registerPressureOutletCell(
        std::vector<std::pair<std::size_t, double>>& pressureOutletCells,
        std::size_t cell, double pressureValue) {
    pressureOutletCells.emplace_back(cell, pressureValue);
}

// Apply a FaceVelocityBC to every face index in faceIndices.
template<std::size_t Dim>
void applyToFaces(const Mesh<Dim>& mesh,
                  const std::vector<std::size_t>& faceIndices,
                  const FaceVelocityBC& fbc,
                  std::unordered_map<std::size_t, FaceVelocityBC>& faceVelBCs) {
    for (std::size_t f : faceIndices) {
        if (f >= mesh.nFaces()) continue;
        faceVelBCs[f] = fbc;
    }
}

template<std::size_t Dim>
void applyWallBCToMasks(const Mesh<Dim>& mesh,
                        std::size_t wall_d, int wall_sign, const VelocityBC& bc,
                        std::vector<std::vector<char>>& dirichletMask,
                        std::vector<std::vector<double>>& dirichletVals,
                        std::vector<std::vector<char>>& slipWallMask) {
    if (bc.type == VelocityBCType::NoSlip || bc.type == VelocityBCType::MovingWall) {
        for (std::size_t d = 0; d < Dim; ++d) {
            double val = componentAt(bc, d);
            forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
                (void)idx;   // idx is a loop counter, not a cell index -- use mesh.owner(f)
                const std::size_t cell = mesh.owner(f);
                dirichletMask[d][cell] = true;
                dirichletVals[d][cell] = val;
            });
        }
    } else if (bc.type == VelocityBCType::Inlet) {
        // Inlet: prescribe the WHOLE velocity vector on every boundary face,
        // for both structured and unstructured meshes. On structured meshes the
        // old code only prescribed the normal component and marked tangential
        // components as slip, which let the pressure correction alter the inlet
        // tangential velocity — violating the prescribed-velocity contract.
        for (std::size_t d = 0; d < Dim; ++d) {
            double val = componentAt(bc, d);
            forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
                (void)idx;   // idx is a loop counter, not a cell index -- use mesh.owner(f)
                const std::size_t cell = mesh.owner(f);
                dirichletMask[d][cell] = true;
                dirichletVals[d][cell] = val;
            });
        }
    } else if (bc.type == VelocityBCType::Symmetry || bc.type == VelocityBCType::SlipWall) {
        forEachBoundaryFace(mesh, wall_d, wall_sign, [&](std::size_t f, std::size_t idx) {
            (void)idx;   // idx is a loop counter, not a cell index -- use mesh.owner(f)
            const std::size_t cell = mesh.owner(f);
            for (std::size_t d = 0; d < Dim; ++d) {
                if (d != wall_d) {
                    slipWallMask[d][cell] = true;
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
void initializeBuffers(BCDiscretization& disc, const Mesh<Dim>& mesh) {
    disc.dirichletMask_.resize(Dim);
    disc.dirichletVals_.resize(Dim);
    disc.slipWallMask_.resize(Dim);
    for (std::size_t d = 0; d < Dim; ++d) {
        disc.dirichletMask_[d].assign(mesh.nCells(), false);
        disc.dirichletVals_[d].assign(mesh.nCells(), 0.0);
        disc.slipWallMask_[d].assign(mesh.nCells(), false);
    }
    disc.isPressureNeumann_.assign(mesh.nCells(), false);
    disc.faceBCs_.assign(mesh.nFaces(), FaceVelocityBC{});
    disc.faceHasBC_.assign(mesh.nFaces(), 0);
}

template<std::size_t Dim>
void applyStructuredCellMasks(const Mesh<Dim>& mesh, const BCSpec& spec,
                               BCDiscretization& disc) {
    // Structured meshes use a cell based boundary treatment: the wall
    // conditions are imposed on the boundary cells. An unstructured mesh
    // is treated face by face (see the face loop below), so the cells are
    // deliberately NOT frozen here - a cell that only owns a boundary face
    // must stay a free unknown, otherwise the pressure correction loses
    // the degree of freedom it needs to balance that cell (see
    // VelocityCorrection.h).
    for (std::size_t wall_d = 0; wall_d < Dim; ++wall_d) {
        applyWallBCToMasks(mesh, wall_d, -1, spec.velBcs[wallIndex(wall_d, -1)],
                           disc.dirichletMask_, disc.dirichletVals_, disc.slipWallMask_);
        applyWallBCToMasks(mesh, wall_d, +1, spec.velBcs[wallIndex(wall_d, +1)],
                           disc.dirichletMask_, disc.dirichletVals_, disc.slipWallMask_);
    }
}

template<std::size_t Dim>
void applyFaceBasedCellPinning(const Mesh<Dim>& mesh, const BCSpec& spec,
                                BCDiscretization& disc) {
    // A cell that owns a boundary face with an explicit FACE condition:
    //   - Inlet/MovingWall: pin the cell velocity to the prescribed value
    //     (dirichlet mask) so VelocityCorrection keeps it exact.
    //   - Other types (Outlet, PressureOutlet, NoSlip, Symmetry, SlipWall):
    //     keep the cell a free unknown; the face condition drives it via
    //     boundaryFaceState() in the momentum assembly. Freezing these
    //     cells would skip the boundary-face flux contributions entirely.
    // Only set pins here -- never clear dirichletMask_/dirichletVals_ -- so a
    // face BC for one component/face of a cell cannot wipe a mask set by
    // another face, by a structured wall slot, or by the corner handler.
    for (const auto& [faceIdx, fbc] : spec.faceVelBCs) {
        if (faceIdx >= mesh.nFaces() || mesh.neighbor(faceIdx) != std::size_t(-1)) continue;
        const std::size_t cell = mesh.owner(faceIdx);
        if (cell >= mesh.nCells()) continue;  // S1: guard against invalid owner index
        const bool prescribeCell =
            fbc.type == VelocityBCType::Inlet || fbc.type == VelocityBCType::MovingWall;
        if (!prescribeCell) continue;
        for (std::size_t d = 0; d < Dim; ++d) {
            double val = componentAt(fbc, d);
            disc.dirichletMask_[d][cell] = 1;
            disc.dirichletVals_[d][cell] = val;
        }
    }
}

template<std::size_t Dim>
void applyStructuredCornerHandling(const Mesh<Dim>& mesh, const BCSpec& spec,
                                    BCDiscretization& disc) {
    for (std::size_t corner_d = 0; corner_d < Dim; ++corner_d) {
        for (int corner_sign : {-1, +1}) {
            for (std::size_t other_d = 0; other_d < Dim; ++other_d) {
                if (other_d == corner_d) continue;
                for (int other_sign : {-1, +1}) {
                    const auto& cornerBC = spec.velBcs[wallIndex(corner_d, corner_sign)];
                    const auto& otherBC  = spec.velBcs[wallIndex(other_d, other_sign)];
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
                        double vC = componentAt(cornerBC, d);
                        double vO = componentAt(otherBC, d);
                        if (mixedMovingStationary) {
                            // Let mixed corners float: do not force a Dirichlet value
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

template<std::size_t Dim>
void collectStructuredPressureOutletCells(const Mesh<Dim>& mesh, const BCSpec& spec,
                                           BCDiscretization& disc) {
    for (std::size_t wall_d = 0; wall_d < Dim; ++wall_d) {
        for (int sign : {-1, +1}) {
            const auto& bc = spec.velBcs[wallIndex(wall_d, sign)];
            if (bc.type == VelocityBCType::PressureOutlet) {
                forEachBoundaryFace(mesh, wall_d, sign, [&](std::size_t f, std::size_t cell) {
                    (void)f;
                    registerPressureOutletCell(disc.pressureOutletCells_, cell, bc.pressureValue);
                });
            }
            // All other BC types (Outlet, Symmetry, SlipWall, Inlet,
            // NoSlip, MovingWall) get their faceBCs_ entry in the boundary
            // face loop below; no separate Neumann flag needed here.
        }
    }
}

template<std::size_t Dim>
void applyFaceBasedBCs(const Mesh<Dim>& mesh, const BCSpec& spec,
                        BCDiscretization& disc) {
    for (const auto& [faceIdx, fbc] : spec.faceVelBCs) {
        if (faceIdx >= mesh.nFaces()) continue;
        std::size_t cell = mesh.owner(faceIdx);
        if (cell >= mesh.nCells()) continue;  // S1: guard against invalid owner index
        disc.faceBCs_[faceIdx] = fbc;
        disc.faceHasBC_[faceIdx] = 1;
        if (fbc.type == VelocityBCType::PressureOutlet) {
            registerPressureOutletCell(disc.pressureOutletCells_, cell, fbc.pressureValue);
        }
        // Pin inlet / moving wall cell-center velocity to the prescribed value.
        // Face-based BCs drive the momentum flux via faceBCs_, but the cell-center
        // velocity is a free unknown unless pinned here — otherwise pressure
        // correction and neighbour influence drift it away from the prescribed
        // inlet speed (probe/post-processing read cell-centred data).
        // NOTE: cell pinning for face-based Inlet/MovingWall BCs is performed
        // once in applyFaceBasedCellPinning(), which runs before
        // applyStructuredCornerHandling() so corner decisions are not overwritten.
    }
}

void sortAndDedupPressureOutletCells(BCDiscretization& disc) {
    std::sort(disc.pressureOutletCells_.begin(), disc.pressureOutletCells_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    disc.pressureOutletCells_.erase(
        std::unique(disc.pressureOutletCells_.begin(), disc.pressureOutletCells_.end(),
                    [](const auto& a, const auto& b) {
                        return a.first == b.first && a.second == b.second;
                    }),
        disc.pressureOutletCells_.end());
}

template<std::size_t Dim>
BCDiscretization buildBCMasks(const Mesh<Dim>& mesh, const BCSpec& spec) {
    BCDiscretization disc;
    initializeBuffers(disc, mesh);

    std::vector<std::pair<std::size_t, int>> faceToWallMap;
    buildFaceToWallMap(mesh, faceToWallMap);

    if (!mesh.isUnstructured()) {
        applyStructuredCellMasks(mesh, spec, disc);
    }

    applyFaceBasedCellPinning(mesh, spec, disc);

    if (!mesh.isUnstructured()) {
        applyStructuredCornerHandling(mesh, spec, disc);
    }

    if (!mesh.isUnstructured()) {
        collectStructuredPressureOutletCells(mesh, spec, disc);
    }

    applyFaceBasedBCs(mesh, spec, disc);
    sortAndDedupPressureOutletCells(disc);

    applyDirectionBasedBCs(mesh, spec, disc, faceToWallMap);
    classifyPressureNeumannCells(mesh, disc);

    return disc;
}

template<std::size_t Dim>
void applyDirectionBasedBCs(const Mesh<Dim>& mesh, const BCSpec& spec,
                             BCDiscretization& disc,
                             const std::vector<std::pair<std::size_t, int>>& faceToWallMap) {
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        auto it = spec.faceVelBCs.find(f);
        if (it != spec.faceVelBCs.end()) {
            disc.faceBCs_[f] = it->second;
            disc.faceHasBC_[f] = 1;
            continue;
        }
        if (!mesh.isUnstructured()) {
            const auto& wallMap = faceToWallMap[f];
            if (wallMap.first != static_cast<std::size_t>(-1)) {
                std::size_t wall_d = wallMap.first;
                int sign = wallMap.second;
                const auto& bc = spec.velBcs[wallIndex(wall_d, sign)];
                assignFaceBC(disc.faceBCs_[f], bc, disc.faceHasBC_, f);
            } else {
                FaceVelocityBC& fb = disc.faceBCs_[f];
                fb.type = VelocityBCType::NoSlip;
                disc.faceHasBC_[f] = 1;
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
            // The dominant component determines which direction-based BC slot to
            // use. Apply it to every boundary face whose normal points along
            // (or near) that axis — even oblique faces must receive the BC so
            // the flow enters/leaves correctly on all mesh types.
            const bool aligned = maxA >= 0.9;
            const auto& bc = spec.velBcs[wallIndex(wall_d, sign)];
            if (aligned || bc.type == VelocityBCType::Inlet ||
                bc.type == VelocityBCType::MovingWall ||
                bc.type == VelocityBCType::Outlet ||
                bc.type == VelocityBCType::PressureOutlet) {
                assignFaceBC(disc.faceBCs_[f], bc, disc.faceHasBC_, f);
                // Pin inlet / moving wall cell-center velocity to the prescribed
                // value. applyWallBCToMasks() is skipped for unstructured meshes,
                // so this is the only path that can pin cell velocity here.
                if (bc.type == VelocityBCType::Inlet || bc.type == VelocityBCType::MovingWall) {
                    const std::size_t cell = mesh.owner(f);
                    if (cell >= mesh.nCells()) continue;  // S1: guard against invalid owner index
                    for (std::size_t d = 0; d < Dim; ++d) {
                        double val = componentAt(bc, d);
                        if (disc.dirichletMask_[d][cell] && disc.dirichletVals_[d][cell] != val) {
                            std::cerr << "WARNING: inlet cell " << cell << " component " << d
                                      << " has conflicting prescribed values\n";
                        }
                        disc.dirichletMask_[d][cell] = 1;
                        disc.dirichletVals_[d][cell] = val;
                    }
                }
            } else {
                // Oblique face for a non-prescribed BC: default to no-slip wall.
                FaceVelocityBC& fb = disc.faceBCs_[f];
                fb.type = VelocityBCType::NoSlip;
                disc.faceHasBC_[f] = 1;
            }
        }
    }
}

template<std::size_t Dim>
void classifyPressureNeumannCells(const Mesh<Dim>& mesh, BCDiscretization& disc) {
    // Cell-level pressure condition: a boundary cell is pressure-Neumann only
    // when EVERY boundary face it owns is Neumann. This must run after the
    // face BC assignment above, because that is where the fallback no-slip wall
    // is installed - and the fallback has to contribute its Neumann flag.
    std::vector<int> boundaryFaceCount(mesh.nCells(), 0);
    std::vector<int> pressureNeumannCount(mesh.nCells(), 0);
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        std::size_t P = mesh.owner(f);
        ++boundaryFaceCount[P];
        if (disc.faceBCs_[f].type != VelocityBCType::PressureOutlet) ++pressureNeumannCount[P];
    }
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        disc.isPressureNeumann_[i] = (boundaryFaceCount[i] > 0 &&
                                     pressureNeumannCount[i] == boundaryFaceCount[i]);
    }
}

template<std::size_t Dim>
BoundaryConditions<Dim>::BoundaryConditions(const Mesh<Dim>& mesh)
    : mesh_(mesh),
      velBcs_(2 * Dim),
      velPatches_(),
      faceVelBCs_() {
    for (auto& bc : velBcs_) bc = VelocityBC{};
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                                                   double val1, double val2, double val3) {
    // Velocity inlet values must be finite: a NaN/Inf inlet would corrupt the
    // momentum assembly, the pressure correction, and every downstream consumer.
    if ((type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) &&
        (!std::isfinite(val1) || !std::isfinite(val2) || !std::isfinite(val3))) {
        throw std::invalid_argument(
            "BoundaryConditions::setVelocityBC: non-finite velocity component for Inlet/MovingWall");
    }
    std::size_t idx = wallIndex(d, sign);
    velBcs_[idx] = type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall
                       ? VelocityBC{type, val1, val2, val3}
                       : VelocityBC{type, 0.0, 0.0, 0.0};
    dirty_ = true;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setPressureOutlet(std::size_t d, int sign, double pressure) {
    if (!std::isfinite(pressure)) {
        throw std::invalid_argument(
            "BoundaryConditions::setPressureOutlet: non-finite pressure value");
    }
    std::size_t idx = wallIndex(d, sign);
    velBcs_[idx].type = VelocityBCType::PressureOutlet;
    velBcs_[idx].u = 0.0;
    velBcs_[idx].v = 0.0;
    velBcs_[idx].w = 0.0;
    velBcs_[idx].pressureValue = pressure;
    dirty_ = true;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::addVelocityPatch(const std::string& name, std::size_t d, int sign,
                                                      VelocityBCType type, double u, double v, double w) {
    velPatches_.push_back({name, d, sign, type, u, v, w});
    setVelocityBC(d, sign, type, u, v, w);
}

template<std::size_t Dim>
const VelocityPatch* BoundaryConditions<Dim>::findVelocityPatch(const std::string& name) const {
    for (const auto& p : velPatches_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                                                           VelocityBCType type,
                                                           double u, double v, double w) {
    if ((type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) &&
        (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))) {
        throw std::invalid_argument(
            "BoundaryConditions::setVelocityBCOnFaces: non-finite velocity component for Inlet/MovingWall");
    }
    FaceVelocityBC fbc;
    fbc.type = type;
    fbc.u = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? u : 0.0;
    fbc.v = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? v : 0.0;
    fbc.w = (type == VelocityBCType::Inlet || type == VelocityBCType::MovingWall) ? w : 0.0;
    applyToFaces(mesh_, faceIndices, fbc, faceVelBCs_);
    dirty_ = true;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setVelocityInletNormalOnFaces(
        const std::vector<std::size_t>& faceIndices, double magnitude) {
    if (!std::isfinite(magnitude) || std::abs(magnitude) > 1.0e6) {
        throw std::invalid_argument(
            "BoundaryConditions::setVelocityInletNormalOnFaces: non-finite or extreme magnitude");
    }
    // `magnitude` is the signed inflow speed (+ = inflow along −outward normal).
    // Face velocity = -magnitude * n so a positive magnitude pushes fluid inward
    // across an outward-pointing unit normal n. Build a fresh FaceVelocityBC per
    // face so no field can carry stale values between faces.
    for (std::size_t f : faceIndices) {
        if (f >= mesh_.nFaces()) continue;
        const auto& n = mesh_.faceNormal(f);
        if (!std::isfinite(n[0]) || !std::isfinite(n[1]) ||
            (Dim >= 3 && !std::isfinite(n[2]))) {
            throw std::invalid_argument(
                "BoundaryConditions::setVelocityInletNormalOnFaces: non-finite face normal");
        }
        FaceVelocityBC fbc;
        fbc.type = VelocityBCType::Inlet;
        fbc.u = -magnitude * n[0];
        fbc.v = -magnitude * n[1];
        if constexpr (Dim >= 3) fbc.w = -magnitude * n[2];
        faceVelBCs_[f] = fbc;
    }
    dirty_ = true;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                                                  const std::string& typeStr,
                                                                  double u, double v, double w,
                                                                  bool slip,
                                                                  double pressure) {
    std::string t;
    t.reserve(typeStr.size());
    std::transform(typeStr.begin(), typeStr.end(), std::back_inserter(t),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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
void BoundaryConditions<Dim>::setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices,
                                                               double pressure) {
    if (!std::isfinite(pressure)) {
        throw std::invalid_argument(
            "BoundaryConditions::setPressureOutletOnFaces: non-finite pressure value");
    }
    FaceVelocityBC fbc;
    fbc.type = VelocityBCType::PressureOutlet;
    fbc.u = 0.0;
    fbc.v = 0.0;
    fbc.w = 0.0;
    fbc.pressureValue = pressure;
    applyToFaces(mesh_, faceIndices, fbc, faceVelBCs_);
    dirty_ = true;
}

template<std::size_t Dim>
void BoundaryConditions<Dim>::setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices,
                                                                          double pressure,
                                                                          bool suppressBackflow,
                                                                          double backflowU, double backflowV, double backflowW,
                                                                          double backflowK, double backflowOmega) {
    if (!std::isfinite(pressure)) {
        throw std::invalid_argument(
            "BoundaryConditions::setPressureOutletOnFacesWithBackflow: non-finite pressure value");
    }
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
    applyToFaces(mesh_, faceIndices, fbc, faceVelBCs_);
    dirty_ = true;
}

template<std::size_t Dim>
double BoundaryConditions<Dim>::maxInletSpeed(double fallback) const {
    double maxS = 0.0;
    for (std::size_t i = 0; i < 2 * Dim; ++i) {
        if (velBcs_[i].type == VelocityBCType::Inlet) {
            double spd = speedMagnitude(velBcs_[i].u, velBcs_[i].v, velBcs_[i].w);
            maxS = std::max(maxS, spd);
        }
    }
    for (const auto& [faceIdx, fbc] : faceVelBCs_) {
        if (fbc.type == VelocityBCType::Inlet) {
            double spd = speedMagnitude(fbc.u, fbc.v, fbc.w);
            maxS = std::max(maxS, spd);
        }
    }
    if (maxS < 1e-12) maxS = fallback;
    return maxS;
}

template<std::size_t Dim>
const BCDiscretization& BoundaryConditions<Dim>::discretization() const {
    if (dirty_) {
        discretization_ = buildBCMasks(mesh_, {velBcs_, velPatches_, faceVelBCs_});
        dirty_ = false;
    }
    return discretization_;
}

template <std::size_t Dim>
double BoundaryConditions<Dim>::maxPrescribedSpeed(double fallback) const {
    double maxS = 0.0;
    auto consider = [&](double u, double v, double w) {
        double s = speedMagnitudeSquared(u, v, w);
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
void BoundaryConditions<Dim>::debugPrint(const BCDiscretization& disc) const {
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
        if (disc.faceBCs_[f].type != VelocityBCType::PressureOutlet) ++neumannFaceCount;
    }
    std::size_t pressureOutletFaceCount = 0;
    for (const auto& fb : disc.faceBCs_)
        if (fb.type == VelocityBCType::PressureOutlet) ++pressureOutletFaceCount;
    std::cout << "  [BC] faceVelBCs=" << faceVelBCCount
              << " dirichlet U=" << dirichletCount[0] << " V=" << dirichletCount[1];
    if constexpr (Dim == 3) std::cout << " W=" << dirichletCount[2];
    std::cout << " pressureNeumann cells=" << neumannCount
              << " faces=" << neumannFaceCount
              << " pressureOutlet cells=" << disc.pressureOutletCells_.size()
              << " faces=" << pressureOutletFaceCount << "\n";
}

template class BoundaryConditions<2>;
template class BoundaryConditions<3>;

} // namespace fvm
