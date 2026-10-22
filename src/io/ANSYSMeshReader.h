#pragma once

#include "mesh/Mesh.h"
#include <array>
#include <cstddef>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class ANSYSMeshReader {
public:
    struct Result {
        Mesh<Dim> mesh;
        std::map<std::string, std::vector<std::size_t>> boundaryGroups;
    };

    static Result read(const std::string& filename);

private:
    struct Token {
        enum Kind { LParen, RParen, Integer, Real, String, End };
        Kind kind;
        std::string raw;
    };

    class Tokenizer {
    public:
        explicit Tokenizer(std::ifstream& in) : in_(in) {}

        Token next();
        Token peek();

    private:
        std::ifstream& in_;
        Token buffer_{Token::End, ""};
        bool hasBuffer_ = false;

        Token readToken();
        static Token classify(const std::string& word);
    };

    struct Section {
        int code = -1;
        std::vector<Section> children;
        std::vector<std::string> values;
    };

    struct FaceRecord {
        std::vector<std::size_t> vertices;
        int rightCell = 0;
        int leftCell = 0;
        int zoneId = 0;
    };

    struct CellZone {
        int zoneId = 0;
        std::size_t firstIndex = 0;
        std::size_t lastIndex = 0;
        int zoneType = 0;
        int elementType = 0;
        std::vector<int> perCellTypes;
    };

    struct ParseState {
        std::vector<std::array<double, Dim>> vertices;
        std::vector<FaceRecord> faces;
        std::vector<CellZone> cellZones;
        std::map<int, std::string> zoneNames;
        std::map<int, std::string> zoneTypes;
        std::map<int, int> faceZoneTypes;
        std::size_t totalCells = 0;
    };

    static Section parseGroup_(Tokenizer& tok);
    static Section parseNode(Tokenizer& tok);
    static void handleSection(const Section& sec, ParseState& state);

    static std::vector<std::vector<std::size_t>> reconstructCells(
        const std::vector<FaceRecord>& faces,
        std::size_t cellCount);

    // Builds, for every cell, the ordered vertex loop of each of its faces.
    // The ANSYS face records carry the face nodes in order around the face, so
    // the loops can be handed to the mesh as an explicit topology. Without it
    // the mesh has to infer the topology from the cell vertex count, which is
    // wrong for every element whose nodes are not in VTK/Gmsh order.
    static std::vector<std::vector<std::vector<std::size_t>>> reconstructCellFaces(
        const std::vector<FaceRecord>& faces,
        std::size_t cellCount);

    static std::map<std::string, std::vector<std::size_t>> extractBoundaryGroups(
        const Mesh<Dim>& mesh,
        const std::vector<FaceRecord>& faces,
        const std::map<int, std::string>& zoneNames,
        const std::map<int, std::string>& zoneTypes,
        const std::map<int, int>& faceZoneTypes);

    static std::size_t parseHex(const std::string& s);
    static double parseDouble(const std::string& s);
    static int parseInt(const std::string& s);
};

extern template class ANSYSMeshReader<2>;
extern template class ANSYSMeshReader<3>;

} // namespace fvm
