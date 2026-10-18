#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class VTKInterpolator {
public:
    explicit VTKInterpolator(const Mesh<Dim>& mesh);

    const std::vector<double>& interpolate(const Field<Dim>& field);
    void invalidate();

private:
    const Mesh<Dim>& mesh_;
    std::vector<double> cachedValues_;
    const void* lastFieldPtr_ = nullptr;
};

template<std::size_t Dim>
class VTKXMLWriter {
public:
    static void write(const std::string& filename,
                      const Mesh<Dim>& mesh,
                      const Field<Dim>& T);

    static void writeWithVelocity(const std::string& filename,
                                  const Mesh<Dim>& mesh,
                                  const Field<Dim>& T,
                                  const std::array<const Field<Dim>*, Dim>& velocity,
                                  const std::string& scalarName = "pressure",
                                  const Field<Dim>* k = nullptr,
                                  const Field<Dim>* omega = nullptr,
                                  const Field<Dim>* nut = nullptr);

    static void writeTurbulence(const std::string& filename,
                                const Mesh<Dim>& mesh,
                                const Field<Dim>& k,
                                const Field<Dim>& omega,
                                const Field<Dim>& nut);

    static std::vector<double> cellToVertex(const Mesh<Dim>& mesh, const Field<Dim>& field);

    static void writePointData(const std::string& filename,
                               const Mesh<Dim>& mesh,
                               const Field<Dim>& T,
                               const std::string& scalarName = "Temperature",
                               VTKInterpolator<Dim>* cache = nullptr);

    static void writeWithVelocityPointData(const std::string& filename,
                                           const Mesh<Dim>& mesh,
                                           const Field<Dim>& T,
                                           const std::array<const Field<Dim>*, Dim>& velocity,
                                           const std::string& scalarName = "Temperature",
                                           VTKInterpolator<Dim>* cache = nullptr);

private:
    static std::string xmlEscape_(const std::string& s);

    static void writeScalars_(std::ostream& out, const std::string& name,
                               const double* data, std::size_t count,
                               std::size_t numComponents);
    static void writePoints_(std::ostream& out, const Mesh<Dim>& mesh);
    static void writeCells_(std::ostream& out, const Mesh<Dim>& mesh);

    static void writeVTI_(std::ostream& out, const Mesh<Dim>& mesh,
                          const std::vector<const double*>& cellData,
                          const std::vector<std::string>& cellNames,
                          const std::vector<const double*>& pointData,
                          const std::vector<std::string>& pointNames);
    static void writeVTR_(std::ostream& out, const Mesh<Dim>& mesh,
                          const std::vector<const double*>& cellData,
                          const std::vector<std::string>& cellNames,
                          const std::vector<const double*>& pointData,
                          const std::vector<std::string>& pointNames);
    static void writeVTU_(std::ostream& out, const Mesh<Dim>& mesh,
                          const std::vector<const double*>& cellData,
                          const std::vector<std::string>& cellNames,
                          const std::vector<const double*>& pointData,
                          const std::vector<std::string>& pointNames);

    static std::vector<double> cellToVertexStructured_(const Mesh<Dim>& mesh,
                                                        const Field<Dim>& field);
    static std::vector<double> cellToVertexUnstructured_(const Mesh<Dim>& mesh,
                                                          const Field<Dim>& field);
};

extern template class VTKXMLWriter<2>;
extern template class VTKXMLWriter<3>;
extern template class VTKInterpolator<2>;
extern template class VTKInterpolator<3>;

} // namespace fvm
