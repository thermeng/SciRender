#pragma once

#include "core/mesh_loader.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace test_support {

using VelocityFn = std::function<glm::vec3(float, float, float)>;
using PressureFn = std::function<float(float, float, float)>;

// Cartesian grid mesh with analytic velocity/pressure sampled at the nodes.
// xs/ys/zs give (possibly non-uniform) axis coordinates.
RenderMesh makeFlowGrid(const std::vector<float>& xs,
                        const std::vector<float>& ys,
                        const std::vector<float>& zs,
                        VelocityFn vel,
                        PressureFn pres,
                        const std::string& vName = "velocity",
                        const std::string& pName = "pressure");

// n evenly spaced samples from a to b (inclusive).
std::vector<float> linspace(float a, float b, int n);

float maxAbs(const std::vector<float>& v);
float maxVecAbs(const std::vector<glm::vec3>& v);

} // namespace test_support
