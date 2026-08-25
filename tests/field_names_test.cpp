// Headless check for FieldResolver's vector-name listing rules: the point and
// cell vector name lists must merge WITHOUT duplicates — a cell-only field
// also exists as the extrapolated point copy under the same name
// (extrapolateCellDataToPoints), so the raw lists commonly overlap. No Qt/GL.
#include "core/FieldResolver.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static bool sameList(const std::vector<std::string>& a,
                     const std::vector<std::string>& b) {
    return a == b;
}

using FieldResolver::availableVectorNames;
using FieldResolver::derivedScalarNames;

// The reported bug: one CELL_DATA vector "velocity" appears in both the
// extrapolated-point list and the cell list -> combo showed it twice.
static void testOverlappingNameDeduplicates() {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity"};
    mesh.availableCellVectorNames = {"velocity"};
    CHECK(sameList(availableVectorNames(mesh), {"velocity"}));
}

// Distinct point-only and cell-only names survive, merged and sorted.
static void testDistinctNamesMergeSorted() {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity", "alpha"};
    mesh.availableCellVectorNames = {"velocity", "beta"};
    CHECK(sameList(availableVectorNames(mesh), {"alpha", "beta", "velocity"}));
}

// Derived scalar fields (_magnitude/_X/_Y/_Z) are generated once per UNIQUE
// vector name, not per raw list entry.
static void testDerivedScalarsGeneratedOncePerUniqueName() {
    RenderMesh mesh;
    mesh.availableVectorNames = {"velocity"};
    mesh.availableCellVectorNames = {"velocity"};
    auto derived = derivedScalarNames(mesh);
    CHECK(derived.size() == 4);
    // Two unique names -> exactly 8 derived entries.
    mesh.availableCellVectorNames = {"velocity", "wind"};
    derived = derivedScalarNames(mesh);
    CHECK(derived.size() == 8);
}

int main() {
    testOverlappingNameDeduplicates();
    testDistinctNamesMergeSorted();
    testDerivedScalarsGeneratedOncePerUniqueName();
    if (g_failures == 0) {
        std::printf("field_names_test: all checks passed\n");
        return 0;
    }
    std::printf("field_names_test: %d failure(s)\n", g_failures);
    return 1;
}
