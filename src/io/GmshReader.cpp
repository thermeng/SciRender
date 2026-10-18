#include "io/GmshReader.h"
#include <cstdlib>

namespace fvm {

template<std::size_t Dim>
typename GmshReader<Dim>::Result
GmshReader<Dim>::read(const std::string& filename) {
    std::ifstream in(filename);
    if (!in) {
        throw std::runtime_error("GmshReader: cannot open file '" + filename + "'");
    }

    std::string line;

    while (std::getline(in, line)) {
        trim_(line);
        if (!line.empty() && line[0] != '#') break;
    }
    if (line != "$MeshFormat") {
        throw std::runtime_error("GmshReader: expected $MeshFormat, got '" + line + "'");
    }
    if (!std::getline(in, line)) {
        throw std::runtime_error("GmshReader: unexpected end of file in MeshFormat");
    }
    trim_(line);
    std::string version = line.substr(0, line.find_first_of(" \t"));
    if (version != "2.2") {
        throw std::runtime_error("GmshReader: only MSH 2.2 supported, got '" + version + "'");
    }
    skipTo_(in, "$EndMeshFormat");

    std::map<int, std::pair<int, std::string>> physicalNames;
    std::vector<std::array<double, Dim>> vertices;
    std::vector<std::vector<std::size_t>> cells;
    std::vector<int> elemPhysTags;
    std::vector<std::pair<FaceKey, int>> boundaryFaces;

    std::map<int, std::size_t> nodeTagMap;

    while (std::getline(in, line)) {
        trim_(line);
        if (line == "$PhysicalNames") {
            parsePhysicalNames_(in, physicalNames);
        } else if (line == "$Nodes") {
            parseNodes_(in, vertices, nodeTagMap);
        } else if (line == "$Elements") {
            parseElements_(in, cells, elemPhysTags, boundaryFaces, nodeTagMap);
        }
    }

    if (vertices.empty()) {
        throw std::runtime_error("GmshReader: no nodes found in '" + filename + "'");
    }
    if (cells.empty()) {
        throw std::runtime_error("GmshReader: no volume elements found in '" + filename + "'");
    }

    Mesh<Dim> mesh(std::move(vertices), std::move(cells));

    std::map<std::string, std::vector<std::size_t>> boundaryGroups;
    if (!physicalNames.empty()) {
        auto groupsFromFaces = extractBoundaryGroupsFromFaces_(mesh, boundaryFaces, physicalNames);
        auto groupsFromCells = extractBoundaryGroupsFromCells_(mesh, elemPhysTags, physicalNames);
        // Merge: face-based takes precedence (it uses boundary element tags)
        for (const auto& [name, faces] : groupsFromFaces) {
            boundaryGroups[name] = faces;
        }
        for (const auto& [name, faces] : groupsFromCells) {
            if (boundaryGroups.find(name) == boundaryGroups.end()) {
                boundaryGroups[name] = faces;
            }
        }

        // Collect boundary-dim physical group names (those with dim < Dim)
        std::vector<std::string> boundaryDimNames;
        for (const auto& [tag, dimName] : physicalNames) {
            if (dimName.first < static_cast<int>(Dim)) {
                boundaryDimNames.push_back(dimName.second);
            }
        }

        // Find all boundary faces not yet assigned to any group
        std::vector<bool> assigned(mesh.nFaces(), false);
        for (const auto& [name, faces] : boundaryGroups) {
            for (auto f : faces) assigned[f] = true;
        }

        // Compute bounding box of ALL boundary face centers
        std::array<double, Dim> bbMin{}, bbMax{};
        bool first = true;
        for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            const auto& fc = mesh.faceCenter(f);
            if (first) {
                bbMin = fc;
                bbMax = fc;
                first = false;
            } else {
                for (std::size_t d = 0; d < Dim; ++d) {
                    bbMin[d] = std::min(bbMin[d], fc[d]);
                    bbMax[d] = std::max(bbMax[d], fc[d]);
                }
            }
        }

        // For each boundary-dim group that already has faces, compute its centroid
        // to determine which edge it's on
        std::map<std::string, std::array<double, Dim>> groupCentroids;
        std::map<std::string, int> groupCounts;
        for (const auto& [name, faces] : boundaryGroups) {
            for (auto f : faces) {
                const auto& fc = mesh.faceCenter(f);
                for (std::size_t d = 0; d < Dim; ++d) {
                    groupCentroids[name][d] += fc[d];
                }
                groupCounts[name]++;
            }
        }
        for (auto& [name, centroid] : groupCentroids) {
            int cnt = groupCounts[name];
            if (cnt > 0) {
                for (std::size_t d = 0; d < Dim; ++d) centroid[d] /= cnt;
            }
        }

        // For unmatched boundary faces, find closest group centroid and assign
        for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            if (assigned[f]) continue;

            const auto& fc = mesh.faceCenter(f);

            std::string bestGroup;
            double bestDist = std::numeric_limits<double>::max();
            for (const auto& [name, centroid] : groupCentroids) {
                double dist = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) {
                    double diff = fc[d] - centroid[d];
                    dist += diff * diff;
                }
                if (dist < bestDist) {
                    bestDist = dist;
                    bestGroup = name;
                }
            }

            if (!bestGroup.empty()) {
                boundaryGroups[bestGroup].push_back(f);
            }
        }
    }

    return {std::move(mesh), std::move(boundaryGroups)};
}

template<std::size_t Dim>
typename GmshReader<Dim>::FaceKey
GmshReader<Dim>::sortedFaceVerts(const std::vector<std::size_t>& verts) {
    FaceKey key(verts.begin(), verts.end());
    std::sort(key.begin(), key.end());
    return key;
}

template<std::size_t Dim>
void GmshReader<Dim>::trim_(std::string& s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    std::size_t end = s.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) s.erase(end + 1);
    else s.clear();
}

template<std::size_t Dim>
std::string GmshReader<Dim>::trimmed(const std::string& s) {
    std::string t(s);
    trim_(t);
    return t;
}

template<std::size_t Dim>
void GmshReader<Dim>::skipTo_(std::ifstream& in, const std::string& marker) {
    std::string line;
    while (std::getline(in, line)) {
        trim_(line);
        if (line == marker) return;
    }
}

template<std::size_t Dim>
void GmshReader<Dim>::parsePhysicalNames_(std::ifstream& in,
                                           std::map<int, std::pair<int, std::string>>& names) {
    std::string line;
    std::getline(in, line);
    int count = std::stoi(trimmed(line));
    for (int i = 0; i < count; ++i) {
        std::getline(in, line);
        trim_(line);
        std::size_t pos = 0;
        int dim = parseInt_(line, pos);
        int tag = parseInt_(line, pos);
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        if (pos < line.size() && line[pos] == '"') {
            ++pos;
            std::size_t end = line.find('"', pos);
            if (end == std::string::npos) end = line.size();
            names[tag] = {dim, line.substr(pos, end - pos)};
        }
    }
    skipTo_(in, "$EndPhysicalNames");
}

template<std::size_t Dim>
void GmshReader<Dim>::parseNodes_(std::ifstream& in,
                                  std::vector<std::array<double, Dim>>& vertices,
                                  std::map<int, std::size_t>& nodeTagMap) {
    std::string line;
    std::getline(in, line);
    int totalNodes = std::stoi(trimmed(line));

    vertices.resize(totalNodes);
    nodeTagMap.clear();

    for (int i = 0; i < totalNodes; ++i) {
        std::getline(in, line);
        trim_(line);
        std::size_t pos = 0;
        int tag = parseInt_(line, pos);
        std::size_t idx = static_cast<std::size_t>(i);
        nodeTagMap[tag] = idx;
        for (std::size_t d = 0; d < Dim; ++d) {
            vertices[idx][d] = parseDouble_(line, pos);
        }
    }
    skipTo_(in, "$EndNodes");
}

template<std::size_t Dim>
std::vector<std::size_t> GmshReader<Dim>::resolveNodeTags_(const std::string& line,
                                                            std::size_t& pos,
                                                            int count,
                                                            const std::map<int, std::size_t>& nodeTagMap) {
    std::vector<std::size_t> result(count);
    for (int v = 0; v < count; ++v) {
        int nodeTag = parseInt_(line, pos);
        auto it = nodeTagMap.find(nodeTag);
        if (it == nodeTagMap.end()) {
            throw std::runtime_error(
                "GmshReader: node tag " + std::to_string(nodeTag) + " not found");
        }
        result[v] = it->second;
    }
    return result;
}

template<std::size_t Dim>
void GmshReader<Dim>::parseElements_(std::ifstream& in,
                                     std::vector<std::vector<std::size_t>>& cells,
                                     std::vector<int>& elemPhysTags,
                                     std::vector<std::pair<FaceKey, int>>& boundaryFaces,
                                     const std::map<int, std::size_t>& nodeTagMap) {
    std::string line;
    std::getline(in, line);
    int totalElements = std::stoi(trimmed(line));

    int simplexType = (Dim == 2) ? 2 : 4;
    int quadType = (Dim == 2) ? 3 : 5;
    int boundaryType = (Dim == 2) ? 1 : 2;

    for (int i = 0; i < totalElements; ++i) {
        std::getline(in, line);
        trim_(line);
        std::size_t pos = 0;
        parseInt_(line, pos); // elem tag
        int elemType = parseInt_(line, pos);
        int nTags = parseInt_(line, pos);

        if (elemType == simplexType || elemType == quadType) {
            int physicalTag = 0;
            int elementaryTag = 0;
            for (int t = 0; t < nTags; ++t) {
                int tv = parseInt_(line, pos);
                if (t == 0) physicalTag = tv;
                else if (t == 1) elementaryTag = tv;
            }
            int effectiveTag = (physicalTag != 0) ? physicalTag : elementaryTag;

            int vertsPerCell;
            if (elemType == simplexType) {
                vertsPerCell = Dim + 1;
            } else {
                vertsPerCell = (Dim == 2) ? 4 : 8;
            }

            std::vector<std::size_t> cell(vertsPerCell);
            for (int v = 0; v < vertsPerCell; ++v) {
                int nodeTag = parseInt_(line, pos);
                auto it = nodeTagMap.find(nodeTag);
                if (it == nodeTagMap.end()) {
                    throw std::runtime_error(
                        "GmshReader: node tag " + std::to_string(nodeTag) + " not found");
                }
                cell[v] = it->second;
            }
            cells.push_back(std::move(cell));
            elemPhysTags.push_back(effectiveTag);
        } else if (elemType == boundaryType) {
            int physicalTag = 0;
            int elementaryTag = 0;
            for (int t = 0; t < nTags; ++t) {
                int tv = parseInt_(line, pos);
                if (t == 0) physicalTag = tv;
                else if (t == 1) elementaryTag = tv;
            }
            int effectiveTag = (physicalTag != 0) ? physicalTag : elementaryTag;
            int vertsPerBoundary = boundaryType + 1;
            auto verts = resolveNodeTags_(line, pos, vertsPerBoundary, nodeTagMap);
            std::sort(verts.begin(), verts.end());
            boundaryFaces.emplace_back(std::move(verts), effectiveTag);
        } else {
            for (int t = 0; t < nTags + elemType; ++t) {
                parseInt_(line, pos);
            }
        }
    }
    skipTo_(in, "$EndElements");
}

template<std::size_t Dim>
std::map<std::string, std::vector<std::size_t>>
GmshReader<Dim>::extractBoundaryGroupsFromFaces_(
    const Mesh<Dim>& mesh,
    const std::vector<std::pair<FaceKey, int>>& boundaryFaces,
    const std::map<int, std::pair<int, std::string>>& physicalNames) {

    std::map<FaceKey, std::size_t> meshFaceMap;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;
        meshFaceMap[sortedFaceVerts(mesh.faceVertices(f))] = f;
    }

    std::map<std::size_t, int> facePhysTags;
    for (const auto& [key, physTag] : boundaryFaces) {
        auto it = meshFaceMap.find(key);
        if (it != meshFaceMap.end()) {
            facePhysTags[it->second] = physTag;
        }
    }

    std::map<std::string, std::vector<std::size_t>> groups;
    for (const auto& [faceIdx, physTag] : facePhysTags) {
        if (physTag == 0) continue;
        auto nameIt = physicalNames.find(physTag);
        if (nameIt != physicalNames.end()) {
            groups[nameIt->second.second].push_back(faceIdx);
        }
    }

    return groups;
}

template<std::size_t Dim>
std::map<std::string, std::vector<std::size_t>>
GmshReader<Dim>::extractBoundaryGroupsFromCells_(
    const Mesh<Dim>& mesh,
    const std::vector<int>& elemPhysTags,
    const std::map<int, std::pair<int, std::string>>& physicalNames) {

    std::map<std::string, std::vector<std::size_t>> groups;

    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) != std::size_t(-1)) continue;

        std::size_t owner = mesh.owner(f);
        if (owner >= elemPhysTags.size()) continue;

        int physTag = elemPhysTags[owner];
        if (physTag == 0) continue;

        auto it = physicalNames.find(physTag);
        if (it != physicalNames.end()) {
            if (it->second.first >= static_cast<int>(Dim)) continue;
            groups[it->second.second].push_back(f);
        }
    }

    return groups;
}

template<std::size_t Dim>
int GmshReader<Dim>::parseInt_(const std::string& s, std::size_t& pos) {
    skipSpace_(s, pos);
    char* end;
    long val = std::strtol(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) {
        throw std::runtime_error("GmshReader: expected integer");
    }
    pos = static_cast<std::size_t>(end - s.c_str());
    return static_cast<int>(val);
}

template<std::size_t Dim>
double GmshReader<Dim>::parseDouble_(const std::string& s, std::size_t& pos) {
    skipSpace_(s, pos);
    char* end;
    double val = std::strtod(s.c_str() + pos, &end);
    if (end == s.c_str() + pos) {
        throw std::runtime_error("GmshReader: expected double");
    }
    pos = static_cast<std::size_t>(end - s.c_str());
    return val;
}

template<std::size_t Dim>
void GmshReader<Dim>::skipSpace_(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
}

template class GmshReader<2>;
template class GmshReader<3>;

} // namespace fvm
