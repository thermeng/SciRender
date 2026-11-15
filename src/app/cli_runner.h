#pragma once

#include "app/cli_options.h"

namespace cli {

// Runs one prepared case to completion on the calling thread's event loop.
// A QCoreApplication must already exist. Returns the process exit code.
int runHeadlessCase(const Options& options);

} // namespace cli
