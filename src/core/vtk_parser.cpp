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
// (Byte-swap is centralized in mesh_utils::byteSwap; see readBinaryArray below.)

// ── Binary Read Helper ──────────────────────────────────────────────────────
// Reads `count` elements of type T, byte-swaps on little-endian hosts, and
// validates the byte count. On a short read, returns false (caller decides how
// to recover, e.g. drop the field / abort parsing) instead of letting a
// truncated block silently desync every following attribute.
template<typename T>
static bool readBinaryArray(std::ifstream& f, size_t count, std::vector<T>& out) {
    out.resize(count);
    if (count == 0) return true;
    const std::streamsize want = static_cast<std::streamsize>(count * sizeof(T));
    f.read(reinterpret_cast<char*>(out.data()), want);
    if (f.gcount() != want) return false;
    if (mesh_utils::isLittleEndian()) {
        for (size_t i = 0; i < count; ++i) mesh_utils::byteSwap(&out[i]);
    }
    return true;
}

// VTK legacy binary: every binary data block is written aligned to a 4-byte
// boundary *relative to the start of that block*. The padding applied after a
// block depends only on the block's own byte size, NOT on the absolute file
// position. If we aligned against the absolute position we would wrongly pad
// whenever a block happened to start at an unaligned offset (e.g. POINTS whose
// preceding ASCII header ended at offset 81), which would overshoot the next
// section header and desync every following block. All legacy VTK element types
// here are 4- or 8-byte wide, so a block's byte size is always a multiple of 4
// and the correct padding is therefore 0 — but we compute it from the bytes
// read so the rule is correct for any type. (File is opened in binary mode, so
// seeking is safe.)
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
    // VTK legacy binary indices/types are strictly 32-bit big-endian. Use a
    // fixed-width type so the byte count read from a binary stream is correct
    // on every platform (a platform-int read would corrupt 64-bit builds).
    std::vector<int32_t> rawCellData, cellTypes;

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
        else if (token == "VERTICES") { parseVerticesBlock(iss, file); }
        else if (token == "LINES") { parseLinesBlock(iss, file); }
        else if (token == "TRIANGLE_STRIPS") { parseTriangleStripsBlock(iss, file); }
        else if (token == "SCALARS") { parseScalarsBlock(iss, file); }
        else if (token == "NORMALS") { parseNormalsBlock(iss, file); }
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
                if (!readBinaryArray(file, count, tmp)) {
                    std::cerr << "VTK Parser Warning: short read on binary X/Y/Z_COORDINATES (DOUBLE)." << std::endl;
                    return;
                }
                for (int i = 0; i < count; ++i) axisCoords[i] = static_cast<float>(tmp[i]);
            } else {
                if (!readBinaryArray(file, count, axisCoords)) {
                    std::cerr << "VTK Parser Warning: short read on binary X/Y/Z_COORDINATES (FLOAT)." << std::endl;
                    return;
                }
            }
            alignStream4(file, static_cast<size_t>(count) * sizeof(float));
        }
        else {
            for (int i = 0; i < count; ++i) file >> axisCoords[i];
        }
    }

    void parsePointsBlock(std::istringstream& iss, std::ifstream& file) {
        long long parsedPoints = 0;
        iss >> parsedPoints;
        // VTK requires the POINTS count to agree with the structured-grid
        // DIMENSIONS product. If they disagree, the POINTS line is authoritative
        // (it is what the geometry block actually contains); warn and prefer it
        // so vertex sizing is correct.
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
            // Drain the rest of the line so the stream stays aligned.
            clearTrailingLine(file);
            mesh.vertices.clear();
            numPoints = 0;
            return;
        }
        numPoints = static_cast<int>(parsedPoints);
        std::string dataType; iss >> dataType; dataType = mesh_utils::toUpper(dataType);
        mesh.vertices.resize(static_cast<size_t>(numPoints) * 3);

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tempDouble(numPoints * 3);
                if (!readBinaryArray(file, tempDouble.size(), tempDouble)) {
                    std::cerr << "VTK Parser Warning: short read on binary POINTS (DOUBLE)." << std::endl;
                    mesh.vertices.clear();
                    return;
                }
                for (size_t i = 0; i < tempDouble.size(); ++i) {
                    float v = static_cast<float>(tempDouble[i]);
                    if (!std::isfinite(v)) {
                        std::cerr << "VTK Parser Warning: non-finite POINTS coordinate; dropping mesh." << std::endl;
                        mesh.vertices.clear();
                        return;
                    }
                    mesh.vertices[i] = v;
                }
            }
            else {
                if (!readBinaryArray(file, mesh.vertices.size(), mesh.vertices)) {
                    std::cerr << "VTK Parser Warning: short read on binary POINTS (FLOAT)." << std::endl;
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
            alignStream4(file, mesh.vertices.size() * sizeof(float));
        }
        else {
            for (int i = 0; i < numPoints * 3; ++i) {
                if (!(file >> mesh.vertices[i]) || !std::isfinite(mesh.vertices[i])) {
                    std::cerr << "VTK Parser Warning: non-finite POINTS coordinate; dropping mesh." << std::endl;
                    mesh.vertices.clear();
                    return;
                }
            }
            clearTrailingLine(file);
        }
    }

    void parseCellsBlock(std::istringstream& iss, std::ifstream& file) {
        iss >> numCells >> cellSize;
        rawCellData.resize(cellSize);
        if (isBinary) {
            if (!readBinaryArray(file, cellSize, rawCellData)) {
                std::cerr << "VTK Parser Warning: short read on binary CELLS." << std::endl;
                rawCellData.clear();
                return;
            }
            alignStream4(file, static_cast<size_t>(cellSize) * sizeof(int32_t));
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
            if (!readBinaryArray(file, numCells, rawTypes)) {
                std::cerr << "VTK Parser Warning: short read on binary CELL_TYPES." << std::endl;
                cellTypes.clear();
                return;
            }
            for (int i = 0; i < numCells; ++i) cellTypes[i] = rawTypes[i];
            alignStream4(file, static_cast<size_t>(numCells) * sizeof(int32_t));
        }
        else {
            for (int i = 0; i < numCells; ++i) file >> cellTypes[i];
            clearTrailingLine(file);
        }
    }

    void parsePolygonsBlock(std::istringstream& iss, std::ifstream& file) {
        int numPolys = 0, sizeOfPolysBlock = 0; iss >> numPolys >> sizeOfPolysBlock;
        std::vector<int32_t> polyData(sizeOfPolysBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfPolysBlock, polyData)) {
                std::cerr << "VTK Parser Warning: short read on binary POLYGONS." << std::endl;
                polyData.clear();
                return;
            }
            alignStream4(file, static_cast<size_t>(sizeOfPolysBlock) * sizeof(int32_t));
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

    // POLYDATA VERTICES: a list of cells, each cell a variable-length list
    // of point indices. Render as a triangle fan (same triangulation as
    // POLYGONS) so the geometry is drawable and the cell count matches
    // ParaView's "Polygonal Mesh" report.
    void parseVerticesBlock(std::istringstream& iss, std::ifstream& file) {
        int numVerts = 0, sizeOfVertsBlock = 0; iss >> numVerts >> sizeOfVertsBlock;
        std::vector<int32_t> vertData(sizeOfVertsBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfVertsBlock, vertData)) {
                std::cerr << "VTK Parser Warning: short read on binary VERTICES." << std::endl;
                vertData.clear();
                return;
            }
            alignStream4(file, static_cast<size_t>(sizeOfVertsBlock) * sizeof(int32_t));
        }
        else {
            for (int i = 0; i < sizeOfVertsBlock; ++i) file >> vertData[i];
            clearTrailingLine(file);
        }
        // VERTICES is its own topology source (see parsePolygonsBlock).
        mesh.indices.clear();
        globalCellToVertices = triangulatePolygons(vertData, numVerts);
        numCells = numVerts;
    }

    // POLYDATA LINES: a list of cells, each cell a polyline (variable-length
    // list of point indices). Emit GL_LINES pairs (p[i], p[i+1]) per segment.
    void parseLinesBlock(std::istringstream& iss, std::ifstream& file) {
        int numLines = 0, sizeOfLinesBlock = 0; iss >> numLines >> sizeOfLinesBlock;
        std::vector<int32_t> lineData(sizeOfLinesBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfLinesBlock, lineData)) {
                std::cerr << "VTK Parser Warning: short read on binary LINES." << std::endl;
                lineData.clear();
                return;
            }
            alignStream4(file, static_cast<size_t>(sizeOfLinesBlock) * sizeof(int32_t));
        }
        else {
            for (int i = 0; i < sizeOfLinesBlock; ++i) file >> lineData[i];
            clearTrailingLine(file);
        }
        // LINES is its own topology source (see parsePolygonsBlock).
        mesh.indices.clear();
        globalCellToVertices = triangulateLines(lineData, numLines);
        numCells = numLines;
    }

    void parseTriangleStripsBlock(std::istringstream& iss, std::ifstream& file) {
        int numStrips = 0, sizeOfStripsBlock = 0; iss >> numStrips >> sizeOfStripsBlock;
        std::vector<int32_t> stripData(sizeOfStripsBlock);
        if (isBinary) {
            if (!readBinaryArray(file, sizeOfStripsBlock, stripData)) {
                std::cerr << "VTK Parser Warning: short read on binary TRIANGLE_STRIPS." << std::endl;
                stripData.clear();
                return;
            }
            alignStream4(file, static_cast<size_t>(sizeOfStripsBlock) * sizeof(int32_t));
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
        if (numComponents < 1) numComponents = 1;
        dataType = mesh_utils::toUpper(dataType);

        // In the VTK legacy format the `LOOKUP_TABLE default` line is present in
        // BOTH ASCII and BINARY datasets: it is an ASCII text line that precedes
        // the binary scalar data. Skipping it in binary mode (as an earlier
        // revision did) made the binary scalar read begin at the
        // "LOOKUP_TABLE default\n" text bytes, yielding garbage/huge values in
        // the color field. Always consume the line (it may also name a custom
        // table; we only support the default table here).
        {
            std::string lutLine; getVTKLine(file, lutLine);
        }

        int activeElementCount = readingPointData ? numPoints : numCells;
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        // Pick the active scalar from the FIRST POINT_DATA scalar so a CELL_DATA
        // block listed earlier cannot shadow a valid point field (which would
        // leave mesh.scalars empty). finalizeMeshData also guards this.
        if (readingPointData && numComponents == 1 && mesh.scalarName.empty()) {
            mesh.scalarName = scalarName;
        }

        // Only a 1-component scalar can be used as the per-vertex color field.
        // A multi-component array would be uploaded as a 1-float-per-vertex
        // attribute and read misaligned (noisy colors). We still consume the
        // binary bytes (with alignment) so the stream stays in sync, then skip
        // the field rather than silently mis-sizing it.
        if (numComponents != 1) {
            std::cerr << "VTK Parser Warning: SCALARS '" << scalarName
                      << "' has " << numComponents << " components; only 1-component "
                      << "scalars are colorable, skipping." << std::endl;
            if (isBinary) {
                size_t total = static_cast<size_t>(activeElementCount) * static_cast<size_t>(numComponents);
                if (!consumeBinaryScalars(file, dataType, total)) {
                    std::cerr << "VTK Parser Warning: short read consuming multi-component SCALARS '" << scalarName << "'." << std::endl;
            }
            alignStream4(file, mesh.vertices.size() * sizeof(float));
        }
            return;
        }

        std::vector<float> readScalars(activeElementCount);

        // Tracks whether the binary read succeeded with the exact byte count.
        // If it under-reads, we must NOT leave a partial buffer behind — doing
        // so desynchronizes the stream and corrupts every following attribute
        // block (the "noisy colors" symptom on binary files).
        bool binaryReadOk = true;
        // A corrupt/mis-typed block may yield NaN/inf; never upload those as
        // colors/lighting inputs. Drop the field (leave it empty) so the shader
        // does not receive non-finite scalars, while keeping the byte stream in
        // sync so downstream blocks still parse.
        bool badField = false;

        if (isBinary) {
            if (dataType == "DOUBLE") {
                std::vector<double> tempDouble(readScalars.size());
                if (!readBinaryArray(file, tempDouble.size(), tempDouble)) binaryReadOk = false;
                else for (size_t i = 0; i < tempDouble.size(); ++i) readScalars[i] = static_cast<float>(tempDouble[i]);
            }
            else if (dataType == "FLOAT") {
                if (!readBinaryArray(file, readScalars.size(), readScalars)) binaryReadOk = false;
            }
            else if (dataType == "INT" || dataType == "UNSIGNED_INT" || dataType == "LONG") {
                std::vector<int32_t> tempInts(readScalars.size());
                if (!readBinaryArray(file, tempInts.size(), tempInts)) binaryReadOk = false;
                else for (size_t i = 0; i < tempInts.size(); ++i) readScalars[i] = static_cast<float>(tempInts[i]);
            }
            else if (dataType == "LONG_LONG" || dataType == "UNSIGNED_LONG_LONG") {
                std::vector<int64_t> tempLongs(readScalars.size());
                if (!readBinaryArray(file, tempLongs.size(), tempLongs)) binaryReadOk = false;
                else for (size_t i = 0; i < tempLongs.size(); ++i) readScalars[i] = static_cast<float>(tempLongs[i]);
            }
            else if (dataType == "SHORT" || dataType == "UNSIGNED_SHORT") {
                std::vector<int16_t> tempShorts(readScalars.size());
                if (!readBinaryArray(file, tempShorts.size(), tempShorts)) binaryReadOk = false;
                else for (size_t i = 0; i < tempShorts.size(); ++i) readScalars[i] = static_cast<float>(tempShorts[i]);
            }
            else if (dataType == "UNSIGNED_CHAR") {
                std::vector<uint8_t> tempBytes(readScalars.size());
                if (!readBinaryArray(file, tempBytes.size(), tempBytes)) binaryReadOk = false;
                else for (size_t i = 0; i < tempBytes.size(); ++i) readScalars[i] = static_cast<float>(tempBytes[i]);
            }
            if (binaryReadOk) {
                for (float v : readScalars) {
                    if (!std::isfinite(v)) { badField = true; break; }
                }
            }
            else {
                std::cerr << "VTK Parser Warning: unsupported SCALARS type '" << dataType
                          << "' for binary data; skipping field." << std::endl;
                binaryReadOk = false;
            }

            if (!binaryReadOk) {
                std::cerr << "VTK Parser Warning: short read on binary SCALARS '" << scalarName
                          << "'; skipping field to avoid stream desync." << std::endl;
                if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
                if (readingPointData) mesh.attributes->pointScalars[scalarName]; // ensure key exists, empty
                else { mesh.attributes->cellScalars[scalarName]; cellScalarsStorage[scalarName]; }
                alignStream4(file, static_cast<size_t>(activeElementCount) * static_cast<size_t>(numComponents) * sizeof(float));
                return;
            }
            alignStream4(file, static_cast<size_t>(activeElementCount) * static_cast<size_t>(numComponents) * sizeof(float));
        }
        else {
            for (size_t i = 0; i < readScalars.size(); ++i) {
                if (!(file >> readScalars[i]) || !std::isfinite(readScalars[i])) {
                    std::cerr << "VTK Parser Warning: non-finite ASCII SCALARS '" << scalarName
                              << "'; dropping field." << std::endl;
                    readScalars.clear();
                    badField = true;
                    break;
                }
            }
            clearTrailingLine(file);
        }

        // FIX: Ensure the optional `attributes` struct is instantiated before insertion
        if (!mesh.attributes.has_value()) {
            mesh.attributes = DatasetAttributes();
        }

        // A non-finite value was found — drop the field entirely so the shader
        // never receives NaN/inf, but keep the attributes key present (empty) so
        // downstream field-name bookkeeping stays consistent.
        if (badField) {
            if (readingPointData) mesh.attributes->pointScalars[scalarName]; // ensure key exists, empty
            else { mesh.attributes->cellScalars[scalarName]; cellScalarsStorage[scalarName]; }
            return;
        }

        if (readingPointData) {
            mesh.attributes->pointScalars[scalarName] = std::move(readScalars);
        }
        else {
            mesh.attributes->cellScalars[scalarName] = readScalars;
            cellScalarsStorage[scalarName] = std::move(readScalars);
        }
    }

    // Consume (and discard) `count` binary scalar values of the declared type so
    // the file position stays aligned for subsequent blocks. Used for
    // multi-component SCALARS that we intentionally skip for coloring.
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

    // POLYDATA NORMALS: a 3-component per-point attribute (same layout as
    // VECTORS). Store it directly in mesh.normals so the renderer uses the
    // author-provided normals instead of angle-based vertex splitting (which
    // would otherwise duplicate vertices and inflate the point count).
    void parseNormalsBlock(std::istringstream& iss, std::ifstream& file) {
        std::string normName, dataType;
        iss >> normName >> dataType;
        dataType = mesh_utils::toUpper(dataType);

        int activeElementCount = readingPointData ? numPoints : numCells;
        if (activeElementCount == 0 && attributeTargetCount > 0) {
            activeElementCount = attributeTargetCount;
        }

        std::vector<float> readNorms(static_cast<size_t>(activeElementCount) * 3);

        if (isBinary) {
            bool ok = true;
            if (dataType == "DOUBLE") {
                std::vector<double> tmp(readNorms.size());
                if (!readBinaryArray(file, tmp.size(), tmp)) ok = false;
                else for (size_t i = 0; i < tmp.size(); ++i) readNorms[i] = static_cast<float>(tmp[i]);
            } else { // FLOAT
                if (!readBinaryArray(file, readNorms.size(), readNorms)) ok = false;
            }
            if (!ok) {
                std::cerr << "VTK Parser Warning: short read on binary NORMALS '" << normName
                          << "'; skipping field to avoid stream desync." << std::endl;
                alignStream4(file, static_cast<size_t>(activeElementCount) * 3 * sizeof(float));
                return;
            }
            alignStream4(file, static_cast<size_t>(activeElementCount) * 3 * sizeof(float));
        } else {
            for (size_t i = 0; i < readNorms.size(); ++i) {
                if (!(file >> readNorms[i])) {
                    std::cerr << "VTK Parser Warning: non-finite ASCII NORMALS '" << normName
                              << "'; dropping field." << std::endl;
                    readNorms.clear();
                    break;
                }
            }
            clearTrailingLine(file);
        }

        if (readNorms.empty()) return;
        mesh.normals = std::move(readNorms);
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
            bool vecReadOk = true;
            if (dataType == "DOUBLE") {
                std::vector<double> tmp(readVecs.size());
                if (!readBinaryArray(file, tmp.size(), tmp)) vecReadOk = false;
                else for (size_t i = 0; i < tmp.size(); ++i) readVecs[i] = static_cast<float>(tmp[i]);
            } else { // FLOAT
                if (!readBinaryArray(file, readVecs.size(), readVecs)) vecReadOk = false;
            }
            if (!vecReadOk) {
                std::cerr << "VTK Parser Warning: short read on binary VECTORS '" << vecName
                          << "'; skipping field to avoid stream desync." << std::endl;
                alignStream4(file, readVecs.size() * sizeof(float));
                return;
            }
            for (float v : readVecs) {
                if (!std::isfinite(v)) {
                    std::cerr << "VTK Parser Warning: non-finite binary VECTORS '" << vecName
                              << "'; dropping field." << std::endl;
                    readVecs.clear();
                    break;
                }
            }
            alignStream4(file, readVecs.size() * sizeof(float));
        } else {
            for (size_t i = 0; i < readVecs.size(); ++i) {
                if (!(file >> readVecs[i]) || !std::isfinite(readVecs[i])) {
                    std::cerr << "VTK Parser Warning: non-finite ASCII VECTORS '" << vecName
                              << "'; dropping field." << std::endl;
                    readVecs.clear();
                    break;
                }
            }
            clearTrailingLine(file);
        }

        // POINT_DATA VECTORS map directly to vertices. CELL_DATA VECTORS are
        // extrapolated to vertices (averaged per incident cell) in finalize.
        // A non-finite vector component invalidates the whole field — drop it
        // so glyph/lighting math never receives NaN/inf.
        if (readVecs.empty()) {
            if (!mesh.attributes.has_value()) mesh.attributes = DatasetAttributes();
            if (readingPointData) mesh.attributes->pointVectors[vecName]; // ensure key exists, empty
            else cellVectorsStorage[vecName];
            return;
        }
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
        else if (datasetType == "POLYDATA") {
            // POLYDATA topology is supplied entirely by the POINTS + VERTICES /
            // LINES / POLYGONS / TRIANGLE_STRIPS blocks (parsed during the line
            // loop). If a topology block produced indices, keep them; otherwise
            // there is nothing to tessellate (points-only POLYDATA).
            if (mesh.indices.empty() && !mesh.vertices.empty()) {
                std::cerr << "VTK Parser Warning: POLYDATA has points but no VERTICES/LINES/POLYGONS/TRIANGLE_STRIPS; rendering points only." << std::endl;
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
            // dY>1 && dX>1 (a planar sheet). Clamp the inner loop bounds so we
            // never index a non-existent row (y+1) or column (x+1).
            for (int y = 0; y + 1 < dY; ++y)
                for (int x = 0; x + 1 < dX; ++x)
                    addQuad(idx(x, y, 0), idx(x + 1, y, 0), idx(x + 1, y + 1, 0), idx(x, y + 1, 0));
        } else if (dY == 1) {
            // dZ>1 && dX>1 (a wall in the XZ plane). No Y dimension to step in.
            for (int z = 0; z + 1 < dZ; ++z)
                for (int x = 0; x + 1 < dX; ++x)
                    addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
        } else if (dX == 1) {
            // dZ>1 && dY>1 (a wall in the YZ plane). No X dimension to step in.
            for (int z = 0; z + 1 < dZ; ++z)
                for (int y = 0; y + 1 < dY; ++y)
                    addQuad(idx(0, y, z), idx(0, y + 1, z), idx(0, y + 1, z + 1), idx(0, y, z + 1));
        }
        return cellToVertices;
    }

    std::vector<std::vector<uint32_t>> triangulatePolygons(const std::vector<int32_t>& rawPolygonData, int numPolys) {
        int idx = 0;
        std::vector<std::vector<uint32_t>> cellToVertices(numPolys);
        for (int p = 0; p < numPolys; ++p) {
            if (idx >= static_cast<int>(rawPolygonData.size())) break;
            int nPoints = rawPolygonData[idx++];
            // Guard against a malformed/truncated polygon claiming more points
            // than the buffer holds — reading past the end is UB / a crash.
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
        int idx = 0;
        std::vector<std::vector<uint32_t>> cellToVertices(numStrips);
        for (int s = 0; s < numStrips; ++s) {
            if (idx >= static_cast<int>(rawStripData.size())) break;
            int nPoints = rawStripData[idx++];
            if (nPoints < 0 || idx + nPoints > static_cast<int>(rawStripData.size())) break;

            for (int i = 0; i < nPoints; ++i) {
                cellToVertices[s].push_back(static_cast<uint32_t>(rawStripData[idx + i]));
            }

            for (int i = 0; i < nPoints - 2; ++i) {
                uint32_t i0 = rawStripData[idx + i];
                uint32_t i1 = rawStripData[idx + i + 1];
                uint32_t i2 = rawStripData[idx + i + 2];
                if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
                else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
            }
            idx += nPoints;
        }
        return cellToVertices;
    }

    // POLYDATA LINES: each cell is a polyline; emit a GL_LINES segment
    // (p[i], p[i+1]) per consecutive pair. Each cell's full vertex list is
    // retained in cellToVertices for attribute extrapolation.
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
        // vertices present but no triangles => blanket error for structured /
        // unstructured grids (they should always tessellate). For POLYDATA it is
        // legitimate to have a points-only or lines-only mesh, so still deliver
        // it to the GPU rather than dropping it as an error.
        if (mesh.indices.empty() && datasetType != "POLYDATA") {
            std::cerr << "VTK Parser Error: topology produced no triangles (missing/invalid CELLS/POLYGONS/DIMENSIONS). Mesh will not render." << std::endl;
            return;
        }

        // Bounds-safety: an out-of-range index (from a malformed/truncated CELLS/
        // POLYGONS/STRIPS block, or a POINTS/CELLS count mismatch) would be an
        // out-of-bounds read in the GL vertex fetch (mesh.indices -> vertices)
        // and either render garbage or crash the driver. Drop the whole index
        // buffer so we hand the renderer a clean, drawable (empty) mesh instead.
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
        // 3. Extract the active array directly for your GPU-bound flat vector representation.
        // Only 1-component point scalars are usable as the per-vertex color field.
        // The active name may have been set from a CELL_DATA block (which is stored
        // under cellScalars, not pointScalars), so fall back to the first available
        // point scalar if the named one isn't in pointScalars. This keeps coloring
        // correct regardless of section order in the file.
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

        // Per-field accumulation buffers. Use a name->index map rather than
        // relying on identical unordered_map iteration order across separate
        // loops — std::unordered_map order is not guaranteed stable.
        struct FieldAcc {
            std::vector<float> sum;      // scalars: per-vertex; vectors: per-vertex*3
        };
        std::vector<FieldAcc> accs;
        accs.reserve(cellScalarsStorage.size() + cellVectorsStorage.size());
        std::map<std::string, size_t> scalarAccIdx;
        std::map<std::string, size_t> vectorAccIdx;

        for (const auto& [name, raw] : cellScalarsStorage) {
            scalarAccIdx[name] = accs.size();
            accs.push_back(FieldAcc{ std::vector<float>(vCount, 0.0f) });
        }
        for (const auto& [name, raw] : cellVectorsStorage) {
            vectorAccIdx[name] = accs.size();
            accs.push_back(FieldAcc{ std::vector<float>(static_cast<size_t>(vCount) * 3, 0.0f) });
        }
        std::vector<float> contributionCounts(vCount, 0.0f);

        // Single traversal of the incidence structure built during triangulation.
        for (size_t c = 0; c < globalCellToVertices.size(); ++c) {
            // Accumulate scalar fields for this cell.
            for (const auto& [name, raw] : cellScalarsStorage) {
                if (c >= raw.size()) continue;
                float val = raw[c];
                size_t si = scalarAccIdx[name];
                for (int vIdx : globalCellToVertices[c]) {
                    if (vIdx >= 0 && vIdx < vCount) accs[si].sum[vIdx] += val;
                }
            }
            // Accumulate vector fields for this cell.
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
            // One contribution per cell per vertex.
            for (int vIdx : globalCellToVertices[c]) {
                if (vIdx >= 0 && vIdx < vCount) contributionCounts[vIdx] += 1.0f;
            }
        }

        // Normalize and store.
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