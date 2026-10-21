// Headless check for FieldResolver's vector-name listing rules: the point and
// cell vector name lists must merge WITHOUT duplicates - a cell-only field
// also exists as the extrapolated point copy under the same name
// (extrapolateCellDataToPoints), so the raw lists commonly overlap. No Qt/GL.
#include "core/FieldResolver.h"
#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace {

bool sameList(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return a == b;
}

using FieldResolver::availableVectorNames;
using FieldResolver::derivedScalarNames;

} // namespace

TEST_SUITE("core") {

// The reported bug: one CELL_DATA vector "velocity" appears in both the
// extrapolated-point list and the cell list -> combo showed it twice.
TEST_CASE("core.fields.overlapping_names_deduplicate") {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity"};
    mesh.availableCellVectorNames = {"velocity"};
    CHECK_MESSAGE(sameList(availableVectorNames(mesh), {"velocity"}),
                  "overlapping point/cell name must appear once");
}

// Distinct point-only and cell-only names survive, merged and sorted.
TEST_CASE("core.fields.distinct_names_merge_sorted") {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity", "alpha"};
    mesh.availableCellVectorNames = {"velocity", "beta"};
    CHECK_MESSAGE(sameList(availableVectorNames(mesh), {"alpha", "beta", "velocity"}),
                  "distinct point/cell names merge sorted");
}

// Derived scalar fields (_magnitude/_X/_Y/_Z) are generated once per UNIQUE
// vector name, not per raw list entry.
TEST_CASE("core.fields.derived_scalars_once_per_unique_name") {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity"};
    mesh.availableCellVectorNames = {"velocity"};
    auto derived = derivedScalarNames(mesh);
    CHECK_MESSAGE(derived.size() == 4, "one unique name -> four derived scalars");
    // Two unique names -> exactly 8 derived entries.
    mesh.availableCellVectorNames = {"velocity", "wind"};
    derived = derivedScalarNames(mesh);
    CHECK_MESSAGE(derived.size() == 8, "two unique names -> eight derived scalars");
}

} // TEST_SUITE("core")
