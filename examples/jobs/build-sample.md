---
name: build-sample
description: Build the Sample Xcode project and report any warnings.

# Which model, and how OpenRouter should route to it.
model: anthropic/claude-sonnet-5
models:                       # tried in order if the primary is unavailable
  - z-ai/glm-5.2
  - deepseek/deepseek-v4-pro

provider:
  sort: throughput            # price | throughput | latency
  allow_fallbacks: true
  data_collection: deny
  # order: [anthropic, google-vertex]
  # only:  [anthropic]
  # ignore: [some-provider]
  # require_parameters: true
  # quantizations: [fp16, bf16]

reasoning:
  effort: medium              # minimal | low | medium | high

temperature: 0.2
max_tokens: 8192
max_turns: 25

# Machine context. "full" spends the most tokens describing this host and its
# toolchain; drop to brief/minimal for small-context models.
environment:
  detail: full
  knowledge: true

# Only these tools may mutate anything. Read-only tools always work.
tools:
  allow:
    - bash
    - run_background
    - job_output

output: text
---

Build the Xcode project at `~/Desktop/Sample`.

1. Run `xcodebuild -configuration Debug` in that directory.
2. If the build succeeds, report the path of the produced binary and its
   architecture (use `file`).
3. If there are any compiler warnings, list them with the file and line, but do
   **not** change any source files — this job is read-only apart from building.

Keep the summary to a few lines.
