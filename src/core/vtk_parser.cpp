#include "core/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <sstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

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

        mesh.datasetType = datasetType.empty() ? "UNKNOWN" : datasetType;
        mesh.fileFormat = "VTK";
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
    std::unordered_map<std::string, std::vector<float>> cellScalarsStorage;
    std::unordered_map<std::string, std::vector<float>> cellVectorsStorage; // CELL_DATA VECTORS, extrapolated to points
    std::vector<std::vector<uint32_t>> globalCellToVertices;

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
        // POLYGONS/STRIPS are their own topology source: start from a clean
        // index list so they never concatenate onto a preceding CELLS block
        // (each block is authoritative for the final `mesh.indices`).
        mesh.indices.clear();
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
        // See parsePolygonsBlock: own topology stream, clear first.
        mesh.indices.clear();
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

        // The `LOOKUP_TABLE default` line exists ONLY in ASCII datasets. In
        // BINARY mode the next 4 bytes belong to the scalar data itself, so
        // reading a "line" here would consume the first float and corrupt the
        // entire field. Guard the read on !isBinary.
        if (!isBinary) {
            std::string lutLine; getVTKLine(file, lutLine);
        }

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
            else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG" || dataType == "SHORT") {
                std::vector<int32_t> tempInts(readScalars.size());
                file.read(reinterpret_cast<char*>(tempInts.data()), tempInts.size() * sizeof(int32_t));
                fixEndianness(tempInts.data(), tempInts.size());
                for (size_t i = 0; i < tempInts.size(); ++i) readScalars[i] = static_cast<float>(tempInts[i]);
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

        // POINT_DATA VECTORS map directly to vertices. CELL_DATA VECTORS are
        // extrapolated to vertices (averaged per incident cell) in finalize.
        if (readingPointData) {
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
            // Curvilinear grid: point positions come from the POINTS block (parsed
            // separately); we only build the surface tessellation here.
            if (!mesh.vertices.empty()) {
                globalCellToVertices = generateStructuredGridSurface(dimX, dimY, dimZ);
                numCells = static_cast<int>(globalCellToVertices.size());
            }
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

    // Build a renderable SURFACE for a STRUCTURED_GRID (curvilinear) dataset.
    // A curvilinear grid is a deformed lattice whose point positions are given
    // explicitly (POINTS block); its topology is still a regular DIMENSIONS grid.
    // Rendering the full volumetric tessellation is wasteful and hides interior
    // faces, so we emit the 6 boundary faces for a 3D grid (or the single planar
    // layer for a 2D grid). Point indexing follows VTK: idx = x + y*dX + z*dX*dY
    // (x fastest), matching the order the POINTS block is written in.
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
            // All six faces are wound CCW as seen from OUTSIDE the box (i.e. the
            // triangle (a,b,c) of each addQuad has its geometric normal pointing
            // along the face's outward direction). This makes the surface robust
            // to GL_CULL_FACE ever being enabled (S1).

            // Bottom (z = 0, outward -Z): CCW seen from -Z
            for (int y = 0; y < cy; ++y)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, y, 0), idx(x, y + 1, 0), idx(x + 1, y + 1, 0), idx(x + 1, y, 0));
            // Top (z = dZ-1, outward +Z): CCW seen from +Z
            for (int y = 0; y < cy; ++y)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, y, dZ - 1), idx(x + 1, y, dZ - 1), idx(x + 1, y + 1, dZ - 1), idx(x, y + 1, dZ - 1));
            // Left (x = 0, outward -X): CCW seen from -X
            for (int z = 0; z < cz; ++z)
                for (int y = 0; y < cy; ++y)
                    addQuad(idx(0, y, z), idx(0, y, z + 1), idx(0, y + 1, z + 1), idx(0, y + 1, z));
            // Right (x = dX-1, outward +X): CCW seen from +X
            for (int z = 0; z < cz; ++z)
                for (int y = 0; y < cy; ++y)
                    addQuad(idx(dX - 1, y, z), idx(dX - 1, y + 1, z), idx(dX - 1, y + 1, z + 1), idx(dX - 1, y, z + 1));
            // Back (y = 0, outward -Y): CCW seen from -Y
            for (int z = 0; z < cz; ++z)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
            // Front (y = dY-1, outward +Y): CCW seen from +Y
            for (int z = 0; z < cz; ++z)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, dY - 1, z), idx(x, dY - 1, z + 1), idx(x + 1, dY - 1, z + 1), idx(x + 1, dY - 1, z));
        } else if (dZ == 1) {
            for (int y = 0; y < cy; ++y)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, y, 0), idx(x + 1, y, 0), idx(x + 1, y + 1, 0), idx(x, y + 1, 0));
        } else if (dY == 1) {
            for (int z = 0; z < cz; ++z)
                for (int x = 0; x < cx; ++x)
                    addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
        } else if (dX == 1) {
            for (int z = 0; z < cz; ++z)
                for (int y = 0; y < cy; ++y)
                    addQuad(idx(0, y, z), idx(0, y + 1, z), idx(0, y + 1, z + 1), idx(0, y, z + 1));
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulatePolygons(const std::vector<int>& rawPolygonData, int numPolys) {
        int idx = 0;
        std::vector<std::vector<uint32_t>> cellToVertices(numPolys);
        for (int p = 0; p < numPolys; ++p) {
            if (idx >= static_cast<int>(rawPolygonData.size())) break;
            int nPoints = rawPolygonData[idx++];

            for (int i = 0; i < nPoints; ++i) {
                cellToVertices[p].push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
            }

            for (int i = 1; i < nPoints - 1; ++i) {
                if (idx + i + 1 <= static_cast<int>(rawPolygonData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i + 1]));
                }
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulateTriangleStrips(const std::vector<int>& rawStripData, int numStrips) {
        int idx = 0;
        std::vector<std::vector<uint32_t>> cellToVertices(numStrips);
        for (int s = 0; s < numStrips; ++s) {
            if (idx >= static_cast<int>(rawStripData.size())) break;
            int nPoints = rawStripData[idx++];

            for (int i = 0; i < nPoints; ++i) {
                cellToVertices[s].push_back(static_cast<uint32_t>(rawStripData[idx + i]));
            }

            for (int i = 0; i < nPoints - 2; ++i) {
                if (idx + i + 2 < static_cast<int>(rawStripData.size())) {
                    uint32_t i0 = rawStripData[idx + i];
                    uint32_t i1 = rawStripData[idx + i + 1];
                    uint32_t i2 = rawStripData[idx + i + 2];
                    if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
                    else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
                }
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(const std::vector<int>& rawCellData, const std::vector<int>& cellTypes, int totalCells) {
        mesh.indices.clear();
        std::vector<std::vector<uint32_t>> cellToVertices(totalCells);
        int idx = 0;
        for (int c = 0; c < totalCells; ++c) {
            if (idx >= static_cast<int>(rawCellData.size())) break;
            int numPointsInCell = rawCellData[idx++];

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
            case 5: // VTK_TRIANGLE
                if (idx + 2 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 1]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 2]));
                }
                break;
            case 9: // VTK_QUAD
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
                }
                break;
            case 10: // VTK_TETRA
                if (idx + 3 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3];
                    mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3, i0, i3, i1, i1, i3, i2 });
                }
                break;
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
            case 13: // VTK_WEDGE
                if (idx + 5 < static_cast<int>(rawCellData.size())) {
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2];
                    uint32_t i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4], i5 = rawCellData[idx + 5];
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
                    uint32_t i0 = rawCellData[idx + 0], i1 = rawCellData[idx + 1], i2 = rawCellData[idx + 2], i3 = rawCellData[idx + 3], i4 = rawCellData[idx + 4];
                    mesh.indices.insert(mesh.indices.end(), {
                        i0, i2, i1, i0, i3, i2,
                        i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                        });
                }
                break;
            case 11: { // VTK_VOXEL — structured-grid corner ordering, permute to HEX (0,1,3,2,4,5,7,6)
                if (idx + 7 < static_cast<int>(rawCellData.size())) {
                    uint32_t h0 = rawCellData[idx + 0], h1 = rawCellData[idx + 1], h2 = rawCellData[idx + 3], h3 = rawCellData[idx + 2];
                    uint32_t h4 = rawCellData[idx + 4], h5 = rawCellData[idx + 5], h6 = rawCellData[idx + 7], h7 = rawCellData[idx + 6];
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

        // 1. Process cell data onto the compact vertex layout first (merged
        //    scalar+vector extrapolation in a single cell-to-vertex pass).
        extrapolateCellDataToPointsMerged();

        // 2. Keep the mesh INDEXED (shared vertices). Previously the mesh was fully
        //    "unrolled" into a non-indexed layout (one copy of every vertex per
        //    triangle). For a 50^3 volume that is 4.2M triangles -> 12.7M vertices,
        //    and every scalar/vector attribute was duplicated the same way, blowing
        //    RAM to ~1.4 GB. Flat shading is instead obtained via angle-based
        //    normals in computeNormals(), which runs on the indexed layout and only
        //    splits normals at genuinely sharp edges.
        if (mesh.attributes.has_value()) {
            // Align per-point vectors with the (shared) vertices for glyph rendering.
            for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
                mesh.pointVectors[name] = vecArr;
            }
        }

        bool hasAttributes = mesh.attributes.has_value();

        // 3. Extract the active array directly for your GPU-bound flat vector representation.
        // Only 1-component point scalars are usable as the per-vertex color field;
        // a multi-component (e.g. SCALARS name float 3) array would be uploaded
        // as a 1-float-per-vertex attribute and read misaligned. Guard it.
        if (!mesh.scalarName.empty() && hasAttributes && mesh.attributes->pointScalars.count(mesh.scalarName)) {
            const std::vector<float>& active = mesh.attributes->pointScalars[mesh.scalarName];
            size_t vCount = mesh.vertices.size() / 3;
            if (!active.empty() && active.size() == vCount) {
                mesh.scalars = active;
            } else {
                std::cerr << "VTK Parser Warning: active scalar '" << mesh.scalarName
                          << "' is not 1-component per vertex; scalar coloring disabled." << std::endl;
                mesh.scalars.clear();
            }
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

    void extrapolateCellVectorsToPoints() {
        if (cellVectorsStorage.empty() || globalCellToVertices.empty()) return;

        int vCount = mesh.vertices.size() / 3;
        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

        for (const auto& [name, rawCellVecs] : cellVectorsStorage) {
            std::vector<float> pointAlloc(static_cast<size_t>(vCount) * 3, 0.0f);
            std::vector<float> contributionCounts(vCount, 0.0f);

            for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
                if (c >= rawCellVecs.size() / 3) break;
                float vx = rawCellVecs[c * 3 + 0];
                float vy = rawCellVecs[c * 3 + 1];
                float vz = rawCellVecs[c * 3 + 2];

                for (int vIdx : globalCellToVertices[c]) {
                    if (vIdx >= 0 && vIdx < vCount) {
                        pointAlloc[static_cast<size_t>(vIdx) * 3 + 0] += vx;
                        pointAlloc[static_cast<size_t>(vIdx) * 3 + 1] += vy;
                        pointAlloc[static_cast<size_t>(vIdx) * 3 + 2] += vz;
                        contributionCounts[vIdx] += 1.0f;
                    }
                }
            }

            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) {
                    float inv = 1.0f / contributionCounts[i];
                    pointAlloc[static_cast<size_t>(i) * 3 + 0] *= inv;
                    pointAlloc[static_cast<size_t>(i) * 3 + 1] *= inv;
                    pointAlloc[static_cast<size_t>(i) * 3 + 2] *= inv;
                }
            }

            mesh.attributes->pointVectors[name] = std::move(pointAlloc);
        }
    }

    // Merge the cell-scalar and cell-vector extrapolation into a SINGLE pass over
    // the cell-to-vertex incidence (built once during triangulation) instead of
    // iterating globalCellToVertices twice. This replaces the two separate
    // extrapolate* calls, halving the O(cells * verts) work for unstructured grids.
    void extrapolateCellDataToPointsMerged() {
        if (globalCellToVertices.empty()) return;
        if (cellScalarsStorage.empty() && cellVectorsStorage.empty()) return;

        int vCount = mesh.vertices.size() / 3;
        if (vCount == 0) return;
        if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();

        // Per-field accumulation buffers, allocated once for the whole pass.
        struct FieldAcc {
            bool isVector = false;
            std::vector<float> sum;      // scalars: per-vertex; vectors: per-vertex*3
        };
        std::vector<FieldAcc> accs;
        accs.reserve(cellScalarsStorage.size() + cellVectorsStorage.size());

        for (const auto& [name, raw] : cellScalarsStorage) {
            accs.push_back(FieldAcc{ false, std::vector<float>(vCount, 0.0f) });
        }
        size_t scalarAccCount = accs.size();
        for (const auto& [name, raw] : cellVectorsStorage) {
            accs.push_back(FieldAcc{ true, std::vector<float>(static_cast<size_t>(vCount) * 3, 0.0f) });
        }
        std::vector<float> contributionCounts(vCount, 0.0f);

        // Single traversal of the incidence structure built during triangulation.
        for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
            // Accumulate scalar fields for this cell.
            size_t si = 0;
            for (auto it = cellScalarsStorage.begin(); it != cellScalarsStorage.end(); ++it, ++si) {
                if (c >= it->second.size()) continue;
                float val = it->second[c];
                for (int vIdx : globalCellToVertices[c]) {
                    if (vIdx >= 0 && vIdx < vCount) accs[si].sum[vIdx] += val;
                }
            }
            // Accumulate vector fields for this cell.
            size_t vi = scalarAccCount;
            for (auto it = cellVectorsStorage.begin(); it != cellVectorsStorage.end(); ++it, ++vi) {
                if (c >= it->second.size() / 3) continue;
                float vx = it->second[c * 3 + 0];
                float vy = it->second[c * 3 + 1];
                float vz = it->second[c * 3 + 2];
                for (int vIdx : globalCellToVertices[c]) {
                    if (vIdx >= 0 && vIdx < vCount) {
                        accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 0] += vx;
                        accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 1] += vy;
                        accs[vi].sum[static_cast<size_t>(vIdx) * 3 + 2] += vz;
                    }
                }
            }
            // One contribution per cell per vertex.
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) contributionCounts[vIdx] += 1.0f;
            }
        }

        // Normalize and store.
        size_t si = 0;
        for (auto it = cellScalarsStorage.begin(); it != cellScalarsStorage.end(); ++it, ++si) {
            std::vector<float>& sum = accs[si].sum;
            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) sum[i] /= contributionCounts[i];
            }
            mesh.attributes->pointScalars[it->first] = std::move(sum);
        }
        size_t vi = scalarAccCount;
        for (auto it = cellVectorsStorage.begin(); it != cellVectorsStorage.end(); ++it, ++vi) {
            std::vector<float>& sum = accs[vi].sum;
            for (int i = 0; i < vCount; ++i) {
                if (contributionCounts[i] > 0.0f) {
                    float inv = 1.0f / contributionCounts[i];
                    sum[static_cast<size_t>(i) * 3 + 0] *= inv;
                    sum[static_cast<size_t>(i) * 3 + 1] *= inv;
                    sum[static_cast<size_t>(i) * 3 + 2] *= inv;
                }
            }
            mesh.attributes->pointVectors[it->first] = std::move(sum);
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