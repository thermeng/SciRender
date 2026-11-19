#pragma once

// CGNS test fixtures: small known meshes written with the CGNS mid-level API,
// so the reader tests need no binary fixtures in the repo. Header-only and
// compiled only when CGNS is available (FvmCore defines SCIRENDER_HAVE_CGNS
// PUBLIC when the vendored stack is found).

#ifdef SCIRENDER_HAVE_CGNS

#include <cgnslib.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace cgns_fixtures {

inline void ck(int rc, const char* ctx) {
    if (rc != CG_OK) {
        throw std::runtime_error(std::string("cgns fixture: ") + ctx + ": " + cg_get_error());
    }
}

// 2x2x2 unit hex cube: 27 vertices, 8 cells, 6 wall BCs of 4 faces each.
// BC point lists concatenate the 4 faces (16 points) to exercise the reader's
// multi-face chunk matching.
inline void writeHexCube(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0, BC = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[3] = {27, 8, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");

    auto pid = [](int i, int j, int k) { return i + 3 * j + 9 * k; };
    std::vector<double> x(27), y(27), z(27);
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const int p = pid(i, j, k);
                x[p] = i; y[p] = j; z[p] = k;
            }
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x.data(), &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y.data(), &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z.data(), &C), "coordZ");

    std::vector<cgsize_t> conn;
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int bot[4] = {pid(i,j,k), pid(i+1,j,k), pid(i+1,j+1,k), pid(i,j+1,k)};
                const int top[4] = {pid(i,j,k+1), pid(i+1,j,k+1), pid(i+1,j+1,k+1), pid(i,j+1,k+1)};
                for (int v : bot) conn.push_back(v + 1);
                for (int v : top) conn.push_back(v + 1);
            }
    ck(cg_section_write(fn, B, Z, "Hex", CGNS_ENUMV(HEXA_8), 1, 8, 0, conn.data(), &S), "hex");

    auto addBC = [&](const char* name, const std::vector<std::array<int, 4>>& faces) {
        std::vector<cgsize_t> pts;
        for (const auto& f : faces)
            for (int v : f) pts.push_back(v + 1);
        ck(cg_boco_write(fn, B, Z, name, CGNS_ENUMV(BCWall), CGNS_ENUMV(PointList),
                         static_cast<cgsize_t>(pts.size()), pts.data(), &BC), "boco");
    };
    std::vector<std::array<int, 4>> lo, hi;
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            lo.push_back({pid(0,a,b), pid(0,a+1,b), pid(0,a+1,b+1), pid(0,a,b+1)});
            hi.push_back({pid(2,a,b), pid(2,a+1,b), pid(2,a+1,b+1), pid(2,a,b+1)});
        }
    addBC("xminus", lo); addBC("xplus", hi);
    lo.clear(); hi.clear();
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            lo.push_back({pid(a,0,b), pid(a+1,0,b), pid(a+1,0,b+1), pid(a,0,b+1)});
            hi.push_back({pid(a,2,b), pid(a+1,2,b), pid(a+1,2,b+1), pid(a,2,b+1)});
        }
    addBC("yminus", lo); addBC("yplus", hi);
    lo.clear(); hi.clear();
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            lo.push_back({pid(a,b,0), pid(a+1,b,0), pid(a+1,b+1,0), pid(a,b+1,0)});
            hi.push_back({pid(a,b,2), pid(a+1,b,2), pid(a+1,b+1,2), pid(a,b+1,2)});
        }
    addBC("zminus", lo); addBC("zplus", hi);
    ck(cg_close(fn), "close");
}

// One tet, one wedge, one pyramid in separate sections (no BCs). Exercises
// the explicit canonical face loops for the non-inferred cell types.
inline void writeMixedCells(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[3] = {15, 3, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    const double pts[15][3] = {
        {0,0,0}, {1,0,0}, {0,1,0}, {0,0,1},                     // tet
        {2,0,0}, {3,0,0}, {2,1,0}, {2,0,1}, {3,0,1}, {2,1,1},   // wedge
        {4,0,0}, {5,0,0}, {5,1,0}, {4,1,0}, {4.5,0.5,1},        // pyramid
    };
    std::vector<double> x(15), y(15), z(15);
    for (int i = 0; i < 15; ++i) { x[i] = pts[i][0]; y[i] = pts[i][1]; z[i] = pts[i][2]; }
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x.data(), &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y.data(), &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z.data(), &C), "coordZ");
    const cgsize_t tet[4] = {1, 2, 3, 4};
    const cgsize_t wedge[6] = {5, 6, 7, 8, 9, 10};
    const cgsize_t pyr[5] = {11, 12, 13, 14, 15};
    ck(cg_section_write(fn, B, Z, "Tet", CGNS_ENUMV(TETRA_4), 1, 1, 0, tet, &S), "tet");
    ck(cg_section_write(fn, B, Z, "Wedge", CGNS_ENUMV(PENTA_6), 2, 2, 0, wedge, &S), "wedge");
    ck(cg_section_write(fn, B, Z, "Pyramid", CGNS_ENUMV(PYRA_5), 3, 3, 0, pyr, &S), "pyr");
    ck(cg_close(fn), "close");
}

// 2x2 unit quads, 9 vertices, 4 edge BCs. "bottom" uses ElementList (edge
// elements 5,6), the others concatenated PointList pairs.
inline void write2DQuad(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0, BC = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 2, 2, &B), "base");
    const cgsize_t size[3] = {9, 4, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    auto pid = [](int i, int j) { return i + 3 * j; };
    std::vector<double> x(9), y(9);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) { const int p = pid(i,j); x[p] = i; y[p] = j; }
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x.data(), &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y.data(), &C), "coordY");
    std::vector<cgsize_t> conn;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            conn.push_back(pid(i,j)+1); conn.push_back(pid(i+1,j)+1);
            conn.push_back(pid(i+1,j+1)+1); conn.push_back(pid(i,j+1)+1);
        }
    ck(cg_section_write(fn, B, Z, "Quad", CGNS_ENUMV(QUAD_4), 1, 4, 0, conn.data(), &S), "quad");
    const cgsize_t edges[8][2] = {
        {1,2}, {2,3},   // bottom (elements 5,6)
        {3,6}, {6,9},   // right  (7,8)
        {9,8}, {8,7},   // top    (9,10)
        {7,4}, {4,1},   // left   (11,12)
    };
    std::vector<cgsize_t> econn;
    for (const auto& e : edges) { econn.push_back(e[0]); econn.push_back(e[1]); }
    // nbndry=1: ElementList BCs may only reference boundary-flagged elements.
    ck(cg_section_write(fn, B, Z, "Edges", CGNS_ENUMV(BAR_2), 5, 12, 1, econn.data(), &S), "edges");

    const cgsize_t bottomElems[2] = {5, 6};
    ck(cg_boco_write(fn, B, Z, "bottom", CGNS_ENUMV(BCWall), CGNS_ENUMV(ElementList),
                     2, bottomElems, &BC), "bottom");
    const cgsize_t right[4] = {3, 6, 6, 9};
    ck(cg_boco_write(fn, B, Z, "right", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointList),
                     4, right, &BC), "right");
    const cgsize_t top[4] = {9, 8, 8, 7};
    ck(cg_boco_write(fn, B, Z, "top", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointList),
                     4, top, &BC), "top");
    const cgsize_t left[4] = {7, 4, 4, 1};
    ck(cg_boco_write(fn, B, Z, "left", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointList),
                     4, left, &BC), "left");
    ck(cg_close(fn), "close");
}

// One hex + one tet + one boundary quad in a single MIXED section, with an
// element-based BC. Exercises the type-tagged stream parser.
inline void writeMixedSection(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0, BC = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[3] = {12, 2, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    const double pts[12][3] = {
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1},
        {2,0,0}, {3,0,0}, {2,1,0}, {2,0,1},
    };
    std::vector<double> x(12), y(12), z(12);
    for (int i = 0; i < 12; ++i) { x[i] = pts[i][0]; y[i] = pts[i][1]; z[i] = pts[i][2]; }
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x.data(), &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y.data(), &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z.data(), &C), "coordZ");
    // stream: hex #1, tet #2, boundary quad #3 (hex's x=0 face: verts 1,4,8,5)
    const cgsize_t stream[] = {
        CGNS_ENUMV(HEXA_8), 1, 2, 3, 4, 5, 6, 7, 8,
        CGNS_ENUMV(TETRA_4), 9, 10, 11, 12,
        CGNS_ENUMV(QUAD_4), 1, 4, 8, 5,
    };
    ck(cg_poly_section_write(fn, B, Z, "Mixed", CGNS_ENUMV(MIXED), 1, 3, 0,
                             stream, nullptr, &S), "mixed");
    const cgsize_t bcElems[1] = {3};
    ck(cg_boco_write(fn, B, Z, "xminus", CGNS_ENUMV(BCWall), CGNS_ENUMV(ElementList),
                     1, bcElems, &BC), "boco");
    ck(cg_close(fn), "close");
}

// Surface mesh in 3D space (cellDim=2, physDim=3, non-zero Z): must be
// rejected, not silently flattened to 2D.
inline void writeSurface3D(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 2, 3, &B), "base");
    const cgsize_t size[3] = {4, 1, 0};
    ck(cg_zone_write(fn, B, "surface_body", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    const double x[4] = {0, 1, 1, 0};
    const double y[4] = {0, 0, 1, 1};
    const double z[4] = {0, 0, 0.5, 0.5}; // tilted out of plane
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x, &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y, &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z, &C), "coordZ");
    const cgsize_t quad[4] = {1, 2, 3, 4};
    ck(cg_section_write(fn, B, Z, "quads", CGNS_ENUMV(QUAD_4), 1, 1, 0, quad, &S), "quads");
    ck(cg_close(fn), "close");
}

// PointRange BCs on an unstructured zone: element-index ranges (FaceCenter)
// and a vertex-index range (Vertex), the StarCCM+ export convention.
inline void writePointRangeBCs(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0, BC = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[3] = {8, 1, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    const double pts[8][3] = {
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1},
    };
    std::vector<double> x(8), y(8), z(8);
    for (int i = 0; i < 8; ++i) { x[i] = pts[i][0]; y[i] = pts[i][1]; z[i] = pts[i][2]; }
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x.data(), &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y.data(), &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z.data(), &C), "coordZ");
    const cgsize_t hex[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ck(cg_section_write(fn, B, Z, "hex", CGNS_ENUMV(HEXA_8), 1, 1, 0, hex, &S), "hex");
    // boundary quads: element #2 = xminus {1,4,8,5}, #3 = zminus {1,2,3,4}
    const cgsize_t quads[8] = {1, 4, 8, 5, 1, 2, 3, 4};
    ck(cg_section_write(fn, B, Z, "bcfaces", CGNS_ENUMV(QUAD_4), 2, 3, 0, quads, &S), "quads");
    const cgsize_t r1[2] = {2, 2};
    ck(cg_boco_write(fn, B, Z, "xside", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointRange), 2, r1, &BC), "boco1");
    ck(cg_boco_gridlocation_write(fn, B, Z, BC, CGNS_ENUMV(FaceCenter)), "loc1");
    const cgsize_t r2[2] = {3, 3};
    ck(cg_boco_write(fn, B, Z, "bottom", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointRange), 2, r2, &BC), "boco2");
    ck(cg_boco_gridlocation_write(fn, B, Z, BC, CGNS_ENUMV(FaceCenter)), "loc2");
    const cgsize_t r3[2] = {1, 4}; // vertices 1..4 = the zminus face
    ck(cg_boco_write(fn, B, Z, "vrange", CGNS_ENUMV(BCWall), CGNS_ENUMV(PointRange), 2, r3, &BC), "boco3");
    ck(cg_boco_gridlocation_write(fn, B, Z, BC, CGNS_ENUMV(Vertex)), "loc3");
    ck(cg_close(fn), "close");
}

// Structured zone: must be rejected with a clear diagnostic.
inline void writeStructured(const std::string& path) {
    int fn = 0, B = 0, Z = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[9] = {3,3,3, 2,2,2, 0,0,0};
    ck(cg_zone_write(fn, B, "Structured", size, CGNS_ENUMV(Structured), &Z), "zone");
    ck(cg_close(fn), "close");
}

// Polyhedral NGon section: must be rejected with a clear diagnostic.
inline void writePolyhedral(const std::string& path) {
    int fn = 0, B = 0, Z = 0, C = 0, S = 0;
    ck(cg_open(path.c_str(), CG_MODE_WRITE, &fn), "open");
    ck(cg_base_write(fn, "Base", 3, 3, &B), "base");
    const cgsize_t size[3] = {4, 1, 0};
    ck(cg_zone_write(fn, B, "Zone", size, CGNS_ENUMV(Unstructured), &Z), "zone");
    const double x[4] = {0,1,1,0}, y[4] = {0,0,1,1}, z[4] = {0,0,0,0};
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateX", x, &C), "coordX");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateY", y, &C), "coordY");
    ck(cg_coord_write(fn, B, Z, CGNS_ENUMV(RealDouble), "CoordinateZ", z, &C), "coordZ");
    const cgsize_t ngon[5] = {4, 1, 2, 3, 4};  // one quad face
    const cgsize_t offset[1] = {0};
    ck(cg_poly_section_write(fn, B, Z, "Faces", CGNS_ENUMV(NGON_n), 1, 1, 0,
                             ngon, offset, &S), "ngon");
    ck(cg_close(fn), "close");
}

} // namespace cgns_fixtures

#endif // SCIRENDER_HAVE_CGNS
