"""REFRACT axes: per-axis scoring modules.

Status (v0.1.4):

  - gtm:        Greedy Trajectory Match — DEPRECATED, retained for
                backward compatibility. Has a known detokenize→retokenize
                round-trip artifact (LIMITATIONS.md §1, §5) that v0.1.3
                bounded but did not eliminate.
  - trajectory: NEW v0.1.4 replacement for gtm. Captures token IDs at
                decode time via the patched ``llama-completion`` binary,
                so the comparison runs in true model-token units with no
                round-trip. Same result shape as gtm; drop-in upgrade.
  - kld:        KL Divergence vs fp16-KV reference (corpus proxy). v0.1
                approximation; v0.2 plan is per-step trajectory-KLD on the
                same forward pass that emits trajectory token IDs.

Skeletons (v0.2 axes; importable but ``run_*`` raises ``NotImplementedError``):

  - rniah:      Retrieval Needle-In-A-Haystack — long-context degradation
                surface. Designed to catch quants that score 99 on KLD@D
                and still fail at 32K+ context.
  - plad:       Perturbation-Locality Aware Drift — quant brittleness
                under semantically irrelevant prompt perturbations
                (typo / case / punct / paraphrase).

See module docstrings for the per-axis protocols and the v0.2 README for
roadmap and dependencies.
"""

from __future__ import annotations
