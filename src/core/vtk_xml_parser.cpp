#include "core/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <cctype>

// ============================================================================
// Base64 decode
// ============================================================================

static const char* base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_char_to_value(char c) {
    const char* p = std::strchr(base64_chars, c);
    return p ? int(p - base64_chars) : -1;
}

static void decodeBase64(const std::string& input, std::vector<char>& out) {
    out.clear();
    out.reserve(input.size() * 3 / 4);
    std::vector<int> tmp;
    tmp.reserve(input.size());
    for (char c : input) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') { tmp.push_back(-1); continue; }
        int val = base64_char_to_value(c);
        if (val >= 0) tmp.push_back(val);
    }
    for (size_t i = 0; i + 1 < tmp.size(); i += 4) {
        int a = tmp[i], b = (i + 1 < tmp.size()) ? tmp[i + 1] : 0;
        int c = (i + 2 < tmp.size()) ? tmp[i + 2] : 0;
        int d = (i + 3 < tmp.size()) ? tmp[i + 3] : 0;
        if (a < 0 || b < 0) break;
        out.push_back(char((a << 2) | (b >> 4)));
        if (c >= 0) out.push_back(char((b << 4) | (c >> 2)));
        if (d >= 0) out.push_back(char((c << 6) | d));
    }
}

// ============================================================================
// Binary read helpers
// ============================================================================

template<typename T>
static bool readFromBuffer(const char* buf, size_t bufLen, size_t& offset,
                           size_t count, std::vector<T>& out, bool swapBytes) {
    if (offset + count * sizeof(T) > bufLen) return false;
    out.resize(count);
    std::memcpy(out.data(), buf + offset, count * sizeof(T));
    offset += count * sizeof(T);
    if (swapBytes) {
        for (size_t i = 0; i < count; ++i) mesh_utils::byteSwap(&out[i]);
    }
    return true;
}

// ============================================================================
// XML string helpers
// ============================================================================

static std::string readFileIntoString(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string extractAttribute(const std::string& content, const std::string& name) {
    std::string pattern = name + "=\"";
    size_t pos = content.find(pattern);
    if (pos == std::string::npos) {
        pattern = name + "='";
        pos = content.find(pattern);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
    } else {
        pos += pattern.size();
    }
    size_t end = content.find('"', pos);
    if (end == std::string::npos) end = content.size();
    return content.substr(pos, end - pos);
}

static std::string extractTagContent(const std::string& xml, const std::string& tagName) {
    std::string openTag = "<" + tagName;
    std::string closeTag = "</" + tagName + ">";
    size_t start = xml.find(openTag);
    if (start == std::string::npos) return "";
    size_t openEnd = xml.find('>', start);
    if (openEnd == std::string::npos) return "";
    if (openEnd > start && xml[openEnd - 1] == '/') return "";
    size_t contentStart = openEnd + 1;
    size_t closeStart = xml.find(closeTag, contentStart);
    if (closeStart == std::string::npos) return "";
    return xml.substr(contentStart, closeStart - contentStart);
}

static std::pair<std::string, std::string> extractFirstDataArray(const std::string& xml) {
    std::string openTag;
    std::string content;
    size_t start = xml.find("<DataArray");
    if (start == std::string::npos) return {};
    size_t openEnd = xml.find('>', start);
    if (openEnd == std::string::npos) return {};
    openTag = xml.substr(start, openEnd - start + 1);
    if (openTag.empty() || openTag.back() == '/') return {};
    size_t contentStart = openEnd + 1;
    size_t closeStart = xml.find("</DataArray>", contentStart);
    if (closeStart == std::string::npos) return {};
    content = xml.substr(contentStart, closeStart - contentStart);
    return {openTag, content};
}

// ============================================================================
// Topology helpers (standalone versions of legacy parser internals)
// ============================================================================

static void generateStructuredGridSurface(RenderMesh& mesh, int dX, int dY, int dZ) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
    auto addQuad = [&](int a, int b, int c, int d) {
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(b));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(d));
        cellToVertices.push_back({
            static_cast<uint32_t>(a), static_cast<uint32_t>(b),
            static_cast<uint32_t>(c), static_cast<uint32_t>(d)
        });
    };

    int cx = std::max(1, dX - 1);
    int cy = std::max(1, dY - 1);
    int cz = std::max(1, dZ - 1);

    const bool is3D = (dX > 1 && dY > 1 && dZ > 1);
    if (is3D) {
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, 0), idx(x, y + 1, 0), idx(x + 1, y + 1, 0), idx(x + 1, y, 0));
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, dZ - 1), idx(x + 1, y, dZ - 1), idx(x + 1, y + 1, dZ - 1), idx(x, y + 1, dZ - 1));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(0, y, z), idx(0, y, z + 1), idx(0, y + 1, z + 1), idx(0, y + 1, z));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(dX - 1, y, z), idx(dX - 1, y + 1, z), idx(dX - 1, y + 1, z + 1), idx(dX - 1, y, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, dY - 1, z), idx(x, dY - 1, z + 1), idx(x + 1, dY - 1, z + 1), idx(x + 1, dY - 1, z));
    } else if (dZ == 1) {
        for (int y = 0; y + 1 < dY; ++y)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, y, 0), idx(x + 1, y, 0), idx(x + 1, y + 1, 0), idx(x, y + 1, 0));
    } else if (dY == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
    } else if (dX == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int y = 0; y + 1 < dY; ++y)
                addQuad(idx(0, y, z), idx(0, y + 1, z), idx(0, y + 1, z + 1), idx(0, y, z + 1));
    }
    (void)cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulatePolygons(RenderMesh& mesh,
                                                              const std::vector<int32_t>& rawPolygonData,
                                                              int numPolys) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numPolys);
    for (int p = 0; p < numPolys; ++p) {
        if (idx >= static_cast<int>(rawPolygonData.size())) break;
        int nPoints = rawPolygonData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawPolygonData.size())) break;
        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[p].push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
        }
        for (int i = 1; i < nPoints - 1; ++i) {
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + 0]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i + 1]));
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateTriangleStrips(RenderMesh& mesh,
                                                                     const std::vector<int32_t>& rawStripData,
                                                                     int numStrips) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    cellToVertices.reserve(numStrips > 0 ? numStrips * 3 : 0);
    int idx = 0;
    for (int s = 0; s < numStrips; ++s) {
        if (idx >= static_cast<int>(rawStripData.size())) break;
        int nPoints = rawStripData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawStripData.size())) break;
        for (int i = 0; i < nPoints - 2; ++i) {
            uint32_t i0 = static_cast<uint32_t>(rawStripData[idx + i]);
            uint32_t i1 = static_cast<uint32_t>(rawStripData[idx + i + 1]);
            uint32_t i2 = static_cast<uint32_t>(rawStripData[idx + i + 2]);
            cellToVertices.push_back({ i0, i1, i2 });
            if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
            else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateLines(RenderMesh& mesh,
                                                            const std::vector<int32_t>& rawLineData,
                                                            int numLines) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numLines);
    for (int l = 0; l < numLines; ++l) {
        if (idx >= static_cast<int>(rawLineData.size())) break;
        int nPoints = rawLineData[idx++];
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawLineData.size())) break;
        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[l].push_back(static_cast<uint32_t>(rawLineData[idx + i]));
        }
        for (int i = 0; i + 1 < nPoints; ++i) {
            uint32_t a = static_cast<uint32_t>(rawLineData[idx + i]);
            uint32_t b = static_cast<uint32_t>(rawLineData[idx + i + 1]);
            mesh.indices.insert(mesh.indices.end(), { a, b });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

static std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(
    RenderMesh& mesh,
    const std::vector<int32_t>& rawCellData,
    const std::vector<int32_t>& cellTypes,
    int totalCells) {
    mesh.indices.clear();
    std::vector<std::vector<uint32_t>> cellToVertices(totalCells);
    int idx = 0;
    for (int c = 0; c < totalCells; ++c) {
        if (idx >= static_cast<int>(rawCellData.size())) break;
        int numPointsInCell = rawCellData[idx++];
        if (numPointsInCell < 0 || idx + numPointsInCell > static_cast<int>(rawCellData.size())) break;
        for (int i = 0; i < numPointsInCell; ++i) {
            cellToVertices[c].push_back(static_cast<uint32_t>(rawCellData[idx + i]));
        }
        int type = (c < static_cast<int>(cellTypes.size())) ? cellTypes[c] : 0;
        if (type == 0) {
            if (numPointsInCell == 3) type = 5;
            if (numPointsInCell == 4) type = 9;
            if (numPointsInCell == 8) type = 12;
        }
        switch (type) {
        case 5:
            if (idx + 2 < static_cast<int>(rawCellData.size())) {
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 1]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 2]));
            }
            break;
        case 9:
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
            }
            break;
        case 10:
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i1, i2, i0, i2, i3, i0, i3, i1, i1, i3, i2
                });
            }
            break;
        case 12:
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t i6 = static_cast<uint32_t>(rawCellData[idx + 6]);
                uint32_t i7 = static_cast<uint32_t>(rawCellData[idx + 7]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i3, i1, i1, i3, i2, i4, i5, i7, i5, i6, i7,
                    i0, i1, i4, i1, i5, i4, i2, i3, i6, i3, i7, i6,
                    i0, i4, i3, i3, i4, i7, i1, i2, i5, i2, i6, i5
                });
            }
            break;
        case 13:
            if (idx + 5 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i1, i2, i3, i5, i4,
                    i0, i1, i4, i0, i4, i3,
                    i1, i2, i5, i1, i5, i4,
                    i0, i2, i5, i0, i5, i3
                });
            }
            break;
        case 14:
            if (idx + 4 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1, i0, i3, i2,
                    i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                });
            }
            break;
        case 11: {
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t h0 = static_cast<uint32_t>(rawCellData[idx + 0]);
                uint32_t h1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t h2 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t h3 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t h4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t h5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t h6 = static_cast<uint32_t>(rawCellData[idx + 7]);
                uint32_t h7 = static_cast<uint32_t>(rawCellData[idx + 6]);
                mesh.indices.insert(mesh.indices.end(), {
                    h0, h3, h1, h1, h3, h2, h4, h5, h7, h5, h6, h7,
                    h0, h1, h4, h1, h5, h4, h2, h3, h6, h3, h7, h6,
                    h0, h4, h3, h3, h4, h7, h1, h2, h5, h2, h6, h5
                });
            }
            break;
        }
        default:
            for (int i = 1; i < numPointsInCell - 1; ++i) {
                if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                }
            }
            break;
        }
        idx += numPointsInCell;
    }
    return cellToVertices;
}

static void extrapolateCellDataToPointsMerged(
    RenderMesh& mesh,
    const std::vector<std::vector<uint32_t>>& globalCellToVertices,
    const std::unordered_map<std::string, std::vector<float>>& cellScalarsStorage,
    const std::unordered_map<std::string, std::vector<float>>& cellVectorsStorage) {
    if (globalCellToVertices.empty()) return;
    if (cellScalarsStorage.empty() && cellVectorsStorage.empty()) return;

    int vCount = static_cast<int>(mesh.vertices.size() / 3);
    if (vCount == 0) return;
    if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

    struct FieldAcc { std::vector<float> sum; };
    std::vector<FieldAcc> accs;
    accs.reserve(cellScalarsStorage.size() + cellVectorsStorage.size());
    std::map<std::string, size_t> scalarAccIdx;
    std::map<std::string, size_t> vectorAccIdx;

    for (const auto& [name, raw] : cellScalarsStorage) {
        scalarAccIdx[name] = accs.size();
        accs.push_back(FieldAcc{ std::vector<float>(static_cast<size_t>(vCount), 0.0f) });
    }
    for (const auto& [name, raw] : cellVectorsStorage) {
        vectorAccIdx[name] = accs.size();
        accs.push_back(FieldAcc{ std::vector<float>(static_cast<size_t>(vCount) * 3, 0.0f) });
    }
    std::vector<float> contributionCounts(static_cast<size_t>(vCount), 0.0f);

    for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
        for (const auto& [name, raw] : cellScalarsStorage) {
            if (c >= raw.size()) continue;
            float val = raw[c];
            size_t si = scalarAccIdx[name];
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) accs[si].sum[vIdx] += val;
            }
        }
        for (const auto& [name, raw] : cellVectorsStorage) {
            if (c >= raw.size() / 3) continue;
            float vx = raw[c * 3 + 0];
            float vy = raw[c * 3 + 1];
            float vz = raw[c * 3 + 2];
            size_t vi = vectorAccIdx[name];
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) {
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 0] += vx;
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 1] += vy;
                    accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 2] += vz;
                }
            }
        }
        for (int vIdx : globalCellToVertices[c]) {
            if (vIdx >= 0 && vIdx < vCount) contributionCounts[vIdx] += 1.0f;
        }
    }

    for (const auto& [name, raw] : cellScalarsStorage) {
        std::vector<float>& sum = accs[scalarAccIdx[name]].sum;
        for (int i = 0; i < vCount; ++i) {
            if (contributionCounts[i] > 0.0f) sum[i] /= contributionCounts[i];
        }
        mesh.attributes->pointScalars[name] = std::move(sum);
    }
    for (const auto& [name, raw] : cellVectorsStorage) {
        std::vector<float>& sum = accs[vectorAccIdx[name]].sum;
        for (int i = 0; i < vCount; ++i) {
            if (contributionCounts[i] > 0.0f) {
                float inv = 1.0f / contributionCounts[i];
                sum[static_cast<size_t>(i) * 3 + 0] *= inv;
                sum[static_cast<size_t>(i) * 3 + 1] *= inv;
                sum[static_cast<size_t>(i) * 3 + 2] *= inv;
            }
        }
        mesh.attributes->pointVectors[name] = std::move(sum);
    }
}

// ============================================================================
// VTK XML Parser Context
// ============================================================================

class VTKXMLParserContext {
public:
    explicit VTKXMLParserContext(const std::string& path) : filePath(path) {}

    RenderMesh parse() {
        mesh.bounds.centerX = mesh.bounds.centerY = mesh.bounds.centerZ = 0.0;
        mesh.bounds.extent = 1.0;

        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "VTK XML Parser Error: Failed to open: " << filePath << std::endl;
            return mesh;
        }

        // Read entire file into string for XML text parsing
        std::string xml = readFileIntoString(filePath);
        file.close();
        if (xml.empty()) {
            std::cerr << "VTK XML Parser Error: Empty file: " << filePath << std::endl;
            return mesh;
        }

        // Find root VTKFile tag
        size_t pos = 0;
        std::string vtkFileOpenTag;
        {
            std::string openTagStr = "<VTKFile";
            size_t start = xml.find(openTagStr);
            if (start == std::string::npos) {
                std::cerr << "VTK XML Parser Error: Missing <VTKFile> tag." << std::endl;
                return mesh;
            }
            size_t end = xml.find('>', start);
            if (end == std::string::npos) {
                std::cerr << "VTK XML Parser Error: Malformed <VTKFile> tag." << std::endl;
                return mesh;
            }
            vtkFileOpenTag = xml.substr(start, end - start + 1);
        }
        datasetType = mesh_utils::toUpper(extractAttribute(vtkFileOpenTag, "type"));
        byteOrder = mesh_utils::toUpper(extractAttribute(vtkFileOpenTag, "byte_order"));
        if (byteOrder.empty()) byteOrder = "LITTLEENDIAN";
        headerType = mesh_utils::toUpper(extractAttribute(vtkFileOpenTag, "header_type"));
        if (headerType.empty()) headerType = "UINT32";

        bool bigEndian = (byteOrder == "BIGENDIAN");
        bool header64 = (headerType == "UINT64");

        // Find appended data block if present
        std::string appendedXml = extractTagContent(xml, "AppendedData");
        if (!appendedXml.empty()) {
            std::string encoding = mesh_utils::toUpper(extractAttribute(appendedXml, "encoding"));
            if (encoding == "BASE64") {
                decodeBase64(appendedXml, appendedData);
            } else {
                appendedData.assign(appendedXml.begin(), appendedXml.end());
            }
        }

        // Find inner dataset tag
        std::string innerTag;
        if (datasetType == "UNSTRUCTUREDGRID") innerTag = "UnstructuredGrid";
        else if (datasetType == "STRUCTUREDGRID") innerTag = "StructuredGrid";
        else if (datasetType == "IMAGEDATA") innerTag = "ImageData";
        else if (datasetType == "POLYDATA") innerTag = "PolyData";
        else if (datasetType == "RECTILINEARGRID") innerTag = "RectilinearGrid";
        else {
            std::cerr << "VTK XML Parser Error: Unsupported dataset type: " << datasetType << std::endl;
            return mesh;
        }

        std::string datasetXml = extractTagContent(xml, innerTag);
        if (datasetXml.empty()) {
            std::cerr << "VTK XML Parser Error: Missing <" << innerTag << "> tag." << std::endl;
            return mesh;
        }

        if (datasetType == "STRUCTUREDGRID" || datasetType == "IMAGEDATA" || datasetType == "RECTILINEARGRID") {
            std::string datasetOpenTag;
            {
                std::string openTagStr = "<" + innerTag;
                size_t start = xml.find(openTagStr);
                if (start != std::string::npos) {
                    size_t end = xml.find('>', start);
                    if (end != std::string::npos) {
                        datasetOpenTag = xml.substr(start, end - start + 1);
                    }
                }
            }
            if (!datasetOpenTag.empty()) {
                std::string we = extractAttribute(datasetOpenTag, "WholeExtent");
                if (!we.empty()) {
                    std::istringstream iss(we);
                    iss >> wholeExtent[0] >> wholeExtent[1] >> wholeExtent[2] >> wholeExtent[3] >> wholeExtent[4] >> wholeExtent[5];
                }
                std::string org = extractAttribute(datasetOpenTag, "Origin");
                if (!org.empty()) {
                    std::istringstream iss(org);
                    iss >> origin[0] >> origin[1] >> origin[2];
                }
                std::string sp = extractAttribute(datasetOpenTag, "Spacing");
                if (!sp.empty()) {
                    std::istringstream iss(sp);
                    iss >> spacing[0] >> spacing[1] >> spacing[2];
                }
            }
        }

        // Parse all Piece elements
        size_t piecePos = 0;
        while (true) {
            size_t pieceStart = datasetXml.find("<Piece", piecePos);
            if (pieceStart == std::string::npos) break;
            size_t pieceEnd = datasetXml.find('>', pieceStart);
            if (pieceEnd == std::string::npos) break;
            std::string pieceTag = datasetXml.substr(pieceStart, pieceEnd - pieceStart + 1);
            // Self-closing or open tag
            bool selfClosing = (!pieceTag.empty() && pieceTag.back() == '/');
            std::string pieceContent;
            if (selfClosing) {
                pieceContent = "";
            } else {
                size_t contentStart = pieceEnd + 1;
                size_t closePos = datasetXml.find("</Piece>", contentStart);
                if (closePos == std::string::npos) break;
                pieceContent = datasetXml.substr(contentStart, closePos - contentStart);
            }

            int piecePoints = 0;
            int pieceCells = 0;
            std::string numPointsStr = extractAttribute(pieceTag, "NumberOfPoints");
            std::string numCellsStr = extractAttribute(pieceTag, "NumberOfCells");
            if (!numPointsStr.empty()) piecePoints = std::stoi(numPointsStr);
            if (!numCellsStr.empty()) pieceCells = std::stoi(numCellsStr);

            processPieceContent(pieceTag, pieceContent, piecePoints, pieceCells, bigEndian, header64);
            piecePos = pieceEnd + 1;
        }

        buildTopology();
        finalizeMeshData();

        // Emit cell edges for grid datasets
        if (mesh.supportsCellGrid) {
            for (const auto& cell : globalCellToVertices) {
                const size_t n = cell.size();
                if (n < 2) continue;
                for (size_t k = 0; k < n; ++k) {
                    const uint32_t a = cell[k];
                    const uint32_t b = cell[(k + 1) % n];
                    mesh.cellEdges.push_back(mesh.vertices[3 * a + 0]);
                    mesh.cellEdges.push_back(mesh.vertices[3 * a + 1]);
                    mesh.cellEdges.push_back(mesh.vertices[3 * a + 2]);
                    mesh.cellEdges.push_back(mesh.vertices[3 * b + 0]);
                    mesh.cellEdges.push_back(mesh.vertices[3 * b + 1]);
                    mesh.cellEdges.push_back(mesh.vertices[3 * b + 2]);
                }
            }
        }

        mesh.datasetType = datasetType;
        mesh.fileFormat = "VTKXML";
        return mesh;
    }

private:
    std::string filePath;
    RenderMesh mesh;
    std::string datasetType;
    std::string byteOrder;
    std::string headerType;

    int numPoints = 0;
    int numCells = 0;

    // ImageData / StructuredGrid extent
    int wholeExtent[6] = {0, -1, 0, -1, 0, -1};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float spacing[3] = {1.0f, 1.0f, 1.0f};

    // RectilinearGrid coordinates
    std::vector<float> rectX, rectY, rectZ;

    // Points
    std::vector<float> points;

    // UnstructuredGrid cells
    std::vector<int32_t> connectivity;
    std::vector<int32_t> offsets;
    std::vector<int32_t> cellTypes;

    // PolyData
    std::vector<int32_t> polys;
    std::vector<int32_t> lines;
    std::vector<int32_t> strips;
    std::vector<int32_t> verts;
    int numPolys = 0, numLines = 0, numStrips = 0, numVerts = 0;

    // Data arrays
    struct PendingDataArray {
        std::string name;
        int numComponents = 1;
        std::string format;
        std::string type;
        bool isPointData = true;
        size_t appendedOffset = 0;
        size_t tupleCount = 0;
    };
    std::vector<PendingDataArray> pendingArrays;
    std::unordered_map<std::string, std::vector<float>> cellScalarsStorage;
    std::unordered_map<std::string, std::vector<float>> cellVectorsStorage;
    std::vector<std::vector<uint32_t>> globalCellToVertices;

    // Appended binary buffer
    std::vector<char> appendedData;
    size_t appendedOffset = 0;

    void processPieceContent(const std::string& pieceTag, const std::string& content, int piecePoints, int pieceCells,
                             bool bigEndian, bool header64) {
        numPoints += piecePoints;
        numCells += pieceCells;

        // Points
        std::string pointsXml = extractTagContent(content, "Points");
        if (!pointsXml.empty()) {
            size_t daStart = pointsXml.find("<DataArray");
            if (daStart != std::string::npos) {
                size_t daEnd = pointsXml.find('>', daStart);
                if (daEnd != std::string::npos) {
                    std::string daOpenTag = pointsXml.substr(daStart, daEnd - daStart + 1);
                    std::string typeStr = mesh_utils::toUpper(extractAttribute(daOpenTag, "type"));
                    int numComp = 1;
                    std::string ncStr = extractAttribute(daOpenTag, "NumberOfComponents");
                    if (!ncStr.empty()) numComp = std::stoi(ncStr);
                    std::string fmt = mesh_utils::toUpper(extractAttribute(daOpenTag, "format"));
                    if (fmt.empty()) fmt = "ASCII";

                    bool selfClose = (!daOpenTag.empty() && daOpenTag.back() == '/');
                    std::string daContent;
                    if (!selfClose) {
                        size_t contentStart = daEnd + 1;
                        size_t closeDa = pointsXml.find("</DataArray>", contentStart);
                        if (closeDa != std::string::npos) {
                            daContent = pointsXml.substr(contentStart, closeDa - contentStart);
                        }
                    }

                    std::vector<float> vals;
                    if (fmt == "ASCII") {
                        std::istringstream iss(daContent);
                        float v;
                        while (iss >> v) vals.push_back(v);
                    } else if (fmt == "BINARY" || fmt == "APPENDED") {
                        if (fmt == "APPENDED") {
                            size_t offset = 0;
                            std::string offsetStr = extractAttribute(daOpenTag, "offset");
                            if (!offsetStr.empty()) offset = static_cast<size_t>(std::stoull(offsetStr));
                            if (header64) {
                                uint64_t header = 0;
                                if (offset + sizeof(uint64_t) <= appendedData.size()) {
                                    std::memcpy(&header, appendedData.data() + offset, sizeof(uint64_t));
                                    if (bigEndian != mesh_utils::isLittleEndian()) {
                                        mesh_utils::byteSwap(&header);
                                    }
                                }
                                offset += sizeof(uint64_t);
                                size_t count = static_cast<size_t>(header) / sizeof(float);
                                readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                            } else {
                                uint32_t header = 0;
                                if (offset + sizeof(uint32_t) <= appendedData.size()) {
                                    std::memcpy(&header, appendedData.data() + offset, sizeof(uint32_t));
                                    if (bigEndian != mesh_utils::isLittleEndian()) {
                                        mesh_utils::byteSwap(&header);
                                    }
                                }
                                offset += sizeof(uint32_t);
                                size_t count = static_cast<size_t>(header) / sizeof(float);
                                readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                            }
                        } else {
                            decodeBase64(daContent, appendedData);
                            size_t offset = 0;
                            size_t count = appendedData.size() / sizeof(float);
                            readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                            appendedData.clear();
                        }
                    }

                    if (numComp == 3 && !vals.empty()) {
                        size_t actualPoints = vals.size() / 3;
                        points.insert(points.end(), vals.begin(), vals.begin() + actualPoints * 3);
                    } else if (numComp == 1 && !vals.empty()) {
                        size_t actualPoints = vals.size();
                        for (size_t i = 0; i < actualPoints; ++i) {
                            points.push_back(vals[i]);
                            points.push_back(0.0f);
                            points.push_back(0.0f);
                        }
                    }
                }
            }
        }

        // Cells (UnstructuredGrid)
        std::string cellsXml = extractTagContent(content, "Cells");
        if (!cellsXml.empty()) {
            auto parseDataArray = [&](const std::string& parent, const std::string& name) {
                size_t daPos = 0;
                while (true) {
                    size_t daStart = parent.find("<DataArray", daPos);
                    if (daStart == std::string::npos) break;
                    size_t daEnd = parent.find('>', daStart);
                    if (daEnd == std::string::npos) break;
                    std::string daTag = parent.substr(daStart, daEnd - daStart + 1);
                    std::string daName = extractAttribute(daTag, "Name");
                    if (mesh_utils::toUpper(daName) != mesh_utils::toUpper(name)) {
                        daPos = daEnd + 1;
                        continue;
                    }
                    bool selfClose = (!daTag.empty() && daTag.back() == '/');
                    std::string daContent;
                    if (!selfClose) {
                        size_t contentStart = daEnd + 1;
                        size_t closeDa = parent.find("</DataArray>", contentStart);
                        if (closeDa == std::string::npos) break;
                        daContent = parent.substr(contentStart, closeDa - contentStart);
                    }
                    return daContent;
                }
                return std::string();
            };

            std::string connText = parseDataArray(cellsXml, "connectivity");
            std::string offsetsText = parseDataArray(cellsXml, "offsets");
            std::string typesText = parseDataArray(cellsXml, "types");

            auto parseIntArray = [](const std::string& text, std::vector<int32_t>& out) {
                out.clear();
                std::istringstream iss(text);
                int32_t v;
                while (iss >> v) out.push_back(v);
            };

            if (!connText.empty()) parseIntArray(connText, connectivity);
            if (!offsetsText.empty()) parseIntArray(offsetsText, offsets);
            if (!typesText.empty()) parseIntArray(typesText, cellTypes);
        }

        // Coordinates (RectilinearGrid)
        std::string coordsXml = extractTagContent(content, "Coordinates");
        if (!coordsXml.empty()) {
            size_t daPos = 0;
            int coordIdx = 0;
            while (true) {
                size_t daStart = coordsXml.find("<DataArray", daPos);
                if (daStart == std::string::npos) break;
                size_t daEnd = coordsXml.find('>', daStart);
                if (daEnd == std::string::npos) break;
                std::string daTag = coordsXml.substr(daStart, daEnd - daStart + 1);
                bool selfClose = (!daTag.empty() && daTag.back() == '/');
                std::string daContent;
                if (!selfClose) {
                    size_t contentStart = daEnd + 1;
                    size_t closeDa = coordsXml.find("</DataArray>", contentStart);
                    if (closeDa == std::string::npos) break;
                    daContent = coordsXml.substr(contentStart, closeDa - contentStart);
                }
                std::vector<float> vals;
                std::istringstream iss(daContent);
                float v;
                while (iss >> v) vals.push_back(v);
                if (coordIdx == 0) rectX = vals;
                else if (coordIdx == 1) rectY = vals;
                else if (coordIdx == 2) rectZ = vals;
                coordIdx++;
                daPos = daEnd + 1;
            }
        }

        // ImageData extent/origin/spacing
        if (datasetType == "IMAGEDATA") {
            std::string we = extractAttribute(content, "WholeExtent");
            if (!we.empty()) {
                std::istringstream iss(we);
                iss >> wholeExtent[0] >> wholeExtent[1] >> wholeExtent[2] >> wholeExtent[3] >> wholeExtent[4] >> wholeExtent[5];
            }
            std::string org = extractAttribute(content, "Origin");
            if (!org.empty()) {
                std::istringstream iss(org);
                iss >> origin[0] >> origin[1] >> origin[2];
            }
            std::string sp = extractAttribute(content, "Spacing");
            if (!sp.empty()) {
                std::istringstream iss(sp);
                iss >> spacing[0] >> spacing[1] >> spacing[2];
            }
        }

        // PolyData counts
        if (datasetType == "POLYDATA") {
            std::string np = extractAttribute(pieceTag, "NumberOfPoints");
            std::string nv = extractAttribute(pieceTag, "NumberOfVerts");
            std::string nl = extractAttribute(pieceTag, "NumberOfLines");
            std::string ns = extractAttribute(pieceTag, "NumberOfStrips");
            std::string npy = extractAttribute(pieceTag, "NumberOfPolys");
            if (!np.empty()) piecePoints = std::stoi(np);
            if (!nv.empty()) numVerts += std::stoi(nv);
            if (!nl.empty()) numLines += std::stoi(nl);
            if (!ns.empty()) numStrips += std::stoi(ns);
            if (!npy.empty()) numPolys += std::stoi(npy);
        }

        // Data arrays (PointData / CellData)
        auto processDataBlock = [&](const std::string& blockName, bool isPoint) {
            std::string blockXml = extractTagContent(content, blockName);
            if (blockXml.empty()) return;

            size_t daPos = 0;
            while (true) {
                size_t daStart = blockXml.find("<DataArray", daPos);
                if (daStart == std::string::npos) break;
                size_t daEnd = blockXml.find('>', daStart);
                if (daEnd == std::string::npos) break;
                std::string daTag = blockXml.substr(daStart, daEnd - daStart + 1);
                std::string daName = extractAttribute(daTag, "Name");
                std::string typeStr = mesh_utils::toUpper(extractAttribute(daTag, "type"));
                int numComp = 1;
                std::string ncStr = extractAttribute(daTag, "NumberOfComponents");
                if (!ncStr.empty()) numComp = std::stoi(ncStr);
                std::string fmt = mesh_utils::toUpper(extractAttribute(daTag, "format"));
                if (fmt.empty()) fmt = "ASCII";

                bool selfClose = (!daTag.empty() && daTag.back() == '/');
                std::string daContent;
                if (!selfClose) {
                    size_t contentStart = daEnd + 1;
                    size_t closeDa = blockXml.find("</DataArray>", contentStart);
                    if (closeDa == std::string::npos) break;
                    daContent = blockXml.substr(contentStart, closeDa - contentStart);
                }

                std::vector<float> vals;
                if (fmt == "ASCII") {
                    std::istringstream iss(daContent);
                    float v;
                    while (iss >> v) vals.push_back(v);
                } else if (fmt == "BINARY" || fmt == "APPENDED") {
                    if (fmt == "APPENDED") {
                        size_t offset = 0;
                        std::string offsetStr = extractAttribute(daTag, "offset");
                        if (!offsetStr.empty()) offset = static_cast<size_t>(std::stoull(offsetStr));
                        if (header64) {
                            uint64_t header = 0;
                            if (offset + sizeof(uint64_t) <= appendedData.size()) {
                                std::memcpy(&header, appendedData.data() + offset, sizeof(uint64_t));
                                if (bigEndian != mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&header);
                            }
                            offset += sizeof(uint64_t);
                            size_t count = static_cast<size_t>(header) / sizeof(float);
                            readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                        } else {
                            uint32_t header = 0;
                            if (offset + sizeof(uint32_t) <= appendedData.size()) {
                                std::memcpy(&header, appendedData.data() + offset, sizeof(uint32_t));
                                if (bigEndian != mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&header);
                            }
                            offset += sizeof(uint32_t);
                            size_t count = static_cast<size_t>(header) / sizeof(float);
                            readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                        }
                    } else {
                        decodeBase64(daContent, appendedData);
                        size_t offset = 0;
                        size_t count = appendedData.size() / sizeof(float);
                        readFromBuffer(appendedData.data(), appendedData.size(), offset, count, vals, bigEndian != mesh_utils::isLittleEndian());
                        appendedData.clear();
                    }
                }

                if (vals.empty()) continue;

                if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

                if (isPoint) {
                    if (numComp == 1) {
                        mesh.attributes->pointScalars[daName] = vals;
                        if (mesh.scalarName.empty()) mesh.scalarName = daName;
                    } else if (numComp == 3) {
                        mesh.attributes->pointVectors[daName] = vals;
                        if (mesh.vectorName.empty()) mesh.vectorName = daName;
                    }
                } else {
                    if (numComp == 1) {
                        mesh.attributes->cellScalars[daName] = vals;
                        cellScalarsStorage[daName] = vals;
                    } else if (numComp == 3) {
                        mesh.attributes->pointVectors[daName] = vals;
                        cellVectorsStorage[daName] = vals;
                    }
                }
                daPos = daEnd + 1;
            }
        };

        processDataBlock("PointData", true);
        processDataBlock("CellData", false);

        // PolyData topology blocks
        if (datasetType == "POLYDATA") {
            auto parsePolyDataBlock = [&](const std::string& blockName, std::vector<int32_t>& out, int& count) {
                std::string blockXml = extractTagContent(content, blockName);
                if (blockXml.empty()) return;
                size_t daStart = blockXml.find("<DataArray");
                if (daStart == std::string::npos) return;
                size_t daEnd = blockXml.find('>', daStart);
                if (daEnd == std::string::npos) return;
                std::string daTag = blockXml.substr(daStart, daEnd - daStart + 1);
                bool selfClose = (!daTag.empty() && daTag.back() == '/');
                std::string daContent;
                if (!selfClose) {
                    size_t contentStart = daEnd + 1;
                    size_t closeDa = blockXml.find("</DataArray>", contentStart);
                    if (closeDa == std::string::npos) return;
                    daContent = blockXml.substr(contentStart, closeDa - contentStart);
                }
                std::istringstream iss(daContent);
                int32_t v;
                while (iss >> v) out.push_back(v);
                std::string cntStr = extractAttribute(daTag, "NumberOfTuples");
                if (!cntStr.empty()) count = std::stoi(cntStr);
            };
            parsePolyDataBlock("Polys", polys, numPolys);
            parsePolyDataBlock("Lines", lines, numLines);
            parsePolyDataBlock("Strips", strips, numStrips);
            parsePolyDataBlock("Verts", verts, numVerts);
        }
    }

    void buildTopology() {
        if (datasetType == "UNSTRUCTUREDGRID" && !connectivity.empty() && !offsets.empty() && !cellTypes.empty()) {
            mesh.vertices = points;
            // Convert XML connectivity+offsets+types to legacy flat cell format
            std::vector<int32_t> legacyCells;
            for (int c = 0; c < static_cast<int>(offsets.size()); ++c) {
                int start = (c == 0) ? 0 : offsets[c - 1];
                int end = offsets[c];
                legacyCells.push_back(end - start);
                for (int i = start; i < end; ++i) {
                    legacyCells.push_back(connectivity[i]);
                }
            }
            int totalCells = static_cast<int>(offsets.size());
            globalCellToVertices = triangulateUnstructuredCells(mesh, legacyCells, cellTypes, totalCells);
            mesh.supportsCellGrid = true;
        } else if (datasetType == "STRUCTUREDGRID" && !points.empty()) {
            mesh.vertices = points;
            int dX = wholeExtent[1] - wholeExtent[0] + 1;
            int dY = wholeExtent[3] - wholeExtent[2] + 1;
            int dZ = wholeExtent[5] - wholeExtent[4] + 1;
            if (dX <= 0) dX = 1;
            if (dY <= 0) dY = 1;
            if (dZ <= 0) dZ = 1;
            generateStructuredGridSurface(mesh, dX, dY, dZ);
            globalCellToVertices.reserve(static_cast<size_t>(std::max(1, dX - 1)) *
                                         std::max(1, dY - 1) *
                                         std::max(1, dZ - 1));
            // Reconstruct cellToVertices for cell edges
            auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
            int cx = std::max(1, dX - 1), cy = std::max(1, dY - 1), cz = std::max(1, dZ - 1);
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    for (int x = 0; x + 1 < dX; ++x) {
                        uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                        uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                        uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                        uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                        uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                        uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                        uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                        uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                        globalCellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                    }
            mesh.supportsCellGrid = true;
        } else if (datasetType == "IMAGEDATA") {
            int dX = wholeExtent[1] - wholeExtent[0] + 1;
            int dY = wholeExtent[3] - wholeExtent[2] + 1;
            int dZ = wholeExtent[5] - wholeExtent[4] + 1;
            if (dX <= 0) dX = 1;
            if (dY <= 0) dY = 1;
            if (dZ <= 0) dZ = 1;
            numPoints = dX * dY * dZ;
            mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
            int vIdx = 0;
            for (int z = 0; z < dZ; ++z) {
                for (int y = 0; y < dY; ++y) {
                    for (int x = 0; x < dX; ++x) {
                        mesh.vertices[vIdx++] = origin[0] + (wholeExtent[0] + x) * spacing[0];
                        mesh.vertices[vIdx++] = origin[1] + (wholeExtent[2] + y) * spacing[1];
                        mesh.vertices[vIdx++] = origin[2] + (wholeExtent[4] + z) * spacing[2];
                    }
                }
            }
            if (!points.empty() && points.size() == mesh.vertices.size()) {
                mesh.vertices = points;
            }
            generateStructuredGridSurface(mesh, dX, dY, dZ);
            int cx = std::max(1, dX - 1), cy = std::max(1, dY - 1), cz = std::max(1, dZ - 1);
            globalCellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);
            auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    for (int x = 0; x + 1 < dX; ++x) {
                        uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                        uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                        uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                        uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                        uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                        uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                        uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                        uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                        globalCellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                    }
            mesh.supportsCellGrid = true;
            mesh.renderAsPoints = true;
        } else if (datasetType == "RECTILINEARGRID" && !rectX.empty() && !rectY.empty() && !rectZ.empty()) {
            int dX = static_cast<int>(rectX.size());
            int dY = static_cast<int>(rectY.size());
            int dZ = static_cast<int>(rectZ.size());
            numPoints = dX * dY * dZ;
            mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
            int vIdx = 0;
            for (int z = 0; z < dZ; ++z) {
                for (int y = 0; y < dY; ++y) {
                    for (int x = 0; x < dX; ++x) {
                        mesh.vertices[vIdx++] = rectX[x];
                        mesh.vertices[vIdx++] = rectY[y];
                        mesh.vertices[vIdx++] = rectZ[z];
                    }
                }
            }
            generateStructuredGridSurface(mesh, dX, dY, dZ);
            int cx = std::max(1, dX - 1), cy = std::max(1, dY - 1), cz = std::max(1, dZ - 1);
            globalCellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);
            auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    for (int x = 0; x + 1 < dX; ++x) {
                        uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                        uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                        uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                        uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                        uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                        uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                        uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                        uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                        globalCellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                    }
            mesh.supportsCellGrid = true;
        } else if (datasetType == "POLYDATA") {
            mesh.vertices = points;
            if (!polys.empty()) {
                globalCellToVertices = triangulatePolygons(mesh, polys, numPolys);
                mesh.supportsCellGrid = true;
            }
            if (!lines.empty()) {
                triangulateLines(mesh, lines, numLines);
                mesh.supportsCellGrid = true;
            }
            if (!strips.empty()) {
                triangulateTriangleStrips(mesh, strips, numStrips);
                mesh.supportsCellGrid = true;
            }
            if (mesh.indices.empty() && !mesh.vertices.empty()) {
                std::cerr << "VTK XML Parser Warning: POLYDATA has points but no polys/lines/strips; rendering points only." << std::endl;
                mesh.renderAsPoints = true;
            }
        }
    }

    void finalizeMeshData() {
        if (mesh.vertices.empty()) {
            std::cerr << "VTK XML Parser Error: Empty data sequence." << std::endl;
            return;
        }
        if (mesh.indices.empty() && datasetType != "POLYDATA" && !mesh.renderAsPoints) {
            std::cerr << "VTK XML Parser Error: topology produced no triangles and not a point set; mesh will not render." << std::endl;
            return;
        }

        {
            uint32_t vCount = static_cast<uint32_t>(mesh.vertices.size() / 3);
            bool badIndex = false;
            for (uint32_t idx : mesh.indices) {
                if (idx >= vCount) { badIndex = true; break; }
            }
            if (badIndex) {
                std::cerr << "VTK XML Parser Error: topology references vertex index >= vertex count ("
                          << vCount << "); dropping indices." << std::endl;
                mesh.indices.clear();
                return;
            }
        }

        extrapolateCellDataToPointsMerged(mesh, globalCellToVertices, cellScalarsStorage, cellVectorsStorage);

        if (mesh.attributes.has_value()) {
            size_t perVertex = mesh.vertices.size() / 3;
            mesh.pointVectorCount = perVertex;
            for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
                if (vecArr.size() < perVertex * 3) continue;
                mesh.pointVectorOffset[name] = mesh.pointVectorsData.size();
                for (size_t v = 0; v < perVertex; ++v) {
                    mesh.pointVectorsData.emplace_back(
                        vecArr[v * 3 + 0], vecArr[v * 3 + 1], vecArr[v * 3 + 2]);
                }
            }
        }

        bool hasAttributes = mesh.attributes.has_value();
        if (hasAttributes && !mesh.attributes->pointScalars.empty()) {
            if (mesh.scalarName.empty() || !mesh.attributes->pointScalars.count(mesh.scalarName)) {
                mesh.scalarName = mesh.attributes->pointScalars.begin()->first;
            }
            const std::vector<float>& active = mesh.attributes->pointScalars[mesh.scalarName];
            size_t vCount = mesh.vertices.size() / 3;
            if (!active.empty() && active.size() == vCount) {
                mesh.scalars = active;
            } else {
                std::cerr << "VTK XML Parser Warning: active scalar '" << mesh.scalarName
                          << "' is not 1-component per vertex; scalar coloring disabled." << std::endl;
                mesh.scalars.clear();
            }
        } else {
            mesh.scalars.clear();
        }

        if (hasAttributes) {
            for (const auto& [name, _] : mesh.attributes->pointScalars) {
                mesh.availableScalarNames.push_back(name);
            }
            for (const auto& [name, _] : mesh.attributes->pointVectors) {
                mesh.availableVectorNames.push_back(name);
            }
        }
        if (mesh.vectorName.empty() && !mesh.availableVectorNames.empty()) {
            mesh.vectorName = mesh.availableVectorNames.front();
        }

        mesh_utils::computeBounds(mesh);
        mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

        mesh.flatVerts.reserve(mesh.indices.size() * 3);
        for (uint32_t i : mesh.indices) {
            const float* p = &mesh.vertices[i * 3];
            mesh.flatVerts.insert(mesh.flatVerts.end(), { p[0], p[1], p[2] });
        }

        if (mesh.normals.empty() && !mesh.indices.empty()) {
            mesh_utils::computeNormals(mesh);
        }
    }
};

RenderMesh parseVTKXML(const std::string& filePath) {
    VTKXMLParserContext parser(filePath);
    return parser.parse();
}
