#pragma once

namespace ppcode {
// Runs the internal checks. `with_network` adds live calls to OpenRouter.
// Returns the number of failures.
int run_selftest(bool with_network);
} // namespace ppcode
