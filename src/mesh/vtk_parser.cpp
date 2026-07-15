#include "mesh/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <sstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <map>

// ── Endianness Utility Wrapper ──────────────────────────────────────────────

template<typename T>
static void fixEndianness(T* data, size_t count) {
    if (mesh_utils::isLittleEndian()) {
        for (size_t i = 0; i < count; ++i) {
            mesh_utils::byteSwap(&data[i]);
        }
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
        while (getVTKLine(file, line)) {
            line = mesh_utils::trim(line);
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string token;
            iss >> token;
            processToken(mesh_utils::toUpper(token), iss, file);
        }
        file.close();

        buildTopology();
        finalizeMeshData();

        return mesh;
    }

private:
    std::string filePath;
    RenderMesh mesh;
    std::string datasetType = "";
    bool isBinary = false;

    int dimX = 0, dimY = 0, dimZ = 0;
    int numPoints = 0, numCells = 0, cellSize = 0;
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float spacing[3] = { 1.0f, 1.0f, 1.0f };

    bool readingPointData = true;
    int attributeTargetCount = 0;

    std::vector<float> rectX, rectY, rectZ;
    std::vector<int> rawCellData, cellTypes;

    // Intermediate storage cache specifically tracking split configurations
    std::map<std::string, std::vector<float>> cellScalarsStorage;
    std::vector<std::vector<int>> globalCellToVertices;

    bool getVTKLine(std::ifstream& f, std::string& outLine) {
        outLine.clear();
        char ch;
        while (f.get(ch)) {
            if (ch == '\n') return true;
            if (ch != '\r') outLine += ch;
        }
        return !outLine.empty();
    }

    void processToken(const std::string& token, std::istringstream& iss, std::ifstream& file) {
        if (token == "BINARY") { isBinary = true; }
        else if (token == "ASCII") { isBinary = false; }
        else if (token == "DATASET") { iss >> datasetType; datasetType = mesh_utils::toUpper(datasetType); }
        else if (token == "DIMENSIONS") { handleDimensions(iss); }
        else if (token == "ORIGIN") { iss >> origin[0] >> origin[1] >> origin[2]; }
        else if (token == "SPACING" || token == "ASPECT_RATIO") { iss >> spacing[0] >> spacing[1] >> spacing[2]; }
        else if (token == "X_COORDINATES") { parseRectilinearAxis(file, rectX, iss); }
        else if (token == "Y_COORDINATES") { parseRectilinearAxis(file, rectY, iss); }
        else if (token == "Z_COORDINATES") { parseRectilinearAxis(file, rectZ, iss); }
        else if (token == "POINT_DATA") { iss >> attributeTargetCount; readingPointData = true; }
        else if (token == "CELL_DATA") { iss >> attributeTargetCount; readingPointData = false; }
        else if (token == "POINTS") { parsePointsBlock(iss, file); }
        else if (token == "CELLS") { parseCellsBlock(iss, file); }
        else if (token == "CELL_TYPES") { parseCellTypesBlock(iss, file); }
        else if (token == "POLYGONS") { parsePolygonsBlock(iss, file); }
        else if (token == "TRIANGLE_STRIPS") { parseTriangleStripsBlock(iss, file); }
        else if (token == "SCALARS") { parseScalarsBlock(iss, file); }
        else if (token == "VECTORS") { parseVectorsBlock(iss, file); }
    }

    void handleDimensions(std::istringstream& iss) {
        iss >> dimX >> dimY >> dimZ;
        numPoints = dimX * dimY * dimZ;
        if (datasetType == "STRUCTURED_POINTS" || datasetType == "STRUCTURED_GRID" || datasetType == "RECTILINEAR_GRID") {
            numCells = std::max(1, dimX - 1) * std::max(1, dimY - 1) * std::max(1, dimZ - 1);
        }
    }

    void parseRectilinearAxis(std::ifstream& file, std::vector<float>& axisCoords, std::istringstream& iss) {
        int count; std::string type; iss >> count >> type; type = mesh_utils::toUpper(type);
        axisCoords.resize(count);
        if (isBinary) {
            if (type == "DOUBLE") {
                std::vector<double> tmp(count);
                file.read(reinterpret_cast<char*>(tmp.data()), count * sizeof(double));
                if (mesh_utils::isLittleEndian()) {
                    for (auto& v : tmp) std::reverse(reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + sizeof(double));
                }
                for (int i = 0; i < count; ++i) axisCoords[i] = static_cast<float>(tmp[i]);
            } else {
                file.read(reinterpret_cast<char*>(axisCoords.data()), count * sizeof(float));
                fixEndianness(axisCoords.data(), count);
            }
        }
        else {
            for (int i = 0; i < count; ++i) file >> axisCoords[i];
        }
    }

    void parsePointsBlock(std::istringstream& iss, std::ifstream& file) {
        iss >> numPoints;
        std::string dataType; iss >> dataType; dataType = mesh_utils::toUpper(dataType);
        mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tempDouble(numPoints * 3);
                file.read(reinterpret_cast<char*>(tempDouble.data()), tempDouble.size() * sizeof(double));
                if (mesh_utils::isLittleEndian()) {
                    for (auto& val : tempDouble) {
                        std::reverse(reinterpret_cast<char*>(&val), reinterpret_cast<char*>(&val) + sizeof(double));
                    }
                }
                for (size_t i = 0; i < tempDouble.size(); ++i) mesh.vertices[i] = static_cast<float>(tempDouble[i]);
            }
            else {
                file.read(reinterpret_cast<char*>(mesh.vertices.data()), mesh.vertices.size() * sizeof(float));
                fixEndianness(mesh.vertices.data(), mesh.vertices.size());
            }
        }
        else {
            for (int i = 0; i < numPoints * 3; ++i) file >> mesh.vertices[i];
            clearTrailingLine(file);
        }
    }

    void parseCellsBlock(std::istringstream& iss, std::ifstream& file) {
        iss >> numCells >> cellSize;
        rawCellData.resize(cellSize);
        if (isBinary) {
            file.read(reinterpret_cast<char*>(rawCellData.data()), cellSize * sizeof(int32_t));
            fixEndianness(rawCellData.data(), cellSize);
        }
        else {
            for (int i = 0; i < cellSize; ++i) file >> rawCellData[i];
            clearTrailingLine(file);
        }
    }

    void parseCellTypesBlock(std::istringstream& iss, std::ifstream& file) {
        iss >> numCells; cellTypes.resize(numCells);
        if (isBinary) {
            std::vector<int32_t> rawTypes(numCells);
            file.read(reinterpret_cast<char*>(rawTypes.data()), numCells * sizeof(int32_t));
            for (int i = 0; i < numCells; ++i) {
                if (mesh_utils::isLittleEndian()) mesh_utils::byteSwap(&rawTypes[i]);
                cellTypes[i] = rawTypes[i];
            }
        }
        else {
            for (int i = 0; i < numCells; ++i) file >> cellTypes[i];
            clearTrailingLine(file);
        }
    }

    void parsePolygonsBlock(std::istringstream& iss, std::ifstream& file) {
        int numPolys = 0, sizeOfPolysBlock = 0; iss >> numPolys >> sizeOfPolysBlock;
        std::vector<int> polyData(sizeOfPolysBlock);
        if (isBinary) {
            file.read(reinterpret_cast<char*>(polyData.data()), sizeOfPolysBlock * sizeof(int32_t));
            fixEndianness(polyData.data(), sizeOfPolysBlock);
        }
        else {
            for (int i = 0; i < sizeOfPolysBlock; ++i) file >> polyData[i];
            clearTrailingLine(file);
        }
        globalCellToVertices = triangulatePolygons(polyData, numPolys);
        numCells = numPolys;
    }

    void parseTriangleStripsBlock(std::istringstream& iss, std::ifstream& file) {
        int numStrips = 0, sizeOfStripsBlock = 0; iss >> numStrips >> sizeOfStripsBlock;
        std::vector<int> stripData(sizeOfStripsBlock);
        if (isBinary) {
            file.read(reinterpret_cast<char*>(stripData.data()), sizeOfStripsBlock * sizeof(int32_t));
            fixEndianness(stripData.data(), sizeOfStripsBlock);
        }
        else {
            for (int i = 0; i < sizeOfStripsBlock; ++i) file >> stripData[i];
            clearTrailingLine(file);
        }
        globalCellToVertices = triangulateTriangleStrips(stripData, numStrips);
        numCells = numStrips;
    }

    void parseScalarsBlock(std::istringstream& iss, std::ifstream& file) {
        std::string scalarName, dataType; int numComponents = 1;
        iss >> scalarName >> dataType;
        if (iss >> numComponents) {}
        dataType = mesh_utils::toUpper(dataType);

        // FIX: Map to `scalarName` instead of `activeScalarName`
        if (mesh.scalarName.empty()) {
            mesh.scalarName = scalarName;
        }

        std::string lutLine; getVTKLine(file, lutLine);

        int activeElementCount = readingPointData ? numPoints : numCells;
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readScalars(activeElementCount * numComponents);

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tempDouble(readScalars.size());
                file.read(reinterpret_cast<char*>(tempDouble.data()), tempDouble.size() * sizeof(double));
                if (mesh_utils::isLittleEndian()) {
                    for (auto& val : tempDouble) {
                        std::reverse(reinterpret_cast<char*>(&val), reinterpret_cast<char*>(&val) + sizeof(double));
                    }
                }
                for (size_t i = 0; i < tempDouble.size(); ++i) readScalars[i] = static_cast<float>(tempDouble[i]);
            }
            else if (dataType == "FLOAT") {
                file.read(reinterpret_cast<char*>(readScalars.data()), readScalars.size() * sizeof(float));
                fixEndianness(readScalars.data(), readScalars.size());
            }
            else if (dataType == "UNSIGNED_CHAR") {
                std::vector<uint8_t> tempBytes(readScalars.size());
                file.read(reinterpret_cast<char*>(tempBytes.data()), tempBytes.size());
                for (size_t i = 0; i < tempBytes.size(); ++i) readScalars[i] = static_cast<float>(tempBytes[i]);
            }
        }
        else {
            for (size_t i = 0; i < readScalars.size(); ++i) file >> readScalars[i];
            clearTrailingLine(file);
        }

        // FIX: Ensure the optional `attributes` struct is instantiated before insertion
        if (!mesh.attributes.has_value()) {
            mesh.attributes = DatasetAttributes();
        }

        if (readingPointData) {
            mesh.attributes->pointScalars[scalarName] = std::move(readScalars);
        }
        else {
            mesh.attributes->cellScalars[scalarName] = readScalars;
            cellScalarsStorage[scalarName] = std::move(readScalars);
        }
    }

    void parseVectorsBlock(std::istringstream& iss, std::ifstream& file) {
        std::string vecName, dataType;
        iss >> vecName >> dataType;
        dataType = mesh_utils::toUpper(dataType);

        if (mesh.vectorName.empty()) mesh.vectorName = vecName;

        int activeElementCount = readingPointData ? numPoints : numCells;
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readVecs(static_cast<size_t>(activeElementCount) * 3);

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tmp(readVecs.size());
                file.read(reinterpret_cast<char*>(tmp.data()), tmp.size() * sizeof(double));
                if (mesh_utils::isLittleEndian()) {
                    for (auto& v : tmp) std::reverse(reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + sizeof(double));
                }
                for (size_t i = 0; i < tmp.size(); ++i) readVecs[i] = static_cast<float>(tmp[i]);
            } else { // FLOAT
                file.read(reinterpret_cast<char*>(readVecs.data()), readVecs.size() * sizeof(float));
                fixEndianness(readVecs.data(), readVecs.size());
            }
        } else {
            for (size_t i = 0; i < readVecs.size(); ++i) file >> readVecs[i];
            clearTrailingLine(file);
        }

        // POINT_DATA VECTORS only; cell vectors are rare, skip+warn
        if (readingPointData) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            mesh.attributes->pointVectors[vecName] = std::move(readVecs);
            std::cerr << "VTK: parsed POINT VECTORS '" << vecName << "' (" << activeElementCount << " vectors)" << std::endl;
        } else {
            std::cerr << "VTK Parser Warning: CELL_DATA VECTORS not supported yet; skipping '" << vecName << "'" << std::endl;
        }
    }

    void clearTrailingLine(std::ifstream& file) {
        std::string dummy; std::getline(file, dummy);
    }

    // ── Grid Topologies Generators ──────────────────────────────────────────

    void buildTopology() {
        if (datasetType == "STRUCTURED_POINTS") {
            generateStructuredPointsGeometry();
            globalCellToVertices = generateStructuredGridIndices(dimX, dimY, dimZ);
        }
        else if (datasetType == "RECTILINEAR_GRID" && !rectX.empty() && !rectY.empty() && !rectZ.empty()) {
            generateRectilinearGridGeometry();
            globalCellToVertices = generateStructuredGridIndices(dimX, dimY, dimZ);
        }
        else if (datasetType == "UNSTRUCTURED_GRID" && !rawCellData.empty()) {
            globalCellToVertices = triangulateUnstructuredCells(rawCellData, cellTypes, numCells);
        }
        else if (datasetType == "STRUCTURED_GRID") {
            globalCellToVertices = generateStructuredGridIndices(dimX, dimY, dimZ);
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

    std::vector<std::vector<int>> generateStructuredGridIndices(int dX, int dY, int dZ) {
        int cellsX = std::max(1, dX - 1);
        int cellsY = std::max(1, dY - 1);
        int cellsZ = std::max(1, dZ - 1);
        // 64-bit count — int overflows at ~1290^3 cells
        size_t totalCells = static_cast<size_t>(cellsX) * cellsY * cellsZ;

        mesh.indices.reserve(totalCells * 36);
        std::vector<std::vector<int>> cellToVertices(totalCells);

        int cellIdx = 0;
        for (int z = 0; z < dZ - 1; ++z) {
            for (int y = 0; y < dY - 1; ++y) {
                for (int x = 0; x < dX - 1; ++x) {
                    int i0 = x + y * dX + z * dX * dY;
                    int i1 = (x + 1) + y * dX + z * dX * dY;
                    int i2 = (x + 1) + (y + 1) * dX + z * dX * dY;
                    int i3 = x + (y + 1) * dX + z * dX * dY;

                    int i4 = x + y * dX + (z + 1) * dX * dY;
                    int i5 = (x + 1) + y * dX + (z + 1) * dX * dY;
                    int i6 = (x + 1) + (y + 1) * dX + (z + 1) * dX * dY;
                    int i7 = x + (y + 1) * dX + (z + 1) * dX * dY;

                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1, i0, i3, i2, i4, i5, i6, i4, i6, i7,
                        i0, i1, i5, i0, i5, i4, i2, i3, i7, i2, i7, i6,
                        i0, i4, i7, i0, i7, i3, i1, i2, i6, i1, i6, i5
                        });

                    cellToVertices[cellIdx] = { i0, i1, i2, i3, i4, i5, i6, i7 };
                    cellIdx++;
                }
            }
        }
        return cellToVertices;
    }

    std::vector<std::vector<int>> triangulatePolygons(const std::vector<int>& rawPolygonData, int numPolys) {
        int idx = 0;
        std::vector<std::vector<int>> cellToVertices(numPolys);
        for (int p = 0; p < numPolys; ++p) {
            if (idx >= static_cast<int>(rawPolygonData.size())) break;
            int nPoints = rawPolygonData[idx++];

            for (int i = 0; i < nPoints; ++i) {
                cellToVertices[p].push_back(rawPolygonData[idx + i]);
            }

            for (int i = 1; i < nPoints - 1; ++i) {
                if (idx + i + 1 <= static_cast<int>(rawPolygonData.size())) {
                    mesh.indices.push_back(rawPolygonData[idx + 0]);
                    mesh.indices.push_back(rawPolygonData[idx + i]);
                    mesh.indices.push_back(rawPolygonData[idx + i + 1]);
                }
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<int>> triangulateTriangleStrips(const std::vector<int>& rawStripData, int numStrips) {
        int idx = 0;
        std::vector<std::vector<int>> cellToVertices(numStrips);
        for (int s = 0; s < numStrips; ++s) {
            if (idx >= static_cast<int>(rawStripData.size())) break;
            int nPoints = rawStripData[idx++];

            for (int i = 0; i < nPoints; ++i) {
                cellToVertices[s].push_back(rawStripData[idx + i]);
            }

            for (int i = 0; i < nPoints - 2; ++i) {
                if (idx + i + 2 < static_cast<int>(rawStripData.size())) {
                    int i0 = rawStripData[idx + i];
                    int i1 = rawStripData[idx + i + 1];
                    int i2 = rawStripData[idx + i + 2];
                    if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
                    else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
                }
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<int>> triangulateUnstructuredCells(const std::vector<int>& rawCellData, const std::vector<int>& cellTypes, int totalCells) {
        mesh.indices.clear();
        std::vector<std::vector<int>> cellToVertices(totalCells);
        int idx = 0;
        for (int c = 0; c < totalCells; ++c) {
            if (idx >= static_cast<int>(rawCellData.size())) break;
            int numPointsInCell = rawCellData[idx++];

            for (int i = 0; i < numPointsInCell; ++i) {
                cellToVertices[c].push_back(rawCellData[idx + i]);
            }

            int type = (c < static_cast<int>(cellTypes.size())) ? cellTypes[c] : 0;
            if (type == 0) {
                if (numPointsInCell == 3) type = 5;
            if (numPointsInCell == 4) type = 9;
                if (numPointsInCell == 8) type = 12;
            }

            switch (type) {
            case 5: // VTK_TRIANGLE
                if (idx + 2 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(rawCellData[idx + 0]);
                    mesh.indices.push_back(rawCellData[idx + 1]);
                    mesh.indices.push_back(rawCellData[idx + 2]);
                }
                break;
            case 9: // VTK_QUAD
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    int i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
                }
                break;
            case 10: // VTK_TETRA
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    int i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3, i0, i3, i1, i1, i3, i2 });
                }
                break;
            case 12: // VTK_HEXAHEDRON
                if (idx + 7 < static_cast<int>(rawCellData.size())) {
                    int i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    int i4 = rawCellData[idx + 4], i5 = rawCellData[idx + 5], i6 = rawCellData[idx + 6], i7 = rawCellData[idx + 7];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i3, i1, i1, i3, i2, i4, i5, i7, i5, i6, i7,
                        i0, i1, i4, i1, i5, i4, i2, i3, i6, i3, i7, i6,
                        i0, i4, i3, i3, i4, i7, i1, i2, i5, i2, i6, i5
                        });
                }
                break;
            case 13: // VTK_WEDGE
                if (idx + 5 < static_cast<int>(rawCellData.size())) {
                    int i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2];
                    int i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4], i5 = rawCellData[idx + 5];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i1, i2, i3, i5, i4,
                        i0, i1, i4, i0, i4, i3,
                        i1, i2, i5, i1, i5, i4,
                        i0, i2, i5, i0, i5, i3
                        });
                }
                break;
            case 14: // VTK_PYRAMID
                if (idx + 4 < static_cast<int>(rawCellData.size())) {
                    int i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1, i0, i3, i2,
                        i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                        });
                }
                break;
            case 11: { // VTK_VOXEL — structured-grid corner ordering, permute to HEX (0,1,3,2,4,5,7,6)
                if (idx + 7 < static_cast<int>(rawCellData.size())) {
                    int h0 = rawCellData[idx + 0], h1 = rawCellData[idx + 1], h2 = rawCellData[idx + 3], h3 = rawCellData[idx + 2];
                    int h4 = rawCellData[idx + 4], h5 = rawCellData[idx + 5], h6 = rawCellData[idx + 7], h7 = rawCellData[idx + 6];
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
                        mesh.indices.push_back(rawCellData[idx + 0]);
                        mesh.indices.push_back(rawCellData[idx + i]);
                        mesh.indices.push_back(rawCellData[idx + i + 1]);
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
        // vertices present but no triangles => blank viewport with no error.
        // Surface it instead of silently handing an empty draw call to the GPU.
        if (mesh.indices.empty()) {
            std::cerr << "VTK Parser Error: topology produced no triangles (missing/invalid CELLS/POLYGONS/DIMENSIONS). Mesh will not render." << std::endl;
            return;
        }

        // 1. Process cell data onto the compact vertex layout first
        extrapolateCellDataToPoints();

        // 2. Force complete unrolling for flat shading rendering setups
        std::vector<float> rawVertices = mesh.vertices;
        std::vector<int> originalIndices = mesh.indices;

        std::vector<float> unrolledVertices;
        std::vector<int> unrolledIndices;
        std::map<std::string, std::vector<float>> unrolledPointScalars;

        unrolledVertices.reserve(originalIndices.size() * 3);
        unrolledIndices.reserve(originalIndices.size());

        bool hasAttributes = mesh.attributes.has_value();

        for (size_t i = 0; i < originalIndices.size(); ++i) {
            int originalPointIdx = originalIndices[i];

            unrolledVertices.push_back(rawVertices[originalPointIdx * 3 + 0]);
            unrolledVertices.push_back(rawVertices[originalPointIdx * 3 + 1]);
            unrolledVertices.push_back(rawVertices[originalPointIdx * 3 + 2]);

            unrolledIndices.push_back(static_cast<int>(i));

            if (hasAttributes) {
                for (const auto& [name, scalarVec] : mesh.attributes->pointScalars) {
                    if (originalPointIdx >= 0 && originalPointIdx < static_cast<int>(scalarVec.size())) {
                        unrolledPointScalars[name].push_back(scalarVec[originalPointIdx]);
                    }
                    else {
                        unrolledPointScalars[name].push_back(0.0f);
                    }
                }
                // unroll per-point vectors to align with unrolled vertices
                for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
                    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                    if (originalPointIdx >= 0 && originalPointIdx * 3 + 2 < static_cast<int>(vecArr.size())) {
                        vx = vecArr[originalPointIdx * 3 + 0];
                        vy = vecArr[originalPointIdx * 3 + 1];
                        vz = vecArr[originalPointIdx * 3 + 2];
                    }
                    mesh.pointVectors[name].insert(mesh.pointVectors[name].end(), { vx, vy, vz });
                }
            }
        }

        mesh.vertices = std::move(unrolledVertices);
        mesh.indices = std::move(unrolledIndices);

        if (hasAttributes) {
            mesh.attributes->pointScalars = std::move(unrolledPointScalars);
        }

        // 3. Extract the active array directly for your GPU-bound flat vector representation
        if (!mesh.scalarName.empty() && hasAttributes && mesh.attributes->pointScalars.count(mesh.scalarName)) {
            mesh.scalars = mesh.attributes->pointScalars[mesh.scalarName];
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
        }

        if (mesh.vectorName.empty() && !mesh.availableVectorNames.empty()) {
            mesh.vectorName = mesh.availableVectorNames.front();
        }

        // 4. Calculate ranges and bounds safely on the new layout footprint
        calculateScalarRanges();
        mesh_utils::computeBounds(mesh);

        if (mesh.normals.empty() && !mesh.indices.empty()) {
            mesh_utils::computeNormals(mesh);
        }
    }

    void extrapolateCellDataToPoints() {
        if (cellScalarsStorage.empty() || globalCellToVertices.empty()) return;

        int vCount = mesh.vertices.size() / 3;

        // FIX: Safeguard optional initialization
        if (!mesh.attributes.has_value()) {
            mesh.attributes = DatasetAttributes();
        }

        for (const auto& [name, rawCellDataVec] : cellScalarsStorage) {
            std::vector<float> pointAlloc(vCount, 0.0f);
            std::vector<float> contributionCounts(vCount, 0.0f);

            for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
                if (c >= rawCellDataVec.size()) break;
                float val = rawCellDataVec[c];

                for (int vIdx : globalCellToVertices[c]) {
                    if (vIdx >= 0 && vIdx < vCount) {
                        pointAlloc[vIdx] += val;
                        contributionCounts[vIdx] += 1.0f;
                    }
                }
            }

            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) {
                    pointAlloc[i] /= contributionCounts[i];
                }
            }

            mesh.attributes->pointScalars[name] = std::move(pointAlloc);
        }
    }

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

            // Store exactly where renderer.cpp expects them
            mesh.attributes->scalarMin = minVal;
            mesh.attributes->scalarMax = maxVal;

            // Prevent zero-division errors if the dataset is completely uniform
            if (std::abs(mesh.attributes->scalarMax - mesh.attributes->scalarMin) < 1e-6f) {
                mesh.attributes->scalarMax = mesh.attributes->scalarMin + 1.0f;
            }
        }
    }
};

// ── Entry Interface ─────────────────────────────────────────────────────────

RenderMesh parseVTK(const std::string& filePath) {
    VTKParserContext parser(filePath);
    return parser.parse();
}