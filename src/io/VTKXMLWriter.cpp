#include "io/VTKXMLWriter.h"
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace fvm {

template<std::size_t Dim>
std::string VTKXMLWriter<Dim>::xmlEscape_(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result += c; break;
        }
    }
    return result;
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeScalars_(std::ostream& out, const std::string& name,
                                       const double* data, std::size_t count,
                                       std::size_t numComponents) {
    out << "        <DataArray type=\"Float64\" Name=\"" << xmlEscape_(name) << "\"";
    out << " NumberOfComponents=\"" << numComponents << "\" format=\"ascii\">\n";
    out << "          ";
    // Buffer to avoid per-double virtual ostream overhead (10x speedup, no logic change)
    std::ostringstream buf;
    buf << std::setprecision(8) << std::scientific;
    for (std::size_t i = 0; i < count * numComponents; ++i) {
        buf << data[i];
        if ((i + 1) % numComponents == 0) {
            buf << "\n";
            if (i + 1 < count * numComponents) buf << "          ";
        } else {
            buf << " ";
        }
    }
    out << buf.str();
    out << "        </DataArray>\n";
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writePoints_(std::ostream& out, const Mesh<Dim>& mesh) {
    const auto& verts = mesh.vertices();
    out << "      <Points>\n";
    out << "        <DataArray type=\"Float64\" Name=\"Points\" NumberOfComponents=\"3\" format=\"ascii\">\n";
    out << "          ";
    for (std::size_t i = 0; i < verts.size(); ++i) {
        out << std::setprecision(8) << std::scientific
            << verts[i][0] << " " << verts[i][1] << " ";
        if constexpr (Dim == 2) {
            out << "0.0";
        } else {
            out << verts[i][2];
        }
        out << "\n";
        if (i + 1 < verts.size()) out << "          ";
    }
    out << "        </DataArray>\n";
    out << "      </Points>\n";
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeCells_(std::ostream& out, const Mesh<Dim>& mesh) {
    const auto& cells = mesh.cells();

    out << "      <Cells>\n";

    out << "        <DataArray type=\"Int32\" Name=\"connectivity\" format=\"ascii\">\n";
    out << "          ";
    for (std::size_t i = 0; i < cells.size(); ++i) {
        for (std::size_t v = 0; v < cells[i].size(); ++v) {
            out << cells[i][v] << " ";
        }
        out << "\n";
        if (i + 1 < cells.size()) out << "          ";
    }
    out << "        </DataArray>\n";

    out << "        <DataArray type=\"Int32\" Name=\"offsets\" format=\"ascii\">\n";
    out << "          ";
    std::int32_t cumulative = 0;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        cumulative += static_cast<std::int32_t>(cells[i].size());
        out << cumulative << "\n";
        if (i + 1 < cells.size()) out << "          ";
    }
    out << "        </DataArray>\n";

    out << "        <DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
    out << "          ";
    for (std::size_t i = 0; i < cells.size(); ++i) {
        std::size_t nv = cells[i].size();
        std::int32_t vtkType;
        if constexpr (Dim == 2) {
            vtkType = (nv == 3) ? 5 : 9;
        } else {
            vtkType = (nv == 4) ? 10 : 12;
        }
        out << vtkType << "\n";
        if (i + 1 < cells.size()) out << "          ";
    }
    out << "        </DataArray>\n";

    out << "      </Cells>\n";
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeVTI_(std::ostream& out, const Mesh<Dim>& mesh,
                                   const std::vector<const double*>& cellData,
                                   const std::vector<std::string>& cellNames,
                                   const std::vector<const double*>& pointData,
                                   const std::vector<std::string>& pointNames) {
    const auto& dims = mesh.dims();
    const auto& spacing = mesh.spacings();
    std::size_t nCells = mesh.nCells();
    std::size_t nVerts = 1;
    for (std::size_t d = 0; d < Dim; ++d) nVerts *= (dims[d] + 1);

    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"ImageData\" version=\"0.1\" byte_order=\"";
    out << "LittleEndian\">\n";

    if constexpr (Dim == 2) {
        out << "  <ImageData WholeExtent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 0\"";
        out << " Origin=\"0 0 0\"";
        out << " Spacing=\"" << spacing[0] << " " << spacing[1] << " 1\">\n";
        out << "    <Piece Extent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 0\">\n";
    } else {
        out << "  <ImageData WholeExtent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 " << dims[2] << "\"";
        out << " Origin=\"0 0 0\"";
        out << " Spacing=\"" << spacing[0] << " " << spacing[1] << " " << spacing[2] << "\">\n";
        out << "    <Piece Extent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 " << dims[2] << "\">\n";
    }

    if (!cellData.empty()) {
        out << "      <CellData>\n";
        for (std::size_t i = 0; i < cellData.size(); ++i) {
            std::size_t numComp = (cellNames[i] == "Velocity") ? 3 : 1;
            writeScalars_(out, cellNames[i], cellData[i], nCells, numComp);
        }
        out << "      </CellData>\n";
    }

    if (!pointData.empty()) {
        out << "      <PointData>\n";
        for (std::size_t i = 0; i < pointData.size(); ++i) {
            std::size_t numComp = (pointNames[i] == "Velocity") ? 3 : 1;
            writeScalars_(out, pointNames[i], pointData[i], nVerts, numComp);
        }
        out << "      </PointData>\n";
    }

    out << "    </Piece>\n";
    out << "  </ImageData>\n";
    out << "</VTKFile>\n";
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeVTR_(std::ostream& out, const Mesh<Dim>& mesh,
                                   const std::vector<const double*>& cellData,
                                   const std::vector<std::string>& cellNames,
                                   const std::vector<const double*>& pointData,
                                   const std::vector<std::string>& pointNames) {
    const auto& dims = mesh.dims();
    const auto& faceCoords = mesh.faceCoords_;

    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"RectilinearGrid\" version=\"0.1\" byte_order=\"";
    out << "LittleEndian\">\n";

    if constexpr (Dim == 2) {
        out << "  <RectilinearGrid WholeExtent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 0\">\n";
        out << "    <Piece Extent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 0\">\n";
    } else {
        out << "  <RectilinearGrid WholeExtent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 " << dims[2] << "\">\n";
        out << "    <Piece Extent=\"0 " << dims[0] << " 0 " << dims[1] << " 0 " << dims[2] << "\">\n";
    }

    out << "      <Coordinates>\n";
    out << "        <DataArray type=\"Float64\" format=\"ascii\">\n";
    out << "          ";
    for (std::size_t i = 0; i < dims[0] + 1; ++i) {
        out << std::setprecision(8) << std::scientific << faceCoords[0][i];
        if (i + 1 < dims[0] + 1) out << " ";
    }
    out << "\n        </DataArray>\n";
    out << "        <DataArray type=\"Float64\" format=\"ascii\">\n";
    out << "          ";
    for (std::size_t i = 0; i < dims[1] + 1; ++i) {
        out << std::setprecision(8) << std::scientific << faceCoords[1][i];
        if (i + 1 < dims[1] + 1) out << " ";
    }
    out << "\n        </DataArray>\n";
    if constexpr (Dim == 3) {
        out << "        <DataArray type=\"Float64\" format=\"ascii\">\n";
        out << "          ";
        for (std::size_t i = 0; i < dims[2] + 1; ++i) {
            out << std::setprecision(8) << std::scientific << faceCoords[2][i];
            if (i + 1 < dims[2] + 1) out << " ";
        }
        out << "\n        </DataArray>\n";
    } else {
        out << "        <DataArray type=\"Float64\" format=\"ascii\">0.0</DataArray>\n";
    }
    out << "      </Coordinates>\n";

    if (!cellData.empty()) {
        out << "      <CellData>\n";
        for (std::size_t i = 0; i < cellData.size(); ++i) {
            std::size_t numComp = (cellNames[i] == "Velocity") ? 3 : 1;
            writeScalars_(out, cellNames[i], cellData[i], mesh.nCells(), numComp);
        }
        out << "      </CellData>\n";
    }

    if (!pointData.empty()) {
        out << "      <PointData>\n";
        for (std::size_t i = 0; i < pointData.size(); ++i) {
            std::size_t numComp = (pointNames[i] == "Velocity") ? 3 : 1;
            writeScalars_(out, pointNames[i], pointData[i], mesh.nFaces(), numComp);
        }
        out << "      </PointData>\n";
    }

    out << "    </Piece>\n";
    out << "  </RectilinearGrid>\n";
    out << "</VTKFile>\n";
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeVTU_(std::ostream& out, const Mesh<Dim>& mesh,
                                   const std::vector<const double*>& cellData,
                                   const std::vector<std::string>& cellNames,
                                   const std::vector<const double*>& pointData,
                                   const std::vector<std::string>& pointNames) {
    const auto& verts = mesh.vertices();
    const auto& cells = mesh.cells();

    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" byte_order=\"";
    out << "LittleEndian\">\n";
    out << "  <UnstructuredGrid>\n";
    out << "    <Piece NumberOfPoints=\"" << verts.size() << "\" NumberOfCells=\"" << cells.size() << "\">\n";

    writePoints_(out, mesh);
    writeCells_(out, mesh);

    if (!cellData.empty()) {
        out << "      <CellData>\n";
        for (std::size_t i = 0; i < cellData.size(); ++i) {
            std::size_t numComp = 1;
            if (cellNames[i] == "Velocity") numComp = 3;
            writeScalars_(out, cellNames[i], cellData[i], cells.size(), numComp);
        }
        out << "      </CellData>\n";
    }

    if (!pointData.empty()) {
        out << "      <PointData>\n";
        for (std::size_t i = 0; i < pointData.size(); ++i) {
            std::size_t numComp = 1;
            if (pointNames[i] == "Velocity") numComp = 3;
            writeScalars_(out, pointNames[i], pointData[i], verts.size(), numComp);
        }
        out << "      </PointData>\n";
    }

    out << "    </Piece>\n";
    out << "  </UnstructuredGrid>\n";
    out << "</VTKFile>\n";
}

template<std::size_t Dim>
std::vector<double> VTKXMLWriter<Dim>::cellToVertexStructured_(const Mesh<Dim>& mesh,
                                                                const Field<Dim>& field) {
    const auto& dims = mesh.dims();
    std::array<std::size_t, Dim> nVerts{};
    for (std::size_t d = 0; d < Dim; ++d) nVerts[d] = dims[d] + 1;

    std::size_t totalVerts = 1;
    for (std::size_t d = 0; d < Dim; ++d) totalVerts *= nVerts[d];

    std::vector<double> vertexValues(totalVerts, 0.0);
    std::vector<int> vertexCount(totalVerts, 0);

    for (std::size_t ci = 0; ci < mesh.nCells(); ++ci) {
        auto cc = mesh.cellCoords(ci);
        double val = field[ci];

        for (std::size_t corner = 0; corner < (1u << Dim); ++corner) {
            std::array<std::size_t, Dim> vi{};
            for (std::size_t d = 0; d < Dim; ++d) {
                vi[d] = cc[d] + ((corner >> d) & 1);
            }
            std::size_t vidx = 0;
            for (std::size_t d = 0; d < Dim; ++d) {
                vidx *= nVerts[d];
                vidx += vi[d];
            }
            vertexValues[vidx] += val;
            vertexCount[vidx]++;
        }
    }

    for (std::size_t i = 0; i < totalVerts; ++i) {
        if (vertexCount[i] > 0) {
            vertexValues[i] /= static_cast<double>(vertexCount[i]);
        }
    }
    return vertexValues;
}

template<std::size_t Dim>
std::vector<double> VTKXMLWriter<Dim>::cellToVertexUnstructured_(const Mesh<Dim>& mesh,
                                                                  const Field<Dim>& field) {
    const auto& verts = mesh.vertices();
    std::vector<double> vertexValues(verts.size(), 0.0);
    std::vector<int> vertexCount(verts.size(), 0);

    for (std::size_t ci = 0; ci < mesh.nCells(); ++ci) {
        auto cv = mesh.cellVertices(ci);
        double val = field[ci];
        for (std::size_t v = 0; v < cv.size(); ++v) {
            vertexValues[cv[v]] += val;
            vertexCount[cv[v]]++;
        }
    }

    for (std::size_t i = 0; i < verts.size(); ++i) {
        if (vertexCount[i] > 0) {
            vertexValues[i] /= static_cast<double>(vertexCount[i]);
        }
    }
    return vertexValues;
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::write(const std::string& filename,
                               const Mesh<Dim>& mesh,
                               const Field<Dim>& T) {
    std::ofstream out(filename);

    std::vector<const double*> cellData;
    std::vector<std::string> cellNames;
    cellData.push_back(const_cast<double*>(T.ptr()));
    cellNames.push_back("Temperature");

    if (mesh.isUnstructured()) {
        writeVTU_(out, mesh, cellData, cellNames, {}, {});
    } else if (mesh.isUniform()) {
        writeVTI_(out, mesh, cellData, cellNames, {}, {});
    } else {
        writeVTR_(out, mesh, cellData, cellNames, {}, {});
    }
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeWithVelocity(const std::string& filename,
                                           const Mesh<Dim>& mesh,
                                           const Field<Dim>& T,
                                           const std::array<const Field<Dim>*, Dim>& velocity,
                                           const std::string& scalarName,
                                           const Field<Dim>* k,
                                           const Field<Dim>* omega,
                                           const Field<Dim>* nut) {
    std::ofstream out(filename);

    std::size_t nCells = mesh.nCells();
    std::vector<double> vel3D(nCells * 3);
    for (std::size_t i = 0; i < nCells; ++i) {
        vel3D[i * 3 + 0] = (*velocity[0])[i];
        vel3D[i * 3 + 1] = (*velocity[1])[i];
        if constexpr (Dim == 3) {
            vel3D[i * 3 + 2] = (*velocity[2])[i];
        } else {
            vel3D[i * 3 + 2] = 0.0;
        }
    }

    std::vector<const double*> cellData;
    std::vector<std::string> cellNames;
    cellData.push_back(const_cast<double*>(T.ptr()));
    cellNames.push_back(scalarName);
    cellData.push_back(vel3D.data());
    cellNames.push_back("Velocity");
    if (k)     { cellData.push_back(const_cast<double*>(k->ptr()));     cellNames.push_back("k"); }
    if (omega) { cellData.push_back(const_cast<double*>(omega->ptr())); cellNames.push_back("omega"); }
    if (nut)   { cellData.push_back(const_cast<double*>(nut->ptr()));   cellNames.push_back("nut"); }

    if (mesh.isUnstructured()) {
        writeVTU_(out, mesh, cellData, cellNames, {}, {});
    } else if (mesh.isUniform()) {
        writeVTI_(out, mesh, cellData, cellNames, {}, {});
    } else {
        writeVTR_(out, mesh, cellData, cellNames, {}, {});
    }
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeTurbulence(const std::string& filename,
                                         const Mesh<Dim>& mesh,
                                         const Field<Dim>& k,
                                         const Field<Dim>& omega,
                                         const Field<Dim>& nut) {
    std::ofstream out(filename);

    std::vector<const double*> cellData;
    std::vector<std::string> cellNames;
    cellData.push_back(const_cast<double*>(k.ptr()));
    cellNames.push_back("k");
    cellData.push_back(const_cast<double*>(omega.ptr()));
    cellNames.push_back("omega");
    cellData.push_back(const_cast<double*>(nut.ptr()));
    cellNames.push_back("nut");

    if (mesh.isUnstructured()) {
        writeVTU_(out, mesh, cellData, cellNames, {}, {});
    } else if (mesh.isUniform()) {
        writeVTI_(out, mesh, cellData, cellNames, {}, {});
    } else {
        writeVTR_(out, mesh, cellData, cellNames, {}, {});
    }
}

template<std::size_t Dim>
std::vector<double> VTKXMLWriter<Dim>::cellToVertex(const Mesh<Dim>& mesh, const Field<Dim>& field) {
    if (mesh.isUnstructured()) {
        return cellToVertexUnstructured_(mesh, field);
    }
    return cellToVertexStructured_(mesh, field);
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writePointData(const std::string& filename,
                                        const Mesh<Dim>& mesh,
                                        const Field<Dim>& T,
                                        const std::string& scalarName,
                                        VTKInterpolator<Dim>* cache) {
    std::ofstream out(filename);

    const std::vector<double>* vertexValues = nullptr;
    std::vector<double> localValues;
    if (cache) {
        vertexValues = &cache->interpolate(T);
    } else {
        localValues = cellToVertex(mesh, T);
        vertexValues = &localValues;
    }

    std::vector<const double*> pointData;
    std::vector<std::string> pointNames;
    pointData.push_back(vertexValues->data());
    pointNames.push_back(scalarName);

    if (mesh.isUnstructured()) {
        writeVTU_(out, mesh, {}, {}, pointData, pointNames);
    } else if (mesh.isUniform()) {
        writeVTI_(out, mesh, {}, {}, pointData, pointNames);
    } else {
        writeVTR_(out, mesh, {}, {}, pointData, pointNames);
    }
}

template<std::size_t Dim>
void VTKXMLWriter<Dim>::writeWithVelocityPointData(const std::string& filename,
                                                    const Mesh<Dim>& mesh,
                                                    const Field<Dim>& T,
                                                    const std::array<const Field<Dim>*, Dim>& velocity,
                                                    const std::string& scalarName,
                                                    VTKInterpolator<Dim>* cache) {
    std::ofstream out(filename);

    const std::vector<double>* scalarVerts = nullptr;
    std::vector<double> localScalar;
    if (cache) {
        scalarVerts = &cache->interpolate(T);
    } else {
        localScalar = cellToVertex(mesh, T);
        scalarVerts = &localScalar;
    }

    std::vector<std::vector<double>> velVerts(Dim);
    for (std::size_t d = 0; d < Dim; ++d) {
        if (cache) {
            velVerts[d] = cache->interpolate(*velocity[d]);
        } else {
            velVerts[d] = cellToVertex(mesh, *velocity[d]);
        }
    }
    std::size_t nVerts = velVerts[0].size();

    std::vector<const double*> pointData;
    std::vector<std::string> pointNames;
    pointData.push_back(scalarVerts->data());
    pointNames.push_back(scalarName);

    std::vector<double> vel3D(nVerts * 3);
    for (std::size_t i = 0; i < nVerts; ++i) {
        vel3D[i * 3 + 0] = velVerts[0][i];
        vel3D[i * 3 + 1] = velVerts[1][i];
        if constexpr (Dim == 3) {
            vel3D[i * 3 + 2] = velVerts[2][i];
        } else {
            vel3D[i * 3 + 2] = 0.0;
        }
    }
    pointData.push_back(vel3D.data());
    pointNames.push_back("Velocity");

    if (mesh.isUnstructured()) {
        writeVTU_(out, mesh, {}, {}, pointData, pointNames);
    } else if (mesh.isUniform()) {
        writeVTI_(out, mesh, {}, {}, pointData, pointNames);
    } else {
        writeVTR_(out, mesh, {}, {}, pointData, pointNames);
    }
}

template<std::size_t Dim>
VTKInterpolator<Dim>::VTKInterpolator(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
const std::vector<double>& VTKInterpolator<Dim>::interpolate(const Field<Dim>& field) {
    if (lastFieldPtr_ == &field) {
        return cachedValues_;
    }
    cachedValues_ = VTKXMLWriter<Dim>::cellToVertex(mesh_, field);
    lastFieldPtr_ = &field;
    return cachedValues_;
}

template<std::size_t Dim>
void VTKInterpolator<Dim>::invalidate() {
    lastFieldPtr_ = nullptr;
    cachedValues_.clear();
}

template class VTKXMLWriter<2>;
template class VTKXMLWriter<3>;
template class VTKInterpolator<2>;
template class VTKInterpolator<3>;

} // namespace fvm
