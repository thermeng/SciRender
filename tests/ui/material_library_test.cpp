// User material library (QSettings persistence) and the case-codec material
// field, including backward compatibility with pre-material case files.
#include "doctest/doctest.h"
#include "ui/cfd_material_library.h"
#include "cfd/case_codec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
// Throwaway registry branch so tests never touch the user's real library.
cfdUi::MaterialLibrary testLib() {
    QSettings s(QStringLiteral("SciRenderTest"), QStringLiteral("MaterialLibTest"));
    s.clear();
    return cfdUi::MaterialLibrary(QStringLiteral("SciRenderTest"),
                                  QStringLiteral("MaterialLibTest"));
}
} // namespace

TEST_SUITE("ui") {

TEST_CASE("ui.material_library.save_find_remove_roundtrip") {
    auto lib = testLib();
    CHECK(lib.userMaterials().empty());

    cfd::Material oil{"Oil", 900.0, 0.09};
    REQUIRE(lib.saveUserMaterial(oil));
    auto found = lib.find("Oil");
    REQUIRE(found);
    CHECK(found->rho == doctest::Approx(900.0));
    CHECK(found->nu() == doctest::Approx(1e-4));
    CHECK(lib.find("oil").has_value()); // case-insensitive lookup

    // Overwrite by name, not duplicate.
    REQUIRE(lib.saveUserMaterial(cfd::Material{"Oil", 910.0, 0.1}));
    CHECK(lib.userMaterials().size() == 1);
    CHECK(lib.find("Oil")->rho == doctest::Approx(910.0));

    // Built-ins resolve and cannot be overwritten; invalid entries rejected.
    CHECK(lib.find("Water").has_value());
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"Water", 1.0, 1.0}));
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"air", 1.0, 1.0}));
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"Custom", 1.0, 1.0}));
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"", 1.0, 1.0}));
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"Bad", -1.0, 1.0}));
    CHECK_FALSE(lib.saveUserMaterial(cfd::Material{"Bad", 1.0, 0.0}));
    CHECK(lib.find("Water")->rho == doctest::Approx(998.2));
    CHECK(lib.allMaterials().size() == 3); // Air, Water, Oil

    CHECK(lib.removeUserMaterial("Oil"));
    CHECK_FALSE(lib.find("Oil").has_value());
    CHECK_FALSE(lib.removeUserMaterial("Oil"));
    CHECK(lib.userMaterials().empty());

    QSettings s(QStringLiteral("SciRenderTest"), QStringLiteral("MaterialLibTest"));
    s.clear();
}

TEST_CASE("ui.material_codec_roundtrip_and_backward_compat") {
    cfd::CaseSettings s;
    s.materialName = "Water";
    s.rho = 998.2;
    s.nu = 1.002e-3 / 998.2;
    std::vector<cfd::PatchBC> bcs;
    const QByteArray json = cfd::serializeCaseSettings(s, bcs);

    cfd::CaseSettings back;
    std::vector<cfd::PatchBC> backBcs;
    REQUIRE(cfd::deserializeCaseSettings(json, back, backBcs));
    CHECK(back.materialName == "Water");
    CHECK(back.rho == doctest::Approx(998.2));
    CHECK(back.nu == doctest::Approx(1.002e-3 / 998.2));

    // Pre-material file: no materialName key -> the default (empty = custom)
    // survives, so old cases load unchanged.
    QJsonObject obj = QJsonDocument::fromJson(json).object();
    obj.remove("materialName");
    cfd::CaseSettings legacy;
    REQUIRE(cfd::deserializeCaseSettings(QJsonDocument(obj).toJson(), legacy, backBcs));
    CHECK(legacy.materialName.empty());
    CHECK(legacy.rho == doctest::Approx(998.2));
}

} // TEST_SUITE("ui")
