// CLI option parsing: the headless solver's only user-facing surface.
#include "app/cli_options.h"

#include "doctest/doctest.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

struct Invocation {
    std::vector<std::string> storage;
    std::vector<char*> argv;

    explicit Invocation(std::initializer_list<const char*> args) {
        storage.emplace_back("SciRender.exe");
        for (const char* arg : args) storage.emplace_back(arg);
        argv.reserve(storage.size());
        for (auto& arg : storage) argv.push_back(arg.data());
    }

    int argc() const { return static_cast<int>(argv.size()); }
};

struct ParseResult {
    std::optional<cli::Options> options;
    std::string error;
};

ParseResult parse(std::initializer_list<const char*> args) {
    Invocation invocation(args);
    ParseResult result;
    result.options = cli::parse(invocation.argc(), invocation.argv.data(), result.error);
    return result;
}

bool wantsCli(std::initializer_list<const char*> args) {
    Invocation invocation(args);
    return cli::wantsCli(invocation.argc(), invocation.argv.data());
}

} // namespace

TEST_SUITE("cli") {

TEST_CASE("cli.wants_cli_only_for_known_options") {
    CHECK(wantsCli({"--scfd", "case.scfd"}));
    CHECK(wantsCli({"--iter", "10"}));
    CHECK(wantsCli({"--verbosity", "low"}));
    CHECK(wantsCli({"--help"}));
    CHECK_FALSE(wantsCli({"mesh.vtu"}));
    CHECK_FALSE(wantsCli({"-style", "Fusion"}));
}

TEST_CASE("cli.parses_minimal_case") {
    const auto result = parse({"--scfd", "Case.scfd"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->casePath == std::filesystem::path("Case.scfd"));
    CHECK_FALSE(result.options->iterations.has_value());
    CHECK_FALSE(result.options->verbosity.has_value());
    CHECK_FALSE(result.options->help);
}

TEST_CASE("cli.parses_iterations_and_verbosity") {
    const auto result = parse(
        {"--scfd", "Case.scfd", "--iter", "10", "--verbosity", "diagnosis"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->casePath == std::filesystem::path("Case.scfd"));
    REQUIRE(result.options->iterations.has_value());
    CHECK(*result.options->iterations == 10);
    REQUIRE(result.options->verbosity.has_value());
    CHECK(*result.options->verbosity == cfd::AmgSettings::Verbosity::Diagnosis);
}

TEST_CASE("cli.accepts_equals_form_and_case_insensitive_verbosity") {
    const auto result = parse({"--scfd=Case.scfd", "--iter=3", "--verbosity=HIGH"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->casePath == std::filesystem::path("Case.scfd"));
    REQUIRE(result.options->iterations.has_value());
    CHECK(*result.options->iterations == 3);
    REQUIRE(result.options->verbosity.has_value());
    CHECK(*result.options->verbosity == cfd::AmgSettings::Verbosity::High);
}

TEST_CASE("cli.rejects_invalid_arguments") {
    CHECK_FALSE(parse({"--iter", "0", "--scfd", "Case.scfd"}).options.has_value());
    CHECK_FALSE(parse({"--iter", "abc", "--scfd", "Case.scfd"}).options.has_value());
    CHECK_FALSE(parse({"--iter", "--scfd", "Case.scfd"}).options.has_value());
    CHECK_FALSE(parse({"--scfd", "Case.scfd", "--verbosity", "quiet"}).options.has_value());
    CHECK_FALSE(parse({"--iter", "2"}).options.has_value());
    CHECK_FALSE(parse({"--scfd", "a.scfd", "--scfd", "b.scfd"}).options.has_value());
    CHECK_FALSE(parse({"Case.scfd"}).options.has_value());
}

TEST_CASE("cli.help_needs_no_case") {
    const auto result = parse({"--help"});
    REQUIRE(result.options.has_value());
    CHECK(result.options->help);
    CHECK(result.error.empty());
}

} // TEST_SUITE("cli")
