#include "io/ANSYSMeshReader.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fvm {

// ============================================================================
// Tokenizer
// ============================================================================

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Token
ANSYSMeshReader<Dim>::Tokenizer::next() {
    if (hasBuffer_) {
        hasBuffer_ = false;
        return buffer_;
    }
    return readToken();
}

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Token
ANSYSMeshReader<Dim>::Tokenizer::peek() {
    if (!hasBuffer_) {
        buffer_ = readToken();
        hasBuffer_ = true;
    }
    return buffer_;
}

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Token
ANSYSMeshReader<Dim>::Tokenizer::readToken() {
    int c;
    while (true) {
        c = in_.get();
        if (c == std::char_traits<char>::eof()) return {Token::End, ""};
        if (!std::isspace(static_cast<unsigned char>(c))) break;
    }

    if (c == '(') return {Token::LParen, "("};
    if (c == ')') return {Token::RParen, ")"};

    if (c == '"') {
        std::string s;
        while (true) {
            c = in_.get();
            if (c == std::char_traits<char>::eof()) {
                throw std::runtime_error("ANSYSMeshReader: unterminated string");
            }
            if (c == '"') break;
            s += static_cast<char>(c);
        }
        return {Token::String, s};
    }

    std::string word;
    word += static_cast<char>(c);
    while (true) {
        c = in_.get();
        if (c == std::char_traits<char>::eof()) break;
        if (std::isspace(static_cast<unsigned char>(c))) break;
        if (c == '(' || c == ')' || c == '"') {
            in_.unget();
            break;
        }
        word += static_cast<char>(c);
    }
    return classify(word);
}

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Token
ANSYSMeshReader<Dim>::Tokenizer::classify(const std::string& word) {
    if (word.empty()) return {Token::End, word};

    bool hasDot = false;
    bool hasExp = false;
    bool hasHex = false;
    std::size_t start = 0;

    if (word[0] == '-' || word[0] == '+') {
        if (word.size() == 1) return {Token::Integer, word};
        start = 1;
    }

    for (std::size_t i = start; i < word.size(); ++i) {
        char c = word[i];
        if (c == '.') hasDot = true;
        else if (c == 'e' || c == 'E') hasExp = true;
        else if (std::isxdigit(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c))) {
            hasHex = true;
        }
    }

    if (hasDot || hasExp) return {Token::Real, word};
    return {Token::Integer, word};
}

// ============================================================================
// S-Expression Parser
// ============================================================================

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Section
ANSYSMeshReader<Dim>::parseGroup_(Tokenizer& tok) {
    Section sec;
    sec.code = -1;

    Token t = tok.next();
    if (t.kind != Token::LParen) {
        throw std::runtime_error("ANSYSMeshReader: expected '('");
    }

    while (true) {
        t = tok.peek();
        if (t.kind == Token::RParen) {
            tok.next();
            break;
        }
        if (t.kind == Token::LParen) {
            sec.children.push_back(parseGroup_(tok));
        } else if (t.kind == Token::End) {
            throw std::runtime_error("ANSYSMeshReader: unexpected end of file");
        } else {
            tok.next();
            sec.values.push_back(t.raw);
        }
    }

    return sec;
}

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Section
ANSYSMeshReader<Dim>::parseNode(Tokenizer& tok) {
    Section sec;

    Token t = tok.next();
    if (t.kind != Token::LParen) {
        throw std::runtime_error("ANSYSMeshReader: expected '('");
    }

    t = tok.peek();
    if (t.kind == Token::Integer) {
        tok.next();
        sec.code = parseInt(t.raw);
    }

    if (sec.code == 0) {
        int depth = 1;
        while (depth > 0) {
            t = tok.next();
            if (t.kind == Token::End) {
                throw std::runtime_error("ANSYSMeshReader: unexpected end of file in comment");
            }
            if (t.kind == Token::LParen) depth++;
            else if (t.kind == Token::RParen) depth--;
        }
        return sec;
    }

    while (true) {
        t = tok.peek();
        if (t.kind == Token::RParen) {
            tok.next();
            break;
        }
        if (t.kind == Token::LParen) {
            sec.children.push_back(parseGroup_(tok));
        } else if (t.kind == Token::End) {
            throw std::runtime_error("ANSYSMeshReader: unexpected end of file");
        } else {
            tok.next();
            sec.values.push_back(t.raw);
        }
    }

    return sec;
}

// ============================================================================
// Utility Parsers
// ============================================================================

template<std::size_t Dim>
std::size_t ANSYSMeshReader<Dim>::parseHex(const std::string& s) {
    if (s.empty()) throw std::runtime_error("ANSYSMeshReader: empty hex number");
    try {
        return std::stoul(s, nullptr, 16);
    } catch (...) {
        throw std::runtime_error("ANSYSMeshReader: invalid hex number '" + s + "'");
    }
}

template<std::size_t Dim>
double ANSYSMeshReader<Dim>::parseDouble(const std::string& s) {
    if (s.empty()) throw std::runtime_error("ANSYSMeshReader: empty double");
    try {
        return std::stod(s);
    } catch (...) {
        throw std::runtime_error("ANSYSMeshReader: invalid double '" + s + "'");
    }
}

template<std::size_t Dim>
int ANSYSMeshReader<Dim>::parseInt(const std::string& s) {
    if (s.empty()) throw std::runtime_error("ANSYSMeshReader: empty integer");
    bool isHex = false;
    std::size_t start = 0;
    if (s[0] == '-' || s[0] == '+') start = 1;
    for (std::size_t i = start; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            isHex = true;
            break;
        }
    }
    try {
        if (isHex) return static_cast<int>(std::stoul(s, nullptr, 16));
        return std::stoi(s);
    } catch (...) {
        throw std::runtime_error("ANSYSMeshReader: invalid integer '" + s + "'");
    }
}

// ============================================================================
// Section Handlers
// ============================================================================

template<std::size_t Dim>
void ANSYSMeshReader<Dim>::handleSection(const Section& sec, ParseState& state) {
    switch (sec.code) {
        case 0:  // comment
        case 1:  // header
            break;
        case 2: {  // dimension
            if (!sec.values.empty()) {
                int dim = parseInt(sec.values[0]);
                if (dim != static_cast<int>(Dim)) {
                    throw std::runtime_error(
                        "ANSYSMeshReader: file dimension " + std::to_string(dim) +
                        " does not match expected " + std::to_string(Dim));
                }
            }
            break;
        }
        case 10: {  // nodes
            if (sec.children.size() < 2) break;
            const auto& header = sec.children[0].values;
            if (header.size() < 4) break;
            int zoneId = static_cast<int>(parseHex(header[0]));
            std::size_t firstIndex = parseHex(header[1]);
            std::size_t lastIndex = parseHex(header[2]);
            int nComponents = 0;
            if (header.size() >= 5) nComponents = parseInt(header[4]);
            if (nComponents == 0) nComponents = Dim;
            if (nComponents > static_cast<int>(Dim)) nComponents = Dim;

            std::size_t nNodes = lastIndex - firstIndex + 1;
            const auto& data = sec.children[1].values;
            std::size_t expected = nNodes * nComponents;
            if (data.size() < expected) {
                throw std::runtime_error(
                    "ANSYSMeshReader: node zone " + std::to_string(zoneId) +
                    " expected " + std::to_string(expected) +
                    " values, got " + std::to_string(data.size()));
            }

            std::size_t globalOffset = state.vertices.size();
            if (firstIndex > 0 && firstIndex - 1 != globalOffset) {
                throw std::runtime_error(
                    "ANSYSMeshReader: node zone " + std::to_string(zoneId) +
                    " first index " + std::to_string(firstIndex) +
                    " does not match expected offset " +
                    std::to_string(globalOffset + 1));
            }

            for (std::size_t i = 0; i < nNodes; ++i) {
                std::array<double, Dim> coord{};
                coord.fill(0.0);
                for (int c = 0; c < nComponents; ++c) {
                    coord[c] = parseDouble(data[i * nComponents + c]);
                }
                state.vertices.push_back(coord);
            }
            break;
        }
        case 12: {  // cells
            if (sec.children.empty()) break;
            const auto& header = sec.children[0].values;
            if (header.size() < 5) break;
            CellZone zone;
            zone.zoneId = static_cast<int>(parseHex(header[0]));
            zone.firstIndex = parseHex(header[1]);
            zone.lastIndex = parseHex(header[2]);
            zone.zoneType = parseInt(header[3]);
            zone.elementType = parseInt(header[4]);

            if (zone.zoneId == 0) {
                state.totalCells = zone.lastIndex - zone.firstIndex + 1;
            } else if (zone.zoneType != 0) {
                state.totalCells = zone.lastIndex - zone.firstIndex + 1;
                if (zone.elementType == 0 && sec.children.size() >= 2) {
                    for (const auto& v : sec.children[1].values) {
                        zone.perCellTypes.push_back(parseInt(v));
                    }
                }
                state.cellZones.push_back(std::move(zone));
            }
            break;
        }
        case 13: {  // faces
            if (sec.children.size() < 2) break;
            const auto& header = sec.children[0].values;
            if (header.size() < 5) break;
            int zoneId = static_cast<int>(parseHex(header[0]));
            std::size_t firstFaceIndex = parseHex(header[1]);
            std::size_t lastFaceIndex = parseHex(header[2]);
            int type = static_cast<int>(parseHex(header[3]));
            int elemType = static_cast<int>(parseHex(header[4]));

            state.faceZoneTypes[zoneId] = type;

            std::size_t nFaces = lastFaceIndex - firstFaceIndex + 1;

            int nodesPerFace = 2;
            switch (elemType) {
                case 2: nodesPerFace = 2; break;
                case 3: nodesPerFace = 3; break;
                case 4: nodesPerFace = 4; break;
                default: nodesPerFace = 2; break;
            }

            if (Dim == 2) nodesPerFace = 2;

            const auto& data = sec.children[1].values;
            std::size_t valuesPerFace = static_cast<std::size_t>(nodesPerFace) + 2;
            std::size_t expected = nFaces * valuesPerFace;
            if (data.size() < expected) {
                throw std::runtime_error(
                    "ANSYSMeshReader: face zone " + std::to_string(zoneId) +
                    " expected " + std::to_string(expected) +
                    " values, got " + std::to_string(data.size()));
            }

            for (std::size_t i = 0; i < nFaces; ++i) {
                FaceRecord face;
                face.zoneId = zoneId;
                for (int n = 0; n < nodesPerFace; ++n) {
                    face.vertices.push_back(parseHex(data[i * valuesPerFace + n]) - 1);
                }
                face.rightCell = static_cast<int>(parseHex(data[i * valuesPerFace + nodesPerFace])) - 1;
                face.leftCell = static_cast<int>(parseHex(data[i * valuesPerFace + nodesPerFace + 1])) - 1;
                state.faces.push_back(std::move(face));
            }
            break;
        }
        case 39:  // element group (legacy zone)
        case 45: {  // zone name/type
            if (sec.children.empty()) break;
            const auto& header = sec.children[0].values;
            if (header.size() < 3) break;
            int zoneId = parseInt(header[0]);
            state.zoneTypes[zoneId] = header[1];
            state.zoneNames[zoneId] = header[2];
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// Cell Reconstruction
// ============================================================================

template<std::size_t Dim>
std::vector<std::vector<std::size_t>>
ANSYSMeshReader<Dim>::reconstructCells(
    const std::vector<FaceRecord>& faces,
    std::size_t cellCount) {

    std::vector<std::vector<std::size_t>> cellToFaces(cellCount);

    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& f = faces[i];
        if (f.rightCell >= 0 && static_cast<std::size_t>(f.rightCell) < cellCount) {
            cellToFaces[f.rightCell].push_back(i);
        }
        if (f.leftCell >= 0 && static_cast<std::size_t>(f.leftCell) < cellCount) {
            cellToFaces[f.leftCell].push_back(i);
        }
    }

    std::vector<std::vector<std::size_t>> cells(cellCount);
    for (std::size_t c = 0; c < cellCount; ++c) {
        const auto& cf = cellToFaces[c];
        if (cf.empty()) continue;

        if (cf.size() == 1) {
            cells[c] = faces[cf[0]].vertices;
            continue;
        }

        if constexpr (Dim == 3) {
            // In 3D a cell face is a polygon, not an edge, so the edge walk
            // below cannot reconstruct a vertex loop. The cell vertices are
            // simply the union of the vertices of its faces; the ORDER does not
            // matter because the cell geometry is derived from the explicit
            // face topology (see reconstructCellFaces) and the vertex list is
            // only used as a set.
            std::vector<std::size_t> verts;
            std::unordered_map<std::size_t, char> seen;
            for (std::size_t fi : cf) {
                for (std::size_t v : faces[fi].vertices) {
                    if (seen.emplace(v, 1).second) verts.push_back(v);
                }
            }
            cells[c] = std::move(verts);
            continue;
        }

        std::unordered_map<std::size_t, std::vector<std::size_t>> vertToFaces;
        for (std::size_t fi : cf) {
            for (std::size_t v : faces[fi].vertices) {
                vertToFaces[v].push_back(fi);
            }
        }

        std::vector<std::size_t> orderedVerts;
        std::vector<bool> used(cf.size(), false);

        std::size_t firstFaceIdx = cf[0];
        const FaceRecord& firstFace = faces[firstFaceIdx];
        std::size_t startVert = firstFace.vertices[0];
        std::size_t currentVert = firstFace.vertices[1];
        orderedVerts.push_back(startVert);
        orderedVerts.push_back(currentVert);
        used[0] = true;

        int iterations = 0;
        int maxIterations = static_cast<int>(cf.size()) * 4;
        while (currentVert != startVert && iterations < maxIterations) {
            bool found = false;
            for (std::size_t i = 0; i < cf.size(); ++i) {
                if (used[i]) continue;
                const FaceRecord& f = faces[cf[i]];
                if (f.vertices[0] == currentVert) {
                    currentVert = f.vertices[1];
                    used[i] = true;
                    found = true;
                    if (currentVert != startVert) orderedVerts.push_back(currentVert);
                    break;
                } else if (f.vertices[1] == currentVert) {
                    currentVert = f.vertices[0];
                    used[i] = true;
                    found = true;
                    if (currentVert != startVert) orderedVerts.push_back(currentVert);
                    break;
                }
            }
            if (!found) break;
            iterations++;
        }

        if (orderedVerts.size() >= 3 && orderedVerts.back() == orderedVerts.front()) {
            orderedVerts.pop_back();
        }

        cells[c] = std::move(orderedVerts);
    }

    return cells;
}

// ============================================================================
// Cell Face Topology
// ============================================================================

template<std::size_t Dim>
std::vector<std::vector<std::vector<std::size_t>>>
ANSYSMeshReader<Dim>::reconstructCellFaces(
    const std::vector<FaceRecord>& faces,
    std::size_t cellCount) {

    std::vector<std::vector<std::size_t>> cellToFaces(cellCount);
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& f = faces[i];
        if (f.rightCell >= 0 && static_cast<std::size_t>(f.rightCell) < cellCount) {
            cellToFaces[f.rightCell].push_back(i);
        }
        if (f.leftCell >= 0 && static_cast<std::size_t>(f.leftCell) < cellCount) {
            cellToFaces[f.leftCell].push_back(i);
        }
    }

    std::vector<std::vector<std::vector<std::size_t>>> cellFaces(cellCount);
    for (std::size_t c = 0; c < cellCount; ++c) {
        cellFaces[c].reserve(cellToFaces[c].size());
        for (std::size_t fi : cellToFaces[c]) {
            cellFaces[c].push_back(faces[fi].vertices);
        }
    }
    return cellFaces;
}

// ============================================================================
// Boundary Group Extraction
// ============================================================================

template<std::size_t Dim>
std::map<std::string, std::vector<std::size_t>>
ANSYSMeshReader<Dim>::extractBoundaryGroups(
    const Mesh<Dim>& mesh,
    const std::vector<FaceRecord>& faces,
    const std::map<int, std::string>& zoneNames,
    const std::map<int, std::string>& zoneTypes,
    const std::map<int, int>& faceZoneTypes) {

    std::size_t nFaces = mesh.nFaces();
    std::map<std::vector<std::size_t>, std::size_t> faceVertexToIdx;
    for (std::size_t fi = 0; fi < nFaces; ++fi) {
        auto verts = mesh.faceVertices(fi);
        std::sort(verts.begin(), verts.end());
        faceVertexToIdx[verts] = fi;
    }

    std::map<std::string, std::vector<std::size_t>> boundaryGroups;

    auto collectZoneFaces = [&](int zoneId, const std::vector<FaceRecord>& faces,
                                 const std::map<std::vector<std::size_t>, std::size_t>& faceMap) {
        std::vector<std::size_t> indices;
        for (const auto& face : faces) {
            if (face.zoneId != zoneId) continue;
            if (face.rightCell >= 0 && face.leftCell >= 0) continue;

            std::vector<std::size_t> fv = face.vertices;
            std::sort(fv.begin(), fv.end());
            auto it = faceMap.find(fv);
            if (it != faceMap.end()) {
                indices.push_back(it->second);
            }
        }
        return indices;
    };

    for (const auto& [zoneId, zoneName] : zoneNames) {
        auto indices = collectZoneFaces(zoneId, faces, faceVertexToIdx);
        if (!indices.empty()) {
            boundaryGroups[zoneName] = std::move(indices);
        }
    }

    std::set<int> namedZones;
    for (const auto& [zoneId, zoneName] : zoneNames) {
        namedZones.insert(zoneId);
    }

    for (const auto& [zoneId, zoneType] : faceZoneTypes) {
        if (namedZones.count(zoneId)) continue;

        bool hasBoundaryFaces = false;
        for (const auto& face : faces) {
            if (face.zoneId != zoneId) continue;
            if (face.rightCell < 0 || face.leftCell < 0) {
                hasBoundaryFaces = true;
                break;
            }
        }
        if (!hasBoundaryFaces) continue;

        auto indices = collectZoneFaces(zoneId, faces, faceVertexToIdx);
        if (!indices.empty()) {
            boundaryGroups["boundary_" + std::to_string(zoneId)] = std::move(indices);
        }
    }

    return boundaryGroups;
}

// ============================================================================
// Main Read Function
// ============================================================================

template<std::size_t Dim>
int ANSYSMeshReader<Dim>::detectDimension(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ANSYSMeshReader: cannot open file '" + filename + "'");
    }

    // The Fluent format starts with small top-level sections (comment, header,
    // dimension) before the bulk data, so stopping at section 2 costs nothing.
    Tokenizer tokenizer(in);
    while (true) {
        auto t = tokenizer.peek();
        if (t.kind == Token::End) break;
        if (t.kind == Token::LParen) {
            Section sec = parseNode(tokenizer);
            if (sec.code == 2) {
                if (sec.values.empty()) {
                    throw std::runtime_error(
                        "ANSYSMeshReader: empty dimension section in '" + filename + "'");
                }
                const int dim = parseInt(sec.values[0]);
                if (dim != 2 && dim != 3) {
                    throw std::runtime_error(
                        "ANSYSMeshReader: unsupported mesh dimension " + std::to_string(dim));
                }
                return dim;
            }
        } else {
            tokenizer.next();
        }
    }

    throw std::runtime_error(
        "ANSYSMeshReader: no dimension section found in '" + filename + "'");
}

template<std::size_t Dim>
typename ANSYSMeshReader<Dim>::Result
ANSYSMeshReader<Dim>::read(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ANSYSMeshReader: cannot open file '" + filename + "'");
    }

    Tokenizer tokenizer(in);
    ParseState state;

    while (true) {
        auto t = tokenizer.peek();
        if (t.kind == Token::End) break;
        if (t.kind == Token::LParen) {
            Section sec = parseNode(tokenizer);
            handleSection(sec, state);
        } else {
            tokenizer.next();
        }
    }

    if (state.vertices.empty()) {
        throw std::runtime_error("ANSYSMeshReader: no vertices found in '" + filename + "'");
    }
    if (state.faces.empty()) {
        throw std::runtime_error("ANSYSMeshReader: no faces found in '" + filename + "'");
    }
    if (state.totalCells == 0) {
        throw std::runtime_error("ANSYSMeshReader: no cells found in '" + filename + "'");
    }

    auto cellVerts = reconstructCells(state.faces, state.totalCells);
    auto cellFaces = reconstructCellFaces(state.faces, state.totalCells);

    // The explicit face topology keeps the element node order irrelevant: the
    // mesh builds its faces from the face records instead of assuming a VTK
    // element layout that ANSYS does not use.
    std::vector<std::vector<std::size_t>> meshCells;
    std::vector<std::vector<std::vector<std::size_t>>> meshCellFaces;
    meshCells.reserve(cellVerts.size());
    meshCellFaces.reserve(cellVerts.size());
    for (std::size_t c = 0; c < cellVerts.size(); ++c) {
        if (cellVerts[c].empty()) continue;
        meshCells.push_back(std::move(cellVerts[c]));
        // The face list must follow the SAME cells as the vertex list, so it
        // is filtered with the same predicate.
        meshCellFaces.push_back(c < cellFaces.size() ? cellFaces[c]
                                                     : std::vector<std::vector<std::size_t>>{});
    }

    if (meshCells.empty()) {
        throw std::runtime_error("ANSYSMeshReader: cell reconstruction produced no valid cells");
    }

    Mesh<Dim> mesh(state.vertices, meshCells, meshCellFaces);

    auto boundaryGroups = extractBoundaryGroups(
        mesh, state.faces, state.zoneNames, state.zoneTypes, state.faceZoneTypes);

    mesh.setBoundaryGroups(boundaryGroups);

    Result result;
    result.mesh = std::move(mesh);
    result.boundaryGroups = std::move(boundaryGroups);
    return result;
}

// ============================================================================
// Template Instantiation
// ============================================================================

template class ANSYSMeshReader<2>;
template class ANSYSMeshReader<3>;

} // namespace fvm
