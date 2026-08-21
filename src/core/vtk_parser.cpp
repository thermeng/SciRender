#include "core/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <charconv>


void alignStream4(std::ifstream& f, size_t bytesRead) {
    std::streamoff pad = (static_cast<std::streamoff>(4) - (static_cast<std::streamoff>(bytesRead) & 3)) & 3;
    if (pad) {
        std::streampos p = f.tellg();
        if (p == std::streampos(-1)) return;
        f.seekg(p + pad);
    }
}

// ── Parser Context ──────────────────────────────────────────────────────────

class VTKParserContext {
public:
    explicit VTKParserContext(const std::string& filePath) : filePath(filePath) {}

    RenderMesh parse() {
        // Nested under the bounds structure now
        mesh.bounds.centerX = mesh.bounds.centerY = mesh.bounds.centerZ = 0.0;
        mesh.bounds.extent = 1.0;

        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "VTK Parser Error: Failed to open: " << filePath << std::endl;
            return mesh;
        }

        std::string line;
        while (std::getline(file, line)) {
            const char* raw = line.data();
            const char* rawEnd = raw + line.size();

            // Skip leading whitespace (zero-alloc trim)
            const char* p = mesh_utils::skipWhitespace(raw, rawEnd);
            if (p >= rawEnd || *p == '#') continue;

            // Extract first token via pointer scan (zero-alloc)
            const char* tokStart = p;
            p = mesh_utils::skipToken(p, rawEnd);
            std::string_view token(tokStart, p - tokStart);

            // Skip whitespace before rest of line
            p = mesh_utils::skipWhitespace(p, rawEnd);

            processToken(token, p, rawEnd, file);
        }
        file.close();

        buildTopology();
        finalizeMeshData();

        mesh.datasetType = datasetType.empty() ? "UNKNOWN" : datasetType;
        mesh.fileFormat = "VTK";
        if (datasetType == "STRUCTURED_POINTS" || datasetType == "STRUCTURED_GRID" || datasetType == "RECTILINEAR_GRID") {
            mesh.gridDimX = dimX;
            mesh.gridDimY = dimY;
            mesh.gridDimZ = dimZ;
        }
        return mesh;
    }

private:
    std::string filePath;
    RenderMesh mesh;
    std::string datasetType = "";
    bool isBinary = false;
    bool isLittleEndianFile = false;

    int dimX = 0, dimY = 0, dimZ = 0;
    int numPoints = 0, numCells = 0, cellSize = 0;
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float spacing[3] = { 1.0f, 1.0f, 1.0f };

    bool readingPointData = true;
    bool readingFieldData = false;
    int attributeTargetCount = 0;
    int fieldDataCount = 0;

    std::vector<float> rectX, rectY, rectZ;
    std::vector<int32_t> rawCellData, cellTypes;

    // Intermediate storage cache specifically tracking split configurations
    std::unordered_map<std::string, std::vector<float>> cellScalarsStorage;
    std::unordered_map<std::string, std::vector<float>> cellVectorsStorage; // CELL_DATA VECTORS, extrapolated to points
    std::vector<std::vector<uint32_t>> globalCellToVertices;

    template<typename T>
    bool readBinaryArray(std::ifstream& f, size_t count, std::vector<T>& out) {
        out.resize(count);
        if (count == 0) return true;
        const std::streamsize want = static_cast<std::streamsize>(count * sizeof(T));
        f.read(reinterpret_cast<char*>(out.data()), want);
        if (f.gcount() != want) return false;
        if (mesh_utils::isLittleEndian() != isLittleEndianFile) {
            if constexpr (sizeof(T) == 4) {
                uint32_t* words = reinterpret_cast<uint32_t*>(out.data());
                for (size_t i = 0; i < count; ++i) {
                    uint32_t v = words[i];
                    words[i] = ((v >> 24) & 0x000000FF)
                             | ((v >>  8) & 0x0000FF00)
                             | ((v <<  8) & 0x00FF0000)
                             | ((v << 24) & 0xFF000000);
                }
            } else if constexpr (sizeof(T) == 8) {
                uint64_t* words = reinterpret_cast<uint64_t*>(out.data());
                for (size_t i = 0; i < count; ++i) {
                    uint64_t v = words[i];
                    words[i] = ((v >> 56) & 0x00000000000000FFULL)
                             | ((v >> 40) & 0x000000000000FF00ULL)
                             | ((v >> 24) & 0x0000000000FF0000ULL)
                             | ((v >>  8) & 0x00000000FF000000ULL)
                             | ((v <<  8) & 0x000000FF00000000ULL)
                             | ((v << 24) & 0x0000FF0000000000ULL)
                             | ((v << 40) & 0x00FF000000000000ULL)
                             | ((v << 56) & 0xFF00000000000000ULL);
                }
            } else if constexpr (sizeof(T) == 2) {
                uint16_t* words = reinterpret_cast<uint16_t*>(out.data());
                for (size_t i = 0; i < count; ++i) {
                    uint16_t v = words[i];
                    words[i] = static_cast<uint16_t>((v >> 8) | (v << 8));
                }
            } else {
                for (size_t i = 0; i < count; ++i) mesh_utils::byteSwap(&out[i]);
            }
        }
        alignStream4(f, count * sizeof(T));
        return true;
    }

    void processToken(std::string_view token, const char* lineRest, const char* lineEnd, std::ifstream& file) {
        // First-char dispatch + iequal for remaining — avoids string copies entirely.
        if (token == "BINARY") { isBinary = true; isLittleEndianFile = false; }
        else if (token == "BINARY_LE") { isBinary = true; isLittleEndianFile = true; }
        else if (token == "ASCII") { isBinary = false; }
        else if (token[0] == 'D' && mesh_utils::iequal(token, "DATASET")) {
            // Read dataset type from rest of line
            const char* p = mesh_utils::skipWhitespace(lineRest, lineEnd);
            const char* tStart = p;
            p = mesh_utils::skipToken(p, lineEnd);
            datasetType = std::string(tStart, p - tStart);
            mesh_utils::toUpperInPlace(datasetType);
        }
        else if (token[0] == 'D' && mesh_utils::iequal(token, "DIMENSIONS")) { handleDimensions(lineRest, lineEnd); }
        else if (token[0] == 'O' && mesh_utils::iequal(token, "ORIGIN")) {
            const char* p = lineRest;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            auto [p0, e0] = std::from_chars(p, lineEnd, origin[0]); p = p0;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            auto [p1, e1] = std::from_chars(p, lineEnd, origin[1]); p = p1;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            std::from_chars(p, lineEnd, origin[2]);
        }
        else if (token[0] == 'S' && (mesh_utils::iequal(token, "SPACING") || mesh_utils::iequal(token, "ASPECT_RATIO"))) {
            const char* p = lineRest;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            auto [p0, e0] = std::from_chars(p, lineEnd, spacing[0]); p = p0;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            auto [p1, e1] = std::from_chars(p, lineEnd, spacing[1]); p = p1;
            p = mesh_utils::skipWhitespace(p, lineEnd);
            std::from_chars(p, lineEnd, spacing[2]);
        }
        else if (token[0] == 'X' && mesh_utils::iequal(token, "X_COORDINATES")) { parseRectilinearAxis(file, rectX, lineRest, lineEnd); }
        else if (token[0] == 'Y' && mesh_utils::iequal(token, "Y_COORDINATES")) { parseRectilinearAxis(file, rectY, lineRest, lineEnd); }
        else if (token[0] == 'Z' && mesh_utils::iequal(token, "Z_COORDINATES")) { parseRectilinearAxis(file, rectZ, lineRest, lineEnd); }
        else if (token[0] == 'P' && mesh_utils::iequal(token, "POINT_DATA")) {
            const char* p = mesh_utils::skipWhitespace(lineRest, lineEnd);
            std::from_chars(p, lineEnd, attributeTargetCount);
            readingPointData = true;
            readingFieldData = false;
        }
        else if (token[0] == 'C' && mesh_utils::iequal(token, "CELL_DATA")) {
            const char* p = mesh_utils::skipWhitespace(lineRest, lineEnd);
            std::from_chars(p, lineEnd, attributeTargetCount);
            readingPointData = false;
            readingFieldData = false;
        }
        else if (token[0] == 'P' && mesh_utils::iequal(token, "POINTS")) { parsePointsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'C' && mesh_utils::iequal(token, "CELLS")) { parseCellsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'C' && mesh_utils::iequal(token, "CELL_TYPES")) { parseCellTypesBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'P' && mesh_utils::iequal(token, "POLYGONS")) { parsePolygonsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'V' && mesh_utils::iequal(token, "VERTICES")) { parseVerticesBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'L' && mesh_utils::iequal(token, "LINES")) { parseLinesBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'T' && mesh_utils::iequal(token, "TRIANGLE_STRIPS")) { parseTriangleStripsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'T' && mesh_utils::iequal(token, "TEXTURE_COORDINATES")) { parseTexCoordBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'T' && mesh_utils::iequal(token, "TENSORS")) { parseTensorsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'S' && mesh_utils::iequal(token, "SCALARS")) { parseScalarsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'N' && mesh_utils::iequal(token, "NORMALS")) { parseNormalsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'V' && mesh_utils::iequal(token, "VECTORS")) { parseVectorsBlock(lineRest, lineEnd, file); }
        else if (token[0] == 'F' && mesh_utils::iequal(token, "FIELD_DATA")) {
            const char* p = mesh_utils::skipWhitespace(lineRest, lineEnd);
            auto [np, ec] = std::from_chars(p, lineEnd, fieldDataCount);
            if (ec != std::errc()) fieldDataCount = 0;
            readingFieldData = true;
        }
        else if (token[0] == 'F' && mesh_utils::iequal(token, "FIELD")) { parseFieldBlock(lineRest, lineEnd, file); }
    }

    void handleDimensions(const char* lineRest, const char* lineEnd) {
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, dimX); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p1, e1] = std::from_chars(p, lineEnd, dimY); p = p1;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, dimZ);
        numPoints = dimX * dimY * dimZ;
        if (datasetType == "STRUCTURED_POINTS" || datasetType == "STRUCTURED_GRID" || datasetType == "RECTILINEAR_GRID") {
            numCells = std::max(1, dimX - 1) * std::max(1, dimY - 1) * std::max(1, dimZ - 1);
        }
    }

    void parseRectilinearAxis(std::ifstream& file, std::vector<float>& axisCoords, const char* lineRest, const char* lineEnd) {
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        int count = 0;
        auto [p0, e0] = std::from_chars(p, lineEnd, count); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        std::string type(tStart, p - tStart);
        mesh_utils::toUpperInPlace(type);
        axisCoords.resize(count);
        if (isBinary) {
            std::vector<float> tmpFloat;
            if (!readBinaryFloatArray(file, type, count, tmpFloat) || tmpFloat.size() != static_cast<size_t>(count)) {
                std::cerr << "VTK Parser Warning: short read or unsupported type on binary X/Y/Z_COORDINATES (" << type << ")." << std::endl;
                return;
            }
            for (float v : tmpFloat) {
                if (!std::isfinite(v)) {
                    std::cerr << "VTK Parser Warning: non-finite coordinate in X/Y/Z_COORDINATES; clearing axis." << std::endl;
                    axisCoords.clear();
                    return;
                }
            }
            axisCoords = std::move(tmpFloat);
        }
        else {
            if (type == "DOUBLE") {
                std::vector<double> tmp;
                mesh_utils::readAsciiValues(file, count, tmp);
                axisCoords.resize(tmp.size());
                for (size_t i = 0; i < tmp.size(); ++i) axisCoords[i] = static_cast<float>(tmp[i]);
            } else {
                mesh_utils::readAsciiValues(file, count, axisCoords);
            }
        }
    }

    void parsePointsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        long long parsedPoints = 0;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, parsedPoints); p = p0;
     
        if (datasetType == "STRUCTURED_POINTS" || datasetType == "STRUCTURED_GRID" ||
            datasetType == "RECTILINEAR_GRID") {
            const long long expected = static_cast<long long>(dimX) *
                                       static_cast<long long>(dimY) *
                                       static_cast<long long>(dimZ);
            if (expected > 0 && parsedPoints != expected) {
                std::cerr << "VTK Parser Warning: POINTS count (" << parsedPoints
                          << ") != DIMENSIONS product (" << expected
                          << "); using the POINTS count." << std::endl;
            }
        }
        if (parsedPoints <= 0 || parsedPoints > 2000000000LL) {
            std::cerr << "VTK Parser Warning: invalid POINTS count (" << parsedPoints
                      << "); skipping POINTS block." << std::endl;
            
            clearTrailingLine(file);
            mesh.vertices.clear();
            numPoints = 0;
            return;
        }
        numPoints = static_cast<int>(parsedPoints);
        // Read data type from rest of line
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        std::string dataType(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);
        mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);

        if (isBinary) {
            std::vector<float> tempVerts(static_cast<size_t>(numPoints) * 3);
            if (!readBinaryFloatArray(file, dataType, tempVerts.size(), tempVerts)) {
                std::cerr << "VTK Parser Warning: short read or unsupported type on binary POINTS (" << dataType << ")." << std::endl;
                mesh.vertices.clear();
                return;
            }
            for (float v : tempVerts) {
                if (!std::isfinite(v)) {
                    std::cerr << "VTK Parser Warning: non-finite POINTS coordinate; dropping mesh." << std::endl;
                    mesh.vertices.clear();
                    return;
                }
            }
            mesh.vertices = std::move(tempVerts);
        }
        else {
            size_t expected = static_cast<size_t>(numPoints) * 3;
            size_t n = mesh_utils::readAsciiValues(file, expected, mesh.vertices);
            if (n != expected) {
                std::cerr << "VTK Parser Warning: short read on ASCII POINTS; dropping mesh." << std::endl;
                mesh.vertices.clear();
                return;
            }
            for (float v : mesh.vertices) {
                if (!std::isfinite(v)) {
                    std::cerr << "VTK Parser Warning: non-finite POINTS coordinate; dropping mesh." << std::endl;
                    mesh.vertices.clear();
                    return;
                }
            }
        }
    }

    void parseCellsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, numCells); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, cellSize);
        rawCellData.resize(cellSize);
        if (isBinary) {
            if (!readBinaryArray(file, cellSize, rawCellData)) {
                std::cerr << "VTK Parser Warning: short read on binary CELLS." << std::endl;
                rawCellData.clear();
                return;
            }
        }
        else {
            mesh_utils::readAsciiValues(file, cellSize, rawCellData);
        }
    }

    void parseCellTypesBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, numCells); cellTypes.resize(numCells);
        if (isBinary) {
            std::vector<int32_t> rawTypes(numCells);
            if (!readBinaryArray(file, numCells, rawTypes)) {
                std::cerr << "VTK Parser Warning: short read on binary CELL_TYPES." << std::endl;
                cellTypes.clear();
                return;
            }
            for (int i = 0; i < numCells; ++i) cellTypes[i] = rawTypes[i];
        }
        else {
            mesh_utils::readAsciiValues(file, numCells, cellTypes);
        }
    }

    void parsePolygonsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        int numPolys = 0, sizeOfPolysBlock = 0;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, numPolys); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, sizeOfPolysBlock);
        std::vector<int32_t> polyData(sizeOfPolysBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfPolysBlock, polyData)) {
                std::cerr << "VTK Parser Warning: short read on binary POLYGONS." << std::endl;
                polyData.clear();
                return;
            }
        }
        else {
            mesh_utils::readAsciiValues(file, sizeOfPolysBlock, polyData);
        }
        auto cells = triangulatePolygons(polyData, numPolys);
        globalCellToVertices.insert(globalCellToVertices.end(), cells.begin(), cells.end());
        numCells += numPolys;
    }

    void parseVerticesBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        int numVerts = 0, sizeOfVertsBlock = 0;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, numVerts); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, sizeOfVertsBlock);
        std::vector<int32_t> vertData(sizeOfVertsBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfVertsBlock, vertData)) {
                std::cerr << "VTK Parser Warning: short read on binary VERTICES." << std::endl;
                vertData.clear();
                return;
            }
        }
        else {
            mesh_utils::readAsciiValues(file, sizeOfVertsBlock, vertData);
        }
        auto cells = triangulatePolygons(vertData, numVerts);
        globalCellToVertices.insert(globalCellToVertices.end(), cells.begin(), cells.end());
        numCells += numVerts;
    }


    void parseLinesBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        int numLines = 0, sizeOfLinesBlock = 0;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, numLines); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, sizeOfLinesBlock);
        std::vector<int32_t> lineData(sizeOfLinesBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfLinesBlock, lineData)) {
                std::cerr << "VTK Parser Warning: short read on binary LINES." << std::endl;
                lineData.clear();
                return;
            }
        }
        else {
            mesh_utils::readAsciiValues(file, sizeOfLinesBlock, lineData);
        }
        // LINES is its own topology source (see parsePolygonsBlock).
        auto cells = triangulateLines(lineData, numLines);
        globalCellToVertices.insert(globalCellToVertices.end(), cells.begin(), cells.end());
        numCells += numLines;
    }

    void parseTriangleStripsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        int numStrips = 0, sizeOfStripsBlock = 0;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        auto [p0, e0] = std::from_chars(p, lineEnd, numStrips); p = p0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        std::from_chars(p, lineEnd, sizeOfStripsBlock);
        std::vector<int32_t> stripData(sizeOfStripsBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfStripsBlock, stripData)) {
                std::cerr << "VTK Parser Warning: short read on binary TRIANGLE_STRIPS." << std::endl;
                stripData.clear();
                return;
            }
        }
        else {
            mesh_utils::readAsciiValues(file, sizeOfStripsBlock, stripData);
        }
        auto cells = triangulateTriangleStrips(stripData, numStrips);
        globalCellToVertices.insert(globalCellToVertices.end(), cells.begin(), cells.end());
        // ponytail: cells are now per-triangle (see triangulateTriangleStrips),
        // so numCells tracks the real cell count, not the raw strip count.
        numCells += static_cast<int>(cells.size());
    }

    void parseScalarsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string scalarName, dataType; int numComponents = 1;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        // Read scalar name
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        scalarName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        // Read data type
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        // Read numComponents (optional, defaults to 1)
        if (p < lineEnd) {
            auto [np, ec] = std::from_chars(p, lineEnd, numComponents);
            if (ec != std::errc()) numComponents = 1;
        }
        if (numComponents < 1) numComponents = 1;

        std::string lutName = "default";
        int lutCount = 0;
        {
            std::string lutLine;
            std::getline(file, lutLine);
            // Parse "LOOKUP_TABLE name n" (n optional — "default" has no trailing count)
            const char* lp = lutLine.data();
            const char* le = lp + lutLine.size();
            lp = mesh_utils::skipToken(lp, le); // skip "LOOKUP_TABLE"
            lp = mesh_utils::skipWhitespace(lp, le);
            const char* nameStart = lp;
            lp = mesh_utils::skipToken(lp, le);
            lutName = std::string(nameStart, lp - nameStart);
            lp = mesh_utils::skipWhitespace(lp, le);
            if (lp < le) {
                auto [nres, ec] = std::from_chars(lp, le, lutCount);
                if (ec != std::errc()) lutCount = 0;
            }
            if (lutName == "default") lutCount = 0;
        }
        if (lutCount > 0) consumeLookupTable(file, dataType, lutCount);

        int activeElementCount = readingFieldData ? fieldDataCount : (readingPointData ? numPoints : numCells);
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        if (!readingFieldData && readingPointData && numComponents == 1 && mesh.scalarName.empty()) {
            mesh.scalarName = scalarName;
        }

        if (numComponents != 1) {
            std::cerr << "VTK Parser Warning: SCALARS '" << scalarName
                      << "' has " << numComponents << " components; only 1-component "
                      << "scalars are colorable, storing as field data." << std::endl;
            const size_t total = static_cast<size_t>(activeElementCount) * static_cast<size_t>(numComponents);
            std::vector<float> multiCompData(total);
            bool readOk = false;
            if (isBinary) {
                if (dataType == "DOUBLE") {
                    std::vector<double> temp(total);
                    if (readBinaryArray(file, temp.size(), temp)) {
                        for (size_t i = 0; i < temp.size(); ++i) multiCompData[i] = static_cast<float>(temp[i]);
                        readOk = true;
                    }
                }
                else if (dataType == "FLOAT") {
                    if (readBinaryArray(file, multiCompData.size(), multiCompData)) readOk = true;
                }
                else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG") {
                    std::vector<int32_t> temp(total);
                    if (readBinaryArray(file, temp.size(), temp)) {
                        for (size_t i = 0; i < temp.size(); ++i) multiCompData[i] = static_cast<float>(temp[i]);
                        readOk = true;
                    }
                }
                else if (dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
                    std::vector<int64_t> temp(total);
                    if (readBinaryArray(file, temp.size(), temp)) {
                        for (size_t i = 0; i < temp.size(); ++i) multiCompData[i] = static_cast<float>(temp[i]);
                        readOk = true;
                    }
                }
                else if (dataType == "SHORT" || dataType == "UNSIGNED_SHORT") {
                    std::vector<int16_t> temp(total);
                    if (readBinaryArray(file, temp.size(), temp)) {
                        for (size_t i = 0; i < temp.size(); ++i) multiCompData[i] = static_cast<float>(temp[i]);
                        readOk = true;
                    }
                }
                else if (dataType == "UNSIGNED_CHAR") {
                    std::vector<uint8_t> temp(total);
                    if (readBinaryArray(file, temp.size(), temp)) {
                        for (size_t i = 0; i < temp.size(); ++i) multiCompData[i] = static_cast<float>(temp[i]);
                        readOk = true;
                    }
                }
                else {
                    std::cerr << "VTK Parser Warning: unsupported SCALARS type '" << dataType
                              << "' for binary data; skipping field." << std::endl;
                }
                if (!readOk) {
                    std::cerr << "VTK Parser Warning: short read on binary multi-component SCALARS '" << scalarName
                              << "'; skipping field to avoid stream desync." << std::endl;
                    multiCompData.clear();
                }
            }
            else {
                mesh_utils::readAsciiValues(file, total, multiCompData);
                if (multiCompData.size() != total) {
                    std::cerr << "VTK Parser Warning: short read consuming multi-component ASCII SCALARS '" << scalarName << "'." << std::endl;
                    multiCompData.clear();
                }
            }

            if (!multiCompData.empty()) {
                if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
                if (readingFieldData) {
                    mesh.attributes->fieldData[scalarName] = std::move(multiCompData);
                    if (numComponents > 1) mesh.attributes->pointFieldComponents[scalarName] = numComponents;
                }
                else if (readingPointData) {
                    mesh.attributes->pointFieldData[scalarName] = std::move(multiCompData);
                    mesh.attributes->pointFieldComponents[scalarName] = numComponents;
                }
                else {
                    mesh.attributes->cellFieldData[scalarName] = std::move(multiCompData);
                    mesh.attributes->cellFieldComponents[scalarName] = numComponents;
                }
            }
            return;
        }

        std::vector<float> readScalars(activeElementCount);

      
        bool binaryReadOk = false;
        bool badField = false;

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tempDouble(readScalars.size());
                if (readBinaryArray(file, tempDouble.size(), tempDouble)) {
                    for (size_t i = 0; i < tempDouble.size(); ++i) readScalars[i] = static_cast<float>(tempDouble[i]);
                    binaryReadOk = true;
                }
            }
            else if (dataType == "FLOAT") {
                if (readBinaryArray(file, readScalars.size(), readScalars)) binaryReadOk = true;
            }
            else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG") {
                std::vector<int32_t> tempInts(readScalars.size());
                if (readBinaryArray(file, tempInts.size(), tempInts)) {
                    for (size_t i = 0; i < tempInts.size(); ++i) readScalars[i] = static_cast<float>(tempInts[i]);
                    binaryReadOk = true;
                }
            }
            else if (dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
                std::vector<int64_t> tempLongs(readScalars.size());
                if (readBinaryArray(file, tempLongs.size(), tempLongs)) {
                    for (size_t i = 0; i < tempLongs.size(); ++i) readScalars[i] = static_cast<float>(tempLongs[i]);
                    binaryReadOk = true;
                }
            }
            else if (dataType == "SHORT" || dataType == "UNSIGNED_SHORT") {
                std::vector<int16_t> tempShorts(readScalars.size());
                if (readBinaryArray(file, tempShorts.size(), tempShorts)) {
                    for (size_t i = 0; i < tempShorts.size(); ++i) readScalars[i] = static_cast<float>(tempShorts[i]);
                    binaryReadOk = true;
                }
            }
            else if (dataType == "UNSIGNED_CHAR") {
                std::vector<uint8_t> tempBytes(readScalars.size());
                if (readBinaryArray(file, tempBytes.size(), tempBytes)) {
                    for (size_t i = 0; i < tempBytes.size(); ++i) readScalars[i] = static_cast<float>(tempBytes[i]);
                    binaryReadOk = true;
                }
            }
            else {
                std::cerr << "VTK Parser Warning: unsupported SCALARS type '" << dataType
                          << "' for binary data; skipping field." << std::endl;
            }

            if (!binaryReadOk) {
                std::cerr << "VTK Parser Warning: short read on binary SCALARS '" << scalarName
                          << "'; skipping field to avoid stream desync." << std::endl;
                if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
                if (readingFieldData) mesh.attributes->fieldData[scalarName];
                else if (readingPointData) mesh.attributes->pointScalars[scalarName];
                else { mesh.attributes->cellScalars[scalarName]; cellScalarsStorage[scalarName]; }
                return;
            }
        }
        else {
            std::vector<double> tmpVals;
            size_t nRead = mesh_utils::readAsciiValues(file, readScalars.size(), tmpVals);
            if (nRead != readScalars.size()) {
                std::cerr << "VTK Parser Warning: short read on ASCII SCALARS '" << scalarName
                          << "'; dropping field." << std::endl;
                readScalars.clear();
                badField = true;
            } else {
                for (size_t i = 0; i < tmpVals.size(); ++i) {
                    double tmp = tmpVals[i];
                    if (!std::isfinite(tmp)) {
                        std::cerr << "VTK Parser Warning: non-finite ASCII SCALARS '" << scalarName
                                  << "'; dropping field." << std::endl;
                        readScalars.clear();
                        badField = true;
                        break;
                    }
                    if (tmp >  std::numeric_limits<float>::max())      readScalars[i] =  std::numeric_limits<float>::max();
                    else if (tmp < -std::numeric_limits<float>::max()) readScalars[i] = -std::numeric_limits<float>::max();
                    else                                               readScalars[i] = static_cast<float>(tmp);
                }
            }
        }

        if (!mesh.attributes.has_value()) {
            mesh.attributes = DatasetAttributes();
        }

        if (badField) {
            if (readingFieldData) mesh.attributes->fieldData[scalarName];
            else if (readingPointData) mesh.attributes->pointScalars[scalarName];
            else { mesh.attributes->cellScalars[scalarName]; cellScalarsStorage[scalarName]; }
            return;
        }

        if (readingFieldData) {
            mesh.attributes->fieldData[scalarName] = std::move(readScalars);
        }
        else if (readingPointData) {
            mesh.attributes->pointScalars[scalarName] = std::move(readScalars);
        }
        else {
            mesh.attributes->cellScalars[scalarName] = readScalars;
            cellScalarsStorage[scalarName] = std::move(readScalars);
        }
    }

    bool consumeBinaryScalars(std::ifstream& file, const std::string& dataType, size_t count) {
        if (count == 0) return true;
        if (dataType == "DOUBLE") {
            std::vector<double> tmp; return readBinaryArray(file, count, tmp);
        } else if (dataType == "FLOAT") {
            std::vector<float> tmp; return readBinaryArray(file, count, tmp);
        } else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG") {
            std::vector<int32_t> tmp; return readBinaryArray(file, count, tmp);
        } else if (dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
            std::vector<int64_t> tmp; return readBinaryArray(file, count, tmp);
        } else if (dataType == "SHORT" || dataType == "UNSIGNED_SHORT") {
            std::vector<int16_t> tmp; return readBinaryArray(file, count, tmp);
        } else if (dataType == "UNSIGNED_CHAR") {
            std::vector<uint8_t> tmp; return readBinaryArray(file, count, tmp);
        }
        // Unknown type: best-effort skip is impossible without a known size;
        // report failure so the caller can stop parsing this file.
        return false;
    }

    bool readBinaryFloatArray(std::ifstream& file, const std::string& dataType, size_t count, std::vector<float>& out) {
        out.clear();
        if (count == 0) return true;
        if (dataType == "DOUBLE") {
            std::vector<double> tmp(count);
            if (!readBinaryArray(file, tmp.size(), tmp)) return false;
            out.resize(tmp.size());
            for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<float>(tmp[i]);
            return true;
        } else if (dataType == "FLOAT") {
            out.resize(count);
            return readBinaryArray(file, out.size(), out);
        } else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG" ||
                   dataType == "SIGNED_CHAR" || dataType == "UNSIGNED_CHAR" ||
                   dataType == "SHORT" || dataType == "UNSIGNED_SHORT" ||
                   dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
            if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG") {
                std::vector<int32_t> tmp(count);
                if (!readBinaryArray(file, tmp.size(), tmp)) return false;
                out.resize(tmp.size());
                for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<float>(tmp[i]);
                return true;
            } else if (dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
                std::vector<int64_t> tmp(count);
                if (!readBinaryArray(file, tmp.size(), tmp)) return false;
                out.resize(tmp.size());
                for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<float>(tmp[i]);
                return true;
            } else if (dataType == "SHORT" || dataType == "UNSIGNED_SHORT") {
                std::vector<int16_t> tmp(count);
                if (!readBinaryArray(file, tmp.size(), tmp)) return false;
                out.resize(tmp.size());
                for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<float>(tmp[i]);
                return true;
            } else {
                std::vector<uint8_t> tmp(count);
                if (!readBinaryArray(file, tmp.size(), tmp)) return false;
                out.resize(tmp.size());
                for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<float>(tmp[i]);
                return true;
            }
        }
        return false;
    }

    void consumeLookupTable(std::ifstream& file, const std::string& dataType, size_t count) {
        if (count == 0) {
            std::string dummy; std::getline(file, dummy);
            return;
        }
        std::vector<float> colors;
        if (isBinary) {
            readBinaryFloatArray(file, dataType, count * 4, colors);
        } else {
            mesh_utils::readAsciiValues(file, count * 4, colors);
            for (float v : colors) {
                if (!std::isfinite(v)) { colors.clear(); break; }
            }
        }
    }

    void parseTensorsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string tensorName, dataType;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        tensorName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);

        int activeElementCount = readingFieldData ? fieldDataCount : (readingPointData ? numPoints : numCells);
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readTens(static_cast<size_t>(activeElementCount) * 9);

        if (isBinary) {
            if (!readBinaryFloatArray(file, dataType, readTens.size(), readTens)) {
                std::cerr << "VTK Parser Warning: short read on binary TENSORS '" << tensorName
                          << "'; skipping field to avoid stream desync." << std::endl;
                return;
            }
        } else {
            size_t nRead = mesh_utils::readAsciiValues(file, readTens.size(), readTens);
            if (nRead != readTens.size()) {
                std::cerr << "VTK Parser Warning: short read on ASCII TENSORS '" << tensorName << "'." << std::endl;
                readTens.clear();
            }
        }

        if (readTens.empty()) return;
        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
        if (readingFieldData) {
            mesh.attributes->fieldData[tensorName] = std::move(readTens);
            mesh.attributes->pointTensorComponents[tensorName] = 9;
        } else if (readingPointData) {
            mesh.attributes->pointTensors[tensorName] = std::move(readTens);
            mesh.attributes->pointTensorComponents[tensorName] = 9;
        } else {
            mesh.attributes->cellTensors[tensorName] = std::move(readTens);
            mesh.attributes->cellTensorComponents[tensorName] = 9;
        }
        std::cerr << "VTK: parsed " << (readingPointData ? "POINT" : (readingFieldData ? "FIELD" : "CELL"))
                  << " TENSORS '" << tensorName << "' (" << activeElementCount << " tensors)" << std::endl;
    }

    void parseTexCoordBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string tcName, dataType; int numCoords = 2;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        tcName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        if (p < lineEnd) {
            auto [np, ec] = std::from_chars(p, lineEnd, numCoords);
            if (ec != std::errc()) numCoords = 2;
        }
        if (numCoords < 2) numCoords = 2;
        if (numCoords > 3) numCoords = 3;

        int activeElementCount = readingFieldData ? fieldDataCount : (readingPointData ? numPoints : numCells);
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readTC(static_cast<size_t>(activeElementCount) * numCoords);

        if (isBinary) {
            if (!readBinaryFloatArray(file, dataType, readTC.size(), readTC)) {
                std::cerr << "VTK Parser Warning: short read on binary TEXTURE_COORDINATES '" << tcName
                          << "'; skipping field to avoid stream desync." << std::endl;
                return;
            }
        } else {
            size_t nRead = mesh_utils::readAsciiValues(file, readTC.size(), readTC);
            if (nRead != readTC.size()) {
                std::cerr << "VTK Parser Warning: short read on ASCII TEXTURE_COORDINATES '" << tcName << "'." << std::endl;
                readTC.clear();
            }
        }

        if (readTC.empty()) return;
        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
        if (readingFieldData) {
            mesh.attributes->fieldData[tcName] = std::move(readTC);
            mesh.attributes->pointTexCoordComponents[tcName] = numCoords;
        } else if (readingPointData) {
            mesh.attributes->pointTexCoords[tcName] = std::move(readTC);
            mesh.attributes->pointTexCoordComponents[tcName] = numCoords;
        } else {
            mesh.attributes->cellTexCoords[tcName] = std::move(readTC);
            mesh.attributes->cellTexCoordComponents[tcName] = numCoords;
        }
        std::cerr << "VTK: parsed " << (readingPointData ? "POINT" : (readingFieldData ? "FIELD" : "CELL"))
                  << " TEXTURE_COORDINATES '" << tcName << "' (" << activeElementCount << " coords, " << numCoords << " comps)" << std::endl;
    }

    void parseFieldBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string fieldName, dataType; int ntuples = 0, ncomponents = 1;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        fieldName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        if (p < lineEnd) {
            auto [n1, e1] = std::from_chars(p, lineEnd, ntuples); p = n1;
            if (e1 != std::errc()) ntuples = 0;
        } else ntuples = 0;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        if (p < lineEnd) {
            auto [n2, e2] = std::from_chars(p, lineEnd, ncomponents);
            if (e2 != std::errc()) ncomponents = 1;
        }
        if (ncomponents < 1) ncomponents = 1;

        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

        const size_t total = static_cast<size_t>(ntuples) * static_cast<size_t>(ncomponents);
        std::vector<float> fieldDataFlat;

        if (isBinary) {
            if (!readBinaryFloatArray(file, dataType, total, fieldDataFlat)) {
                std::cerr << "VTK Parser Warning: short read on binary FIELD '" << fieldName
                          << "'; skipping field to avoid stream desync." << std::endl;
                return;
            }
        } else {
            size_t nRead = mesh_utils::readAsciiValues(file, total, fieldDataFlat);
            if (nRead != total) {
                std::cerr << "VTK Parser Warning: short read on ASCII FIELD '" << fieldName << "'." << std::endl;
                fieldDataFlat.clear();
            }
        }

        if (!fieldDataFlat.empty()) {
            mesh.attributes->fieldData[fieldName] = std::move(fieldDataFlat);
            if (ncomponents > 1) {
                mesh.attributes->pointFieldComponents[fieldName] = ncomponents;
            }
        }
    }
    void parseNormalsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string normName, dataType;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        normName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);

        int activeElementCount = readingFieldData ? fieldDataCount : (readingPointData ? numPoints : numCells);
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readNorms(static_cast<size_t>(activeElementCount) * 3);

        if (isBinary) {
            std::vector<float> tmpNorms;
            if (!readBinaryFloatArray(file, dataType, readNorms.size(), tmpNorms) || tmpNorms.size() != readNorms.size()) {
                std::cerr << "VTK Parser Warning: short read or unsupported type on binary NORMALS '" << normName
                          << "' (" << dataType << "); skipping field to avoid stream desync." << std::endl;
                return;
            }
            readNorms = std::move(tmpNorms);
        } else {
            size_t nRead = mesh_utils::readAsciiValues(file, readNorms.size(), readNorms);
            if (nRead != readNorms.size()) {
                std::cerr << "VTK Parser Warning: non-finite ASCII NORMALS '" << normName
                          << "'; dropping field." << std::endl;
                readNorms.clear();
            }
        }

        if (readNorms.empty()) return;
        if (readingFieldData) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            mesh.attributes->fieldData[normName] = std::move(readNorms);
        } else {
            mesh.normals = std::move(readNorms);
        }
    }

    void parseVectorsBlock(const char* lineRest, const char* lineEnd, std::ifstream& file) {
        std::string vecName, dataType;
        const char* p = lineRest;
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* nStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        vecName = std::string(nStart, p - nStart);
        p = mesh_utils::skipWhitespace(p, lineEnd);
        const char* tStart = p;
        p = mesh_utils::skipToken(p, lineEnd);
        dataType = std::string(tStart, p - tStart);
        mesh_utils::toUpperInPlace(dataType);

        if (mesh.vectorName.empty()) mesh.vectorName = vecName;

        int activeElementCount = readingFieldData ? fieldDataCount : (readingPointData ? numPoints : numCells);
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readVecs(static_cast<size_t>(activeElementCount) * 3);

        if (isBinary) {
            std::vector<float> tmpVecs;
            if (!readBinaryFloatArray(file, dataType, readVecs.size(), tmpVecs) || tmpVecs.size() != readVecs.size()) {
                std::cerr << "VTK Parser Warning: short read or unsupported type on binary VECTORS '" << vecName
                          << "' (" << dataType << "); skipping field to avoid stream desync." << std::endl;
                return;
            }
            readVecs = std::move(tmpVecs);
            for (float v : readVecs) {
                if (!std::isfinite(v)) {
                    std::cerr << "VTK Parser Warning: non-finite binary VECTORS '" << vecName
                              << "'; dropping field." << std::endl;
                    readVecs.clear();
                    break;
                }
            }
        } else {
            size_t expected = readVecs.size();
            size_t nRead = mesh_utils::readAsciiValues(file, expected, readVecs);
            if (nRead != expected) {
                std::cerr << "VTK Parser Warning: short read on ASCII VECTORS '" << vecName
                          << "'; dropping field." << std::endl;
                readVecs.clear();
            } else {
                for (float v : readVecs) {
                    if (!std::isfinite(v)) {
                        std::cerr << "VTK Parser Warning: non-finite ASCII VECTORS '" << vecName
                                  << "'; dropping field." << std::endl;
                        readVecs.clear();
                        break;
                    }
                }
            }
        }

        if (readVecs.empty()) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            if (readingFieldData) mesh.attributes->fieldData[vecName];
            else if (readingPointData) mesh.attributes->pointVectors[vecName];
            else cellVectorsStorage[vecName];
            return;
        }
        if (readingFieldData) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            mesh.attributes->fieldData[vecName] = std::move(readVecs);
            std::cerr << "VTK: parsed FIELD VECTORS '" << vecName << "' (" << activeElementCount << " vectors)" << std::endl;
        }
        else if (readingPointData) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            mesh.attributes->pointVectors[vecName] = std::move(readVecs);
            std::cerr << "VTK: parsed POINT VECTORS '" << vecName << "' (" << activeElementCount << " vectors)" << std::endl;
        } else {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            cellVectorsStorage[vecName] = std::move(readVecs);
            std::cerr << "VTK: parsed CELL VECTORS '" << vecName << "' (" << activeElementCount << " vectors, extrapolated to points)" << std::endl;
        }
    }

    void clearTrailingLine(std::ifstream& file) {
        std::string dummy; std::getline(file, dummy);
    }

    // ── Grid Topologies Generators ──────────────────────────────────────────

    void buildTopology() {
        if (datasetType == "STRUCTURED_POINTS") {
            generateStructuredPointsGeometry();
            generateStructuredGridSurface(dimX, dimY, dimZ);
            globalCellToVertices = generateStructuredGridCells(dimX, dimY, dimZ);
            numCells = static_cast<int>(globalCellToVertices.size());
            mesh.renderAsPoints = true;
        }
        else if (datasetType == "RECTILINEAR_GRID" && !rectX.empty() && !rectY.empty() && !rectZ.empty()) {
            generateRectilinearGridGeometry();
            generateStructuredGridSurface(dimX, dimY, dimZ);
            globalCellToVertices = generateStructuredGridCells(dimX, dimY, dimZ);
            numCells = static_cast<int>(globalCellToVertices.size());
        }
        else if (datasetType == "UNSTRUCTURED_GRID" && !rawCellData.empty()) {
            globalCellToVertices = triangulateUnstructuredCells(rawCellData, cellTypes, numCells);
        }
        else if (datasetType == "STRUCTURED_GRID") {
            if (!mesh.vertices.empty()) {
                generateStructuredGridSurface(dimX, dimY, dimZ);
                globalCellToVertices = generateStructuredGridCells(dimX, dimY, dimZ);
                numCells = static_cast<int>(globalCellToVertices.size());
            }
        }
        else if (datasetType == "POLYDATA") {
            if (mesh.indices.empty() && !mesh.vertices.empty()) {
                std::cerr << "VTK Parser Warning: POLYDATA has points but no VERTICES/LINES/POLYGONS/TRIANGLE_STRIPS; rendering points only." << std::endl;
                mesh.renderAsPoints = true; // ponytail: mark as point cloud
            }
        }

        // Extract cell-boundary edges for cell-edge wireframe mode. Must run
        // before computeNormals() (which only appends vertices, so the
        // pre-split vertex indices remain valid in the final mesh).
        if (!globalCellToVertices.empty()) {
            mesh.cellEdgeIndices = mesh_utils::extractCellEdges(globalCellToVertices, cellTypes);
        } else if (!mesh.indices.empty() && mesh.indices.size() % 3 == 0) {
            mesh.cellEdgeIndices = mesh_utils::extractTriEdges(mesh.indices);
        }
    }

    void generateStructuredPointsGeometry() {
        mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
        int vIdx = 0;
        for (int z = 0; z < dimZ; ++z) {
            for (int y = 0; y < dimY; ++y) {
                for (int x = 0; x < dimX; ++x) {
                    mesh.vertices[vIdx++] = origin[0] + x * spacing[0];
                    mesh.vertices[vIdx++] = origin[1] + y * spacing[1];
                    mesh.vertices[vIdx++] = origin[2] + z * spacing[2];
                }
            }
        }
    }

    void generateRectilinearGridGeometry() {
        mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);
        int vIdx = 0;
        for (int z = 0; z < dimZ; ++z) {
            for (int y = 0; y < dimY; ++y) {
                for (int x = 0; x < dimX; ++x) {
                    mesh.vertices[vIdx++] = rectX[x];
                    mesh.vertices[vIdx++] = rectY[y];
                    mesh.vertices[vIdx++] = rectZ[z];
                }
            }
        }
    }

    std::vector<std::vector<uint32_t>> generateStructuredGridIndices(int dX, int dY, int dZ) {
        int cellsX = std::max(1, dX - 1);
        int cellsY = std::max(1, dY - 1);
        int cellsZ = std::max(1, dZ - 1);
        // 64-bit count — int overflows at ~1290^3 cells
        size_t totalCells = static_cast<size_t>(cellsX) * cellsY * cellsZ;

        mesh.indices.reserve(totalCells * 36);
        std::vector<std::vector<uint32_t>> cellToVertices(totalCells);

        int cellIdx = 0;
        for (int z = 0; z < dZ - 1; ++z) {
            for (int y = 0; y < dY - 1; ++y) {
                for (int x = 0; x < dX - 1; ++x) {
                    uint32_t i0 = x + y * dX + z * dX * dY;
                    uint32_t i1 = (x + 1) + y * dX + z * dX * dY;
                    uint32_t i2 = (x + 1) + (y + 1) * dX + z * dX * dY;
                    uint32_t i3 = x + (y + 1) * dX + z * dX * dY;

                    uint32_t i4 = x + y * dX + (z + 1) * dX * dY;
                    uint32_t i5 = (x + 1) + y * dX + (z + 1) * dX * dY;
                    uint32_t i6 = (x + 1) + (y + 1) * dX + (z + 1) * dX * dY;
                    uint32_t i7 = x + (y + 1) * dX + (z + 1) * dX * dY;

                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1, i0, i3, i2,        // bottom  z=0  (-Z)
                        i4, i5, i6, i4, i6, i7,        // top     z=1  (+Z)
                        i0, i4, i7, i0, i7, i3,        // x=0     (-X)
                        i1, i2, i6, i1, i6, i5,        // x=1     (+X)
                        i0, i1, i5, i0, i5, i4,        // y=0     (-Y)
                        i3, i7, i6, i3, i6, i2         // y=1     (+Y)
                    });

                    cellToVertices[cellIdx] = { i0, i1, i2, i3, i4, i5, i6, i7 };
                    cellIdx++;
                }
            }
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> generateStructuredGridSurface(int dX, int dY, int dZ) {
        std::vector<std::vector<uint32_t>> cellToVertices;
        auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
        auto addQuad = [&](int a, int b, int c, int d) {
            mesh.indices.push_back(static_cast<uint32_t>(a)); mesh.indices.push_back(static_cast<uint32_t>(b)); mesh.indices.push_back(static_cast<uint32_t>(c));
            mesh.indices.push_back(static_cast<uint32_t>(a)); mesh.indices.push_back(static_cast<uint32_t>(c)); mesh.indices.push_back(static_cast<uint32_t>(d));
            cellToVertices.push_back({ static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c), static_cast<uint32_t>(d) });
        };

        const int cx = std::max(1, dX - 1);
        const int cy = std::max(1, dY - 1);
        const int cz = std::max(1, dZ - 1);

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
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> generateStructuredGridCells(int dX, int dY, int dZ) {
        std::vector<std::vector<uint32_t>> cellToVertices;
        auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
        const int cx = std::max(1, dX - 1);
        const int cy = std::max(1, dY - 1);
        const int cz = std::max(1, dZ - 1);
        cellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);

        if (dX > 1 && dY > 1 && dZ > 1) {
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
                        cellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                    }
        } else if (dZ == 1) {
            for (int y = 0; y + 1 < dY; ++y)
                for (int x = 0; x + 1 < dX; ++x)
                    cellToVertices.push_back({
                        static_cast<uint32_t>(idx(x, y, 0)),
                        static_cast<uint32_t>(idx(x + 1, y, 0)),
                        static_cast<uint32_t>(idx(x + 1, y + 1, 0)),
                        static_cast<uint32_t>(idx(x, y + 1, 0)) });
        } else if (dY == 1) {
            for (int z = 0; z + 1 < dZ; ++z)
                for (int x = 0; x + 1 < dX; ++x)
                    cellToVertices.push_back({
                        static_cast<uint32_t>(idx(x, 0, z)),
                        static_cast<uint32_t>(idx(x + 1, 0, z)),
                        static_cast<uint32_t>(idx(x + 1, 0, z + 1)),
                        static_cast<uint32_t>(idx(x, 0, z + 1)) });
        } else if (dX == 1) {
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    cellToVertices.push_back({
                        static_cast<uint32_t>(idx(0, y, z)),
                        static_cast<uint32_t>(idx(0, y + 1, z)),
                        static_cast<uint32_t>(idx(0, y + 1, z + 1)),
                        static_cast<uint32_t>(idx(0, y, z + 1)) });
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulatePolygons(const std::vector<int32_t>& rawPolygonData, int numPolys) {
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

    std::vector<std::vector<uint32_t>> triangulateTriangleStrips(const std::vector<int32_t>& rawStripData, int numStrips) {
        // ponytail: a triangle strip is a BAND of triangles, not one polygon.
        // Store each triangle as its own 3-vertex cell so the cyclic cell-edge
        // emitter draws true per-triangle boundaries (3 edges, no diagonal,
        // no spurious wrap edge). The old code kept the whole strip as a single
        // "cell", which the emitter wrapped with a long v_{N-1}->v0 edge
        // across the entire strip — garbage on strip meshes (e.g. lung.vtk).
        std::vector<std::vector<uint32_t>> cellToVertices;
        cellToVertices.reserve(numStrips > 0 ? numStrips * 3 : 0);
        int idx = 0;
        for (int s = 0; s < numStrips; ++s) {
            if (idx >= static_cast<int>(rawStripData.size())) break;
            int nPoints = rawStripData[idx++];
            if (nPoints < 0 || idx + nPoints > static_cast<int>(rawStripData.size())) break;

            for (int i = 0; i < nPoints - 2; ++i) {
                uint32_t i0 = rawStripData[idx + i];
                uint32_t i1 = rawStripData[idx + i + 1];
                uint32_t i2 = rawStripData[idx + i + 2];
                // triangle as a cell (winding only affects the cyclic edge
                // order; all 3 edges are emitted either way)
                cellToVertices.push_back({ i0, i1, i2 });
                if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
                else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulateLines(const std::vector<int32_t>& rawLineData, int numLines) {
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
                uint32_t a = rawLineData[idx + i];
                uint32_t b = rawLineData[idx + i + 1];
                mesh.indices.insert(mesh.indices.end(), { a, b });
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(const std::vector<int32_t>& rawCellData, const std::vector<int32_t>& cellTypes, int totalCells) {
        mesh.indices.clear();
        std::vector<std::vector<uint32_t>> cellToVertices(totalCells);
        int idx = 0;
        for (int c = 0; c < totalCells; ++c) {
            if (idx >= static_cast<int>(rawCellData.size())) break;
            int numPointsInCell = rawCellData[idx++];
            // Guard against a malformed/truncated cell claiming more points than
            // the buffer holds — reading past the end is UB / a crash.
            if (numPointsInCell < 0 || idx + numPointsInCell > static_cast<int>(rawCellData.size())) break;

            for (int i = 0; i < numPointsInCell; ++i) {
                cellToVertices[c].push_back(static_cast<uint32_t>(rawCellData[idx + i]));
            }

            int type = (c < static_cast<int>(cellTypes.size())) ? cellTypes[c] : 0;
            if (type == 0) {
                if (numPointsInCell == 3) type = 5;   // VTK_TRIANGLE
                if (numPointsInCell == 4) type = 10;  // VTK_QUAD
                if (numPointsInCell == 8) type = 12;  // VTK_HEXAHEDRON
            }

            switch (type) {
            case 1:  // VTK_VERTEX
            case 2:  // VTK_POLY_VERTEX
            case 3:  // VTK_LINE
            case 4:  // VTK_POLY_LINE
                break;
            case 5: // VTK_TRIANGLE
                if (idx + 2 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 1]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 2]));
                }
                break;
            case 6: { // VTK_TRIANGLE_STRIP
                for (int i = 0; i + 2 < numPointsInCell; ++i) {
                    if (idx + i + 2 < static_cast<int>(rawCellData.size())) {
                        if (i & 1) {
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                        } else {
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                            mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                        }
                    }
                }
                break;
            }
            case 7: // VTK_POLYGON (triangle fan)
                for (int i = 1; i < numPointsInCell - 1; ++i) {
                    if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                    }
                }
                break;
            case 8: { // VTK_PIXEL (4 pts: BL, BR, TL, TR)
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1];
                    uint32_t i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i2, i1, i0, i1, i3 });
                }
                break;
            }
            case 9: { // VTK_VOXEL — structured-grid corner ordering, permute to HEX (0,1,3,2,4,5,7,6)
                if (idx + 7 < static_cast<int>(rawCellData.size())) {
                    uint32_t h0 = rawCellData[idx + 0], h1 = rawCellData[idx + 1], h2 = rawCellData[idx + 3], h3 = rawCellData[idx + 2];
                    uint32_t h4 = rawCellData[idx + 4], h5 = rawCellData[idx + 5], h6 = rawCellData[idx + 7], h7 = rawCellData[idx + 6];
                    cellToVertices[c] = {h0, h1, h2, h3, h4, h5, h6, h7};
                    mesh.indices.insert(mesh.indices.end(), {
                        h0, h3, h1, h1, h3, h2, h4, h5, h7, h5, h6, h7,
                        h0, h1, h4, h1, h5, h4, h2, h3, h6, h3, h7, h6,
                        h0, h4, h3, h3, h4, h7, h1, h2, h5, h2, h6, h5
                    });
                }
                break;
            }
            case 10: // VTK_QUAD
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
                }
                break;
            case 11: { // VTK_TETRA
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1,   i0, i1, i3,
                        i1, i2, i3,   i2, i0, i3
                    });
                }
                break;
            }
            case 12: // VTK_HEXAHEDRON
                if (idx + 7 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    uint32_t i4 = rawCellData[idx + 4], i5 = rawCellData[idx + 5], i6 = rawCellData[idx + 6], i7 = rawCellData[idx + 7];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i3, i1, i1, i3, i2, i4, i5, i7, i5, i6, i7,
                        i0, i1, i4, i1, i5, i4, i2, i3, i6, i3, i7, i6,
                        i0, i4, i3, i3, i4, i7, i1, i2, i5, i2, i6, i5
                    });
                }
                break;
            case 13: { // VTK_PYRAMID
                if (idx + 4 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1, i0, i3, i2,
                        i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                    });
                }
                break;
            }
            case 14: { // VTK_WEDGE
                if (idx + 5 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2];
                    uint32_t i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4], i5 = rawCellData[idx + 5];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1,
                        i3, i4, i5,
                        i0, i1, i4, i0, i4, i3,
                        i1, i2, i5, i1, i5, i4,
                        i2, i0, i3, i2, i3, i5
                    });
                }
                break;
            }
            case 15: { // VTK_PENTAGONAL_PRISM (10 vertices)
                if (idx + 9 < static_cast<int>(rawCellData.size())) {
                    uint32_t v0 = rawCellData[idx + 0], v1 = rawCellData[idx + 1], v2 = rawCellData[idx + 2];
                    uint32_t v3 = rawCellData[idx + 3], v4 = rawCellData[idx + 4];
                    uint32_t v5 = rawCellData[idx + 5], v6 = rawCellData[idx + 6], v7 = rawCellData[idx + 7];
                    uint32_t v8 = rawCellData[idx + 8], v9 = rawCellData[idx + 9];
                    cellToVertices[c] = {v0, v1, v2, v3, v4, v5, v6, v7, v8, v9};
                    mesh.indices.insert(mesh.indices.end(), {
                        v5, v7, v6, v5, v8, v7, v5, v9, v8,
                        v4, v2, v3, v4, v3, v1, v4, v1, v0,
                        v0, v1, v6, v0, v6, v5,
                        v1, v2, v7, v1, v7, v6,
                        v2, v3, v8, v2, v8, v7,
                        v3, v4, v9, v3, v9, v8,
                        v0, v5, v9, v0, v9, v4
                    });
                }
                break;
            }
            default:
                if (type >= 21) {
                    std::cerr << "VTK Parser Warning: unsupported polyhedron/higher-order cell type "
                              << type << " (requires subdivision); fan-triangulating as fallback."
                              << std::endl;
                }
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

    // ── Post Processing & Metrics Finalization ──────────────────────────────

    void finalizeMeshData() {
        if (mesh.vertices.empty()) {
            std::cerr << "VTK Parser Error: Empty data sequence." << std::endl;
            return;
        }

        if (mesh.indices.empty() && datasetType != "POLYDATA" && !mesh.renderAsPoints) {
            std::cerr << "VTK Parser Error: topology produced no triangles and not a point set; mesh will not render." << std::endl;
            return;
        }

        {
            const uint32_t vCount = static_cast<uint32_t>(mesh.vertices.size() / 3);
            bool badIndex = false;
            for (uint32_t idx : mesh.indices) {
                if (idx >= vCount) { badIndex = true; break; }
            }
            if (badIndex) {
                std::cerr << "VTK Parser Error: topology references vertex index >= vertex count ("
                          << vCount << "); dropping indices to avoid out-of-range GPU fetch." << std::endl;
                mesh.indices.clear();
                return;
            }
        }

        mesh_utils::extrapolateCellDataToPoints(mesh, globalCellToVertices, cellScalarsStorage, cellVectorsStorage);

        if (mesh.attributes.has_value()) {
            // Flatten per-point vectors into one contiguous vec3 buffer with a
            // per-field offset (shared_ptr/zero-copy glyph pipeline expects this).
            const size_t perVertex = mesh.vertices.size() / 3;
            mesh.pointVectorCount = perVertex; // per-POINT count, set BEFORE computeNormals splits
            for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
                if (vecArr.size() < perVertex * 3) continue; // skip unusable field
                mesh.pointVectorOffset[name] = mesh.pointVectorsData.size();
                for (size_t v = 0; v < perVertex; ++v) {
                    mesh.pointVectorsData.emplace_back(
                        vecArr[v * 3 + 0], vecArr[v * 3 + 1], vecArr[v * 3 + 2]);
                }
            }
        }

        {
            const size_t cellCount = globalCellToVertices.size();
            mesh.cellVectorCount = cellCount;
            for (const auto& [name, raw] : cellVectorsStorage) {
                if (cellCount == 0 || raw.size() < cellCount * 3) continue;
                mesh.cellVectorOffset[name] = mesh.cellVectorsData.size();
                for (size_t c = 0; c < cellCount; ++c) {
                    mesh.cellVectorsData.emplace_back(
                        raw[c * 3 + 0], raw[c * 3 + 1], raw[c * 3 + 2]);
                }
                mesh.availableCellVectorNames.push_back(name);
            }
            if (mesh.cellVectorName.empty() && !mesh.availableCellVectorNames.empty()) {
                mesh.cellVectorName = mesh.availableCellVectorNames.front();
            }
            if (cellCount != 0 && !mesh.vertices.empty()) {
                mesh.cellCenters.reserve(cellCount);
                for (size_t c = 0; c < cellCount; ++c) {
                    const auto& corners = globalCellToVertices[c];
                    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                    for (uint32_t vi : corners) {
                        const size_t base = static_cast<size_t>(vi) * 3;
                        cx += mesh.vertices[base + 0];
                        cy += mesh.vertices[base + 1];
                        cz += mesh.vertices[base + 2];
                    }
                    const float n = static_cast<float>(corners.size());
                    mesh.cellCenters.emplace_back(cx / n, cy / n, cz / n);
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
                std::cerr << "VTK Parser Warning: active scalar '" << mesh.scalarName
                          << "' is not 1-component per vertex; scalar coloring disabled." << std::endl;
                mesh.scalars.clear();
            }
        } else {
            mesh.scalars.clear();
        }

        // expose every point-scalar field name so the UI can switch fields
        if (hasAttributes) {
            for (const auto& [name, _] : mesh.attributes->pointScalars) {
                mesh.availableScalarNames.push_back(name);
            }
            // expose vector field names for the UI switcher
            for (const auto& [name, _] : mesh.attributes->pointVectors) {
                mesh.availableVectorNames.push_back(name);
            }
            std::sort(mesh.availableScalarNames.begin(), mesh.availableScalarNames.end());
            std::sort(mesh.availableVectorNames.begin(), mesh.availableVectorNames.end());
        }

        if (mesh.vectorName.empty() && !mesh.availableVectorNames.empty()) {
            mesh.vectorName = mesh.availableVectorNames.front();
        }

        calculateScalarRanges();
        mesh_utils::computeBounds(mesh);

        mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

        // flatVerts is now lazily computed via ensureFlatVerts() — only built
        // when mesh_quality analysis actually runs, saving 3x index-count memory.

        if (mesh.normals.empty() && !mesh.indices.empty()) {
            mesh_utils::computeNormals(mesh);
        }
    }

    // extrapolateCellDataToPointsMerged is now in mesh_utils (shared implementation).

    void calculateScalarRanges() {
        // Ensure attributes are allocated
        if (!mesh.attributes.has_value()) {
            mesh.attributes = DatasetAttributes();
        }

        if (!mesh.scalarName.empty() && mesh.attributes->pointScalars.count(mesh.scalarName)) {
            const auto& activeVec = mesh.attributes->pointScalars[mesh.scalarName];
            if (activeVec.empty()) return;

            float minVal = std::numeric_limits<float>::max();
            float maxVal = -std::numeric_limits<float>::max();
            for (float val : activeVec) {
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }

            mesh.attributes->scalarMin = minVal;
            mesh.attributes->scalarMax = maxVal;

            if (std::abs(mesh.attributes->scalarMax - mesh.attributes->scalarMin) < 1e-6f) {
                mesh.attributes->scalarMax = mesh.attributes->scalarMin + 1.0f;
            }
        }
    }
};

RenderMesh parseVTK(const std::string& filePath) {
    VTKParserContext parser(filePath);
    return parser.parse();
}