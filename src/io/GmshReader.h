#pragma once

#include "mesh/Mesh.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class GmshReader {
public:
    struct Result {
        Mesh<Dim> mesh;
        std::map<std::string, std::vector<std::size_t>> boundaryGroups;
    };

    static Result read(const std::string& filename);

    // Scans $Elements for element types and returns 3 when the file contains
    // volume elements, 2 otherwise. Only MSH 2.2 is supported, so a file that
    // read() would reject is rejected here with the same diagnostic.
    static int detectDimension(const std::string& filename);

private:
    using FaceKey = std::vector<std::size_t>;

    static FaceKey sortedFaceVerts(const std::vector<std::size_t>& verts);
    static void trim_(std::string& s);
    static std::string trimmed(const std::string& s);
    static void skipTo_(std::ifstream& in, const std::string& marker);
    static void parsePhysicalNames_(std::ifstream& in,
                                    std::map<int, std::pair<int, std::string>>& names);
    static void parseNodes_(std::ifstream& in,
                            std::vector<std::array<double, Dim>>& vertices,
                            std::map<int, std::size_t>& nodeTagMap);
    static std::vector<std::size_t> resolveNodeTags_(const std::string& line,
                                                     std::size_t& pos,
                                                     int count,
                                                     const std::map<int, std::size_t>& nodeTagMap);
    static void parseElements_(std::ifstream& in,
                               std::vector<std::vector<std::size_t>>& cells,
                               std::vector<int>& elemPhysTags,
                               std::vector<std::pair<FaceKey, int>>& boundaryFaces,
                               const std::map<int, std::size_t>& nodeTagMap);
    static std::map<std::string, std::vector<std::size_t>> extractBoundaryGroupsFromFaces_(
        const Mesh<Dim>& mesh,
        const std::vector<std::pair<FaceKey, int>>& boundaryFaces,
        const std::map<int, std::pair<int, std::string>>& physicalNames);
    static std::map<std::string, std::vector<std::size_t>> extractBoundaryGroupsFromCells_(
        const Mesh<Dim>& mesh,
        const std::vector<int>& elemPhysTags,
        const std::map<int, std::pair<int, std::string>>& physicalNames);
    static int parseInt_(const std::string& s, std::size_t& pos);
    static double parseDouble_(const std::string& s, std::size_t& pos);
    static void skipSpace_(const std::string& s, std::size_t& pos);
};

extern template class GmshReader<2>;
extern template class GmshReader<3>;

} // namespace fvm
