#include "io/CGNSReader.h"

#include "core/cell_topology.h"

#include <cgnslib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fvm {
namespace {

void check(int rc, const char* ctx) {
    if (rc != CG_OK) {
        throw std::runtime_error(std::string("CGNSReader: ") + ctx + ": " + cg_get_error());
    }
}

struct File {
    int fn = -1;
    explicit File(const std::string& path) {
        if (cg_open(path.c_str(), CG_MODE_READ, &fn) != CG_OK) {
            throw std::runtime_error("CGNSReader: cannot open '" + path + "': " + cg_get_error());
        }
    }
    ~File() { if (fn >= 0) cg_close(fn); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

// First unstructured zone across all bases. Structured zones are rejected:
// the CFD mesh model is cell/face based and has no IJK mapping for them yet.
void firstUnstructuredZone(int fn, int& B, int& Z, cgsize_t* size) {
    int nbases = 0;
    check(cg_nbases(fn, &nbases), "cg_nbases");
    if (nbases < 1) throw std::runtime_error("CGNSReader: file has no CGNSBase_t");
    for (int b = 1; b <= nbases; ++b) {
        int nzones = 0;
        check(cg_nzones(fn, b, &nzones), "cg_nzones");
        for (int z = 1; z <= nzones; ++z) {
            ZoneType_t zt = static_cast<ZoneType_t>(0);
            check(cg_zone_type(fn, b, z, &zt), "cg_zone_type");
            if (zt != CGNS_ENUMV(Unstructured)) continue;
            char name[33];
            check(cg_zone_read(fn, b, z, name, size), "cg_zone_read");
            B = b; Z = z;
            return;
        }
    }
    throw std::runtime_error("CGNSReader: no unstructured zone found "
                             "(structured CGNS zones are not supported)");
}

enum class ElemRole { Volume, BoundaryFace, Ignore, Unsupported };

struct ElemInfo {
    ElemRole role = ElemRole::Ignore;
    int npe = 0;                 // nodes per element
    std::uint8_t vtkType = 0;    // cell_topology type for 3D volume cells
};

// CGNS linear element types in VTK node order. `dim` decides whether TRI/QUAD
// sections are volume cells (2D) or boundary faces (3D).
ElemInfo classify(ElementType_t t, int dim) {
    switch (t) {
        case CGNS_ENUMV(TETRA_4):   return {ElemRole::Volume, 4, cell_topology::kTetra};
        case CGNS_ENUMV(PYRA_5): return {ElemRole::Volume, 5, cell_topology::kPyramid};
        case CGNS_ENUMV(PENTA_6):   return {ElemRole::Volume, 6, cell_topology::kWedge};
        case CGNS_ENUMV(HEXA_8):    return {ElemRole::Volume, 8, cell_topology::kHexahedron};
        case CGNS_ENUMV(TRI_3):
            return dim == 3 ? ElemInfo{ElemRole::BoundaryFace, 3, 0}
                            : ElemInfo{ElemRole::Volume, 3, 0};
        case CGNS_ENUMV(QUAD_4):
            return dim == 3 ? ElemInfo{ElemRole::BoundaryFace, 4, 0}
                            : ElemInfo{ElemRole::Volume, 4, 0};
        case CGNS_ENUMV(BAR_2):
            // 2D boundary "faces" are edges; in 3D they carry no face info.
            return dim == 2 ? ElemInfo{ElemRole::BoundaryFace, 2, 0}
                            : ElemInfo{ElemRole::Ignore, 2, 0};
        case CGNS_ENUMV(NODE):
            return {ElemRole::Ignore, 0, 0};
        case CGNS_ENUMV(NGON_n):
        case CGNS_ENUMV(NFACE_n):
            throw std::runtime_error("CGNSReader: polyhedral NGon/NFace elements "
                                     "are not supported");
        default:
            // Higher-order variants (TETRA_10, HEXA_20/27, TRI_6, ...) and
            // anything exotic: reject rather than silently drop cells.
            throw std::runtime_error("CGNSReader: unsupported element type " +
                                     std::to_string(static_cast<int>(t)) +
                                     " (only linear TETRA_4/PYRA_5/PENTA_6/HEXA_8"
                                     " and TRI_3/QUAD_4 are supported)");
    }
}

bool isVolumeSection(ElementType_t t) {
    switch (t) {
        case CGNS_ENUMV(TETRA_4):
        case CGNS_ENUMV(PYRA_5):
        case CGNS_ENUMV(PENTA_6):
        case CGNS_ENUMV(HEXA_8):
        case CGNS_ENUMV(NGON_n):
        case CGNS_ENUMV(NFACE_n):
            return true;
        default:
            return false;
    }
}

struct Section {
    ElementType_t type = CGNS_ENUMV(ElementTypeNull);
    cgsize_t start = 0, end = 0;
    std::vector<cgsize_t> conn;   // flat, 1-based, npe per element
};

std::vector<Section> readSections(int fn, int B, int Z, bool connectivity) {
    int nsections = 0;
    check(cg_nsections(fn, B, Z, &nsections), "cg_nsections");
    std::vector<Section> out;
    out.reserve(static_cast<std::size_t>(nsections));
    for (int s = 1; s <= nsections; ++s) {
        Section sec;
        char name[33];
        int nbndry = 0, pflag = 0;
        check(cg_section_read(fn, B, Z, s, name, &sec.type, &sec.start, &sec.end,
                              &nbndry, &pflag), "cg_section_read");
        if (connectivity && sec.type != CGNS_ENUMV(NGON_n) &&
            sec.type != CGNS_ENUMV(NFACE_n)) {
            // Polyhedral sections cannot be read with cg_elements_read;
            // classify() rejects them below, so leave the connectivity empty
            // rather than failing inside the CGNS call. MIXED is a flat
            // [type, nodes...] stream: cg_poly_elements_read returns it raw.
            cgsize_t dsz = 0;
            check(cg_ElementDataSize(fn, B, Z, s, &dsz), "cg_ElementDataSize");
            sec.conn.resize(static_cast<std::size_t>(dsz));
            if (sec.type == CGNS_ENUMV(MIXED)) {
                check(cg_poly_elements_read(fn, B, Z, s, sec.conn.data(),
                                            nullptr, nullptr),
                      "cg_poly_elements_read (MIXED)");
            } else {
                check(cg_elements_read(fn, B, Z, s, sec.conn.data(), nullptr),
                      "cg_elements_read");
            }
        }
        out.push_back(std::move(sec));
    }
    return out;
}

struct VecHash {
    std::size_t operator()(const std::vector<std::size_t>& v) const noexcept {
        std::size_t h = 1469598103934665603ull;
        for (std::size_t x : v) {
            h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        }
        return h;
    }
};

std::vector<std::size_t> sortedKey(const cgsize_t* p, std::size_t n) {
    std::vector<std::size_t> key(n);
    for (std::size_t i = 0; i < n; ++i) key[i] = static_cast<std::size_t>(p[i] - 1);
    std::sort(key.begin(), key.end());
    return key;
}

} // namespace

template<std::size_t Dim>
int CGNSReader<Dim>::detectDimension(const std::string& filename) {
    File f(filename);
    int B = 0, Z = 0;
    cgsize_t size[3] = {0, 0, 0};
    firstUnstructuredZone(f.fn, B, Z, size);
    const auto sections = readSections(f.fn, B, Z, /*connectivity=*/false);
    if (sections.empty()) {
        throw std::runtime_error("CGNSReader: zone has no element sections");
    }
    for (const auto& s : sections) {
        if (isVolumeSection(s.type)) return 3;
    }
    // MIXED sections carry the dimension in their per-element type tags.
    bool hasMixed = false;
    for (const auto& s : sections) hasMixed |= s.type == CGNS_ENUMV(MIXED);
    if (hasMixed) {
        const auto full = readSections(f.fn, B, Z, /*connectivity=*/true);
        for (const auto& s : full) {
            if (s.type != CGNS_ENUMV(MIXED)) continue;
            std::size_t pos = 0;
            while (pos < s.conn.size()) {
                const auto et = static_cast<ElementType_t>(s.conn[pos++]);
                if (isVolumeSection(et)) return 3;
                int npe = 0;
                check(cg_npe(et, &npe), "cg_npe (MIXED)");
                if (npe <= 0 || pos + static_cast<std::size_t>(npe) > s.conn.size()) {
                    throw std::runtime_error("CGNSReader: malformed MIXED element stream");
                }
                pos += static_cast<std::size_t>(npe);
            }
        }
    }
    return 2;
}

template<std::size_t Dim>
typename CGNSReader<Dim>::Result CGNSReader<Dim>::read(const std::string& filename) {
    File f(filename);
    int B = 0, Z = 0;
    cgsize_t size[3] = {0, 0, 0};
    firstUnstructuredZone(f.fn, B, Z, size);
    const cgsize_t nvert = size[0];
    if (nvert <= 0) throw std::runtime_error("CGNSReader: zone has no vertices");

    // --- coordinates (first Dim arrays: X, Y [, Z]) ---
    int ncoords = 0;
    check(cg_ncoords(f.fn, B, Z, &ncoords), "cg_ncoords");
    if (ncoords < static_cast<int>(Dim)) {
        throw std::runtime_error("CGNSReader: zone has " + std::to_string(ncoords) +
                                 " coordinate arrays, need " + std::to_string(Dim));
    }
    std::vector<std::array<double, Dim>> vertices(static_cast<std::size_t>(nvert));
    if constexpr (Dim == 2) {
        // 2D cells with a third coordinate are a surface mesh in 3D space
        // (e.g. a StarCCM+ boundary export), not a CFD volume mesh. Reading
        // it as Mesh<2> would silently drop Z, so reject unless truly planar.
        if (ncoords >= 3) {
            DataType_t dt = CGNS_ENUMV(DataTypeNull);
            char cname[33];
            check(cg_coord_info(f.fn, B, Z, 3, &dt, cname), "cg_coord_info");
            const cgsize_t rmin[3] = {1, 1, 1};
            const cgsize_t rmax[3] = {nvert, 1, 1};
            std::vector<double> zbuf(static_cast<std::size_t>(nvert));
            check(cg_coord_read(f.fn, B, Z, cname, CGNS_ENUMV(RealDouble), rmin, rmax,
                                zbuf.data()), "cg_coord_read");
            double zmax = 0.0;
            for (double zv : zbuf) zmax = std::max(zmax, std::fabs(zv));
            if (zmax > 1e-9) {
                throw std::runtime_error(
                    "CGNSReader: this file is a surface mesh (2D cells in 3D "
                    "space with non-zero Z). CFD needs a volume mesh - export "
                    "the volume region, not the boundary surface.");
            }
        }
    }
    for (int c = 1; c <= static_cast<int>(Dim); ++c) {
        DataType_t dt = CGNS_ENUMV(DataTypeNull);
        char cname[33];
        check(cg_coord_info(f.fn, B, Z, c, &dt, cname), "cg_coord_info");
        const cgsize_t rmin[3] = {1, 1, 1};
        const cgsize_t rmax[3] = {nvert, 1, 1};
        std::vector<double> buf(static_cast<std::size_t>(nvert));
        check(cg_coord_read(f.fn, B, Z, cname, CGNS_ENUMV(RealDouble), rmin, rmax,
                            buf.data()), "cg_coord_read");
        for (cgsize_t i = 0; i < nvert; ++i) {
            vertices[static_cast<std::size_t>(i)][static_cast<std::size_t>(c - 1)] =
                buf[static_cast<std::size_t>(i)];
        }
    }

    // --- elements ---
    const auto sections = readSections(f.fn, B, Z, /*connectivity=*/true);
    std::vector<std::vector<std::size_t>> cells;
    std::vector<std::vector<std::vector<std::size_t>>> cellFaces;
    // Boundary elements by global (1-based) element number, for ElementList BCs.
    std::unordered_map<cgsize_t, std::vector<std::size_t>> boundaryElems;
    auto addCell = [&](const cgsize_t* p, const ElemInfo& info) {
        std::vector<std::size_t> corners(static_cast<std::size_t>(info.npe));
        for (int k = 0; k < info.npe; ++k) {
            const cgsize_t v = p[k];
            if (v < 1 || v > nvert) {
                throw std::runtime_error("CGNSReader: element vertex out of range");
            }
            corners[static_cast<std::size_t>(k)] = static_cast<std::size_t>(v - 1);
        }
        cells.push_back(corners);
        if constexpr (Dim == 3) {
            // Explicit canonical face loops: Mesh infers only tet/hex, and
            // wedge/pyramid need their topology spelled out.
            const auto& localFaces = cell_topology::faces(info.vtkType);
            std::vector<std::vector<std::size_t>> loops;
            loops.reserve(localFaces.size());
            for (const auto& lf : localFaces) {
                std::vector<std::size_t> loop;
                loop.reserve(lf.size());
                for (int slot : lf) loop.push_back(corners[static_cast<std::size_t>(slot)]);
                loops.push_back(std::move(loop));
            }
            cellFaces.push_back(std::move(loops));
        }
    };
    for (const auto& sec : sections) {
        if (sec.type == CGNS_ENUMV(MIXED)) {
            // Flat stream: [ElementType, node ids...] per element. Element
            // numbers continue the section's start..end range in order.
            std::size_t pos = 0;
            cgsize_t elemNo = sec.start;
            while (pos < sec.conn.size()) {
                const auto et = static_cast<ElementType_t>(sec.conn[pos++]);
                int npe = 0;
                check(cg_npe(et, &npe), "cg_npe (MIXED)");
                if (npe <= 0 || pos + static_cast<std::size_t>(npe) > sec.conn.size()) {
                    throw std::runtime_error("CGNSReader: malformed MIXED element stream");
                }
                const cgsize_t* p = sec.conn.data() + pos;
                pos += static_cast<std::size_t>(npe);
                const ElemInfo info = classify(et, static_cast<int>(Dim));
                if (info.role == ElemRole::BoundaryFace) {
                    boundaryElems.emplace(elemNo, sortedKey(p, info.npe));
                } else if (info.role == ElemRole::Volume) {
                    addCell(p, info);
                }
                ++elemNo;
            }
            continue;
        }
        const ElemInfo info = classify(sec.type, static_cast<int>(Dim));
        if (info.role == ElemRole::Ignore) continue;
        const cgsize_t count = sec.end - sec.start + 1;
        if (count <= 0) continue;
        if (sec.conn.size() != static_cast<std::size_t>(count) * info.npe) {
            throw std::runtime_error("CGNSReader: element section size mismatch");
        }
        for (cgsize_t e = 0; e < count; ++e) {
            const cgsize_t* p = sec.conn.data() + static_cast<std::size_t>(e) * info.npe;
            if (info.role == ElemRole::BoundaryFace) {
                boundaryElems.emplace(sec.start + e, sortedKey(p, info.npe));
                continue;
            }
            addCell(p, info);
        }
    }
    if (cells.empty()) {
        throw std::runtime_error("CGNSReader: no volume elements found in '" + filename + "'");
    }

    Mesh<Dim> mesh(std::move(vertices), std::move(cells), std::move(cellFaces));

    // --- boundary conditions: match BC point sets to boundary faces ---
    std::unordered_map<std::vector<std::size_t>, std::size_t, VecHash> faceByKey;
    for (std::size_t fc = 0; fc < mesh.nFaces(); ++fc) {
        if (mesh.neighbor(fc) != std::size_t(-1)) continue;
        const auto& fv = mesh.faceVertices(fc);
        std::vector<std::size_t> key(fv.begin(), fv.end());
        std::sort(key.begin(), key.end());
        faceByKey.emplace(std::move(key), fc);
    }

    std::map<std::string, std::vector<std::size_t>> groups;
    int nbocos = 0;
    check(cg_nbocos(f.fn, B, Z, &nbocos), "cg_nbocos");
    for (int bc = 1; bc <= nbocos; ++bc) {
        char bname[33];
        BCType_t btype = CGNS_ENUMV(BCTypeNull);
        PointSetType_t pst = CGNS_ENUMV(PointSetTypeNull);
        cgsize_t npnts = 0, nlsz = 0;
        int nidx = 0, ndset = 0;
        DataType_t ndt = CGNS_ENUMV(DataTypeNull);
        check(cg_boco_info(f.fn, B, Z, bc, bname, &btype, &pst, &npnts,
                           &nidx, &nlsz, &ndt, &ndset), "cg_boco_info");
        if (npnts <= 0) continue;
        std::vector<cgsize_t> pnts(static_cast<std::size_t>(npnts));
        check(cg_boco_read(f.fn, B, Z, bc, pnts.data(), nullptr), "cg_boco_read");
        const std::string group = bname[0] ? std::string(bname)
                                           : "BC_" + std::to_string(bc);
        auto& faces = groups[group];

        auto matchKey = [&](const std::vector<std::size_t>& key) {
            auto it = faceByKey.find(key);
            if (it == faceByKey.end()) {
                throw std::runtime_error("CGNSReader: BC '" + group +
                                         "' references a vertex set that is not a "
                                         "boundary face of the mesh");
            }
            faces.push_back(it->second);
        };

        // CGNS 4.x stores element-based BCs as PointList + GridLocation
        // FaceCenter/EdgeCenter (only legacy ADF2 keeps ElementList), so the
        // grid location decides whether the list holds vertices or elements.
        GridLocation_t loc = CGNS_ENUMV(Vertex);
        if (cg_boco_gridlocation_read(f.fn, B, Z, bc, &loc) != CG_OK) {
            loc = CGNS_ENUMV(Vertex); // absent node: vertex semantics
        }
        const bool elementBased =
            pst == CGNS_ENUMV(ElementList) ||
            ((pst == CGNS_ENUMV(PointList) || pst == CGNS_ENUMV(PointRange)) &&
             (loc == CGNS_ENUMV(FaceCenter) || loc == CGNS_ENUMV(EdgeCenter)));

        if (pst == CGNS_ENUMV(PointRange)) {
            // A contiguous index range [start, end] — valid on unstructured
            // zones too; StarCCM+ exports boundary-element ranges this way.
            if (npnts != 2 || pnts[0] < 1 || pnts[1] < pnts[0]) {
                throw std::runtime_error("CGNSReader: BC '" + group +
                                         "' has a malformed PointRange");
            }
            const cgsize_t rstart = pnts[0], rend = pnts[1];
            if (elementBased) {
                for (cgsize_t idx = rstart; idx <= rend; ++idx) {
                    auto it = boundaryElems.find(idx);
                    if (it == boundaryElems.end()) {
                        throw std::runtime_error("CGNSReader: BC '" + group +
                                                 "' references unknown boundary element " +
                                                 std::to_string(idx));
                    }
                    matchKey(it->second);
                }
            } else {
                // Vertex range: every boundary face whose vertices all lie
                // in the range. Keys are sorted, so front/back bound the set.
                for (const auto& [key, fc] : faceByKey) {
                    if (key.front() + 1 >= rstart && key.back() + 1 <= rend) {
                        faces.push_back(fc);
                    }
                }
            }
        } else if (elementBased) {
            for (cgsize_t idx : pnts) {
                auto it = boundaryElems.find(idx);
                if (it == boundaryElems.end()) {
                    throw std::runtime_error("CGNSReader: BC '" + group +
                                             "' references unknown boundary element " +
                                             std::to_string(idx));
                }
                matchKey(it->second);
            }
        } else if (pst == CGNS_ENUMV(PointList)) {
            // One or more faces concatenated: try tri then quad chunks in 3D,
            // edges in 2D. CGNS does not record the face size in the BC node.
            std::array<int, 2> sizes = Dim == 3 ? std::array<int, 2>{3, 4}
                                                : std::array<int, 2>{2, 0};
            cgsize_t pos = 0;
            while (pos < npnts) {
                bool matched = false;
                for (int k : sizes) {
                    if (k == 0 || pos + k > npnts) continue;
                    auto key = sortedKey(pnts.data() + pos, static_cast<std::size_t>(k));
                    if (faceByKey.count(key)) {
                        matchKey(key);
                        pos += k;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    throw std::runtime_error("CGNSReader: BC '" + group +
                                             "' point list does not match any "
                                             "boundary face at offset " +
                                             std::to_string(pos));
                }
            }
        } else {
            throw std::runtime_error("CGNSReader: BC '" + group +
                                     "' uses an unsupported point set type");
        }
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    }
    mesh.setBoundaryGroups(groups);
    return {std::move(mesh), std::move(groups)};
}

template class CGNSReader<2>;
template class CGNSReader<3>;

} // namespace fvm
