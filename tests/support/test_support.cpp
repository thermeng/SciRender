#include "support/test_support.h"

#include "doctest/doctest.h"

#include <atomic>
#include <cstdio>

namespace test_support {
namespace {

std::atomic<int> g_tempDirCounter{0};

} // namespace

std::filesystem::path samplesDir() {
    return std::filesystem::path(SCIRENDER_SAMPLES_DIR);
}

std::filesystem::path samplePath(const std::string& relative) {
    return samplesDir() / std::filesystem::path(relative);
}

void requireSample(const std::string& relative) {
    const std::filesystem::path path = samplePath(relative);
    REQUIRE_MESSAGE(std::filesystem::exists(path),
                    "required sample fixture is missing: " << path.string());
}

TempDir::TempDir(const std::string& name) {
    const std::filesystem::path root(SCIRENDER_TEST_TMP_DIR);
    const int index = g_tempDirCounter.fetch_add(1);
    m_path = root / (name + "-" + std::to_string(index));
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
    std::filesystem::create_directories(m_path, ec);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
}

} // namespace test_support
