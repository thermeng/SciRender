#pragma once

#include <filesystem>
#include <string>

namespace test_support {

// Root of the repository's samples/ fixture tree.
// Resolved at compile time so tests never depend on the current directory.
std::filesystem::path samplesDir();

// Absolute path of a fixture inside samples/.
std::filesystem::path samplePath(const std::string& relative);

// Fails the current test case when the fixture is missing.
// Every fixture the suite needs is required: a missing file is a failure,
// never a silent skip.
void requireSample(const std::string& relative);

// Scratch directory for tests that write files.
// Created on construction, removed on destruction. Always lives under the
// build tree, so the samples/ fixture tree stays read-only.
class TempDir {
public:
    explicit TempDir(const std::string& name);
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    std::filesystem::path file(const std::string& name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
};

} // namespace test_support
