// yaml.hpp -- a small YAML parser, enough for job-file frontmatter.
//
// There is no YAML library on this machine and pulling one in for frontmatter
// would be disproportionate, so this handles the subset that frontmatter
// actually uses:
//
//   key: value                  scalars, with type inference
//   key:                        nested block mappings, by indentation
//     nested: value
//   key:                        block sequences
//     - one
//     - two
//   key: [a, b]                 flow sequences
//   key: {a: 1, b: 2}           flow mappings
//   - name: x                   a mapping that starts on the dash line
//     value: y
//   key: |                      literal block scalars (and |-, >, >-)
//     verbatim
//   # comments
//
// Not supported, and rejected rather than silently misread: anchors/aliases,
// tags, multiple documents, complex keys, and folded-flow hybrids.
#pragma once

#include "common.hpp"

namespace ppcode::yaml {

// Parse YAML text into JSON. Returns false with a line-numbered message on
// failure.
bool parse(const std::string& text, json* out, std::string* error);

// Split "---\nfrontmatter\n---\nbody" into its parts. If the text does not
// begin with a frontmatter fence, `front` is empty and everything is `body`.
// Returns false only for a fence that is opened and never closed.
bool split_frontmatter(const std::string& text, std::string* front,
                       std::string* body, std::string* error);

// Convert a scalar string to bool/int/double/null/string the way YAML would.
json infer_scalar(const std::string& s);

} // namespace ppcode::yaml
