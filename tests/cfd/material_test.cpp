// Material model: built-in reference values and the nu = mu/rho derivation
// the solver relies on.
#include "doctest/doctest.h"
#include "cfd/material.h"

TEST_SUITE("cfd") {

TEST_CASE("cfd.material.builtin_reference_values") {
    const auto air = cfd::materials::air();
    CHECK(air.name == "Air");
    CHECK(air.rho == doctest::Approx(1.204));
    CHECK(air.mu == doctest::Approx(1.825e-5));
    CHECK(air.nu() == doctest::Approx(1.825e-5 / 1.204));

    const auto water = cfd::materials::water();
    CHECK(water.name == "Water");
    CHECK(water.rho == doctest::Approx(998.2));
    CHECK(water.mu == doctest::Approx(1.002e-3));
    // Water at 20 C: nu ~ 1.004e-6 m^2/s.
    CHECK(water.nu() == doctest::Approx(1.0038e-6).epsilon(1e-3));

    CHECK(cfd::materials::builtIns().size() == 2);
}

TEST_CASE("cfd.material.custom_nu_derivation") {
    cfd::Material oil{"Oil", 900.0, 0.09};
    CHECK(oil.nu() == doctest::Approx(1e-4));
}

} // TEST_SUITE("cfd")
