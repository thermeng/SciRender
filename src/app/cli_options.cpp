#include "app/cli_options.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

namespace cli {

namespace {

constexpr std::size_t kMaxIterations = 1'000'000;

// Split "--name=value" into its name and value. "--name" yields an empty
// optional value, which the caller resolves by consuming the next argument.
struct Arg {
    std::string_view name;
    std::optional<std::string_view> inlineValue;
};

Arg splitArg(const std::string& arg) {
    const std::size_t eq = arg.find('=');
    if (eq == std::string::npos) return {arg, std::nullopt};
    return {std::string_view(arg).substr(0, eq),
            std::string_view(arg).substr(eq + 1)};
}

bool isKnownOption(std::string_view name) {
    return name == "--scfd" || name == "--iter" || name == "--verbosity" ||
           name == "--smoothed-aggregation" ||
           name == "--help" || name == "-h";
}

std::optional<std::string_view> takeValue(const Arg& arg, int& i, int argc,
                                          char** argv) {
    if (arg.inlineValue) return arg.inlineValue;
    if (i + 1 >= argc) return std::nullopt;
    ++i;
    return std::string_view(argv[i]);
}

bool parseIterations(std::string_view text, std::size_t& out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) return false;
    if (value < 1 || value > kMaxIterations) return false;
    out = value;
    return true;
}

std::optional<cfd::AmgSettings::Verbosity> parseVerbosity(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "low") return cfd::AmgSettings::Verbosity::Low;
    if (lower == "high") return cfd::AmgSettings::Verbosity::High;
    if (lower == "diagnosis") return cfd::AmgSettings::Verbosity::Diagnosis;
    return std::nullopt;
}

} // namespace

bool wantsCli(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (isKnownOption(splitArg(argv[i]).name)) return true;
    }
    return false;
}

std::optional<Options> parse(int argc, char** argv, std::string& error) {
    Options options;
    bool sawCase = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const Arg parsed = splitArg(arg);

        if (parsed.name == "--help" || parsed.name == "-h") {
            options.help = true;
            continue;
        }

        if (parsed.name == "--scfd") {
            if (sawCase) {
                error = "--scfd was given more than once.";
                return std::nullopt;
            }
            const auto value = takeValue(parsed, i, argc, argv);
            if (!value || value->empty()) {
                error = "--scfd requires a case file path.";
                return std::nullopt;
            }
            options.casePath = std::filesystem::path(*value);
            sawCase = true;
            continue;
        }

        if (parsed.name == "--iter") {
            const auto value = takeValue(parsed, i, argc, argv);
            if (!value) {
                error = "--iter requires a positive integer.";
                return std::nullopt;
            }
            std::size_t iterations = 0;
            if (!parseIterations(*value, iterations)) {
                error = "--iter must be an integer between 1 and 1000000.";
                return std::nullopt;
            }
            options.iterations = iterations;
            continue;
        }

        if (parsed.name == "--smoothed-aggregation") {
            options.smoothedAggregation = true;
            continue;
        }

        if (parsed.name == "--verbosity") {
            const auto value = takeValue(parsed, i, argc, argv);
            if (!value) {
                error = "--verbosity requires low, high or diagnosis.";
                return std::nullopt;
            }
            const auto verbosity = parseVerbosity(*value);
            if (!verbosity) {
                error = "--verbosity must be low, high or diagnosis.";
                return std::nullopt;
            }
            options.verbosity = *verbosity;
            continue;
        }

        error = "Unexpected argument: " + arg;
        return std::nullopt;
    }

    if (!options.help && !sawCase) {
        error = "--scfd is required in command-line mode.";
        return std::nullopt;
    }
    return options;
}

std::string usage() {
    return
        "Usage:\n"
        "  SciRender.exe --scfd <case.scfd> [--iter <n>] [--verbosity <level>]\n"
        "  SciRender.exe --help\n"
        "\n"
        "Options:\n"
        "  --scfd <path>        Load and solve this .scfd case headlessly.\n"
        "  --iter <n>           Steady: run exactly n iterations.\n"
        "                       Transient: run n time steps from the restart point.\n"
        "  --verbosity <level>  AMG verbosity: low, high or diagnosis.\n"
        "  --smoothed-aggregation  Enable smoothed prolongation (Smoothed\n"
        "                        Aggregation AMG).\n"
        "  --help, -h           Show this help.\n"
        "\n"
        "Without --scfd the GUI starts. CLI runs write no result files.\n";
}

} // namespace cli
