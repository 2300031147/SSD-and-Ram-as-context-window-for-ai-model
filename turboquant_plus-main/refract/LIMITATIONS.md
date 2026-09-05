# REFRACT v0.1.3 — Known Limitations

REFRACT v0.1.3 is a defensive release. It does not add new functional
axes; it fixes the bugs the v0.1.2 matrix exposed, removes a silent
fallback that was producing wrong-unit scores, and adds the regression
test coverage that earlier versions lacked.

The limitations below are the ones the codex review surfaced and that we
are explicitly choosing to ship with. Each one has a v0.2 plan.

## 1. Detokenize → retokenize gap

GTM tokenization comes from re-tokenizing the *decoded stdout text* of
`llama-cli`, not from the model's actual decoded token IDs.

- Detokenization can collapse special tokens (BOS/EOS/role markers) and
  whitespace.
- Retokenizing the resulting text can produce a different sequence than
  the model originally emitted — sometimes 2-3× as many tokens.
- v0.1.3 mitigates the *score* impact by normalizing by
  `mean_cand_length` rather than `n_predict`, so the score stays bounded
  and meaningful even when retokenization inflates. But the underlying
  comparison is still text-based, not ID-based.

**v0.2 plan:** capture token IDs at decode time via a custom
llama.cpp binary that dumps both per-step logits and the chosen ID. This
also unlocks true trajectory-KLD (the v0.2 KLD axis), so the two
problems get fixed together.

## 2. Corpus-anchored KLD

The KLD axis uses `llama-perplexity --kl-divergence`'s corpus-KLD
machinery against a reference base file built from the same corpus.

- Rankings between configurations on the same corpus are reliable —
  this is what the paper §4.3 uses and what we re-validated.
- **Absolute KLD magnitudes depend on corpus choice.** A 1.7-nat
  result on wikitext-2 is not directly comparable to a 1.7-nat result
  on a code corpus.
- v0.1.3 records corpus identity (path, size, first-MiB SHA) in the
  output JSON and refuses to score against a base file built from a
  different corpus, so cross-corpus accidents are caught at runtime.

**v0.2 plan:** trajectory KLD computed at decode time on the same
prompts the GTM axis uses, eliminating the corpus dependence entirely.

## 3. Bands are provisional

The 95 / 80 / 60 thresholds (EXCELLENT / PASS / DEGRADED / FAIL) were
calibrated against a single matrix on wikitext-2 with limited model
coverage (7 models, M5 Max only). Expect re-calibration in v0.2 once
trajectory KLD changes the absolute KLD scale.

The harmonic-mean composite formula and the band names are stable; the
numeric thresholds are not.

## 4. Single platform tested

REFRACT has only been validated on Apple Silicon Metal builds (M5 Max).
The `--measure-floor` step relies on the bit-exact-zero KLD noise floor
that Metal exhibits on the `llama-perplexity` codepath (paper §4.5).
CUDA and HIP builds may have a non-zero noise floor; users on those
platforms should re-measure with `--measure-floor` and treat any non-zero
floor as their working noise threshold rather than assuming 99.5 is
achievable.

## 5. GTM conflates divergence with EOS truncation

A model that emits EOS at token 5 and a model that diverges at token 5
both produce `prefix_agreement_length = 5`. GTM treats them identically
even though the failure modes are completely different.

In practice this rarely matters for KV-quantization comparisons (both
candidate and reference tend to either both stop short or both keep
going), but it's a genuine ambiguity in the metric.

**v0.2 plan:** distinguish "EOS at position k" from "wrong token at
position k" by recording the per-step token IDs and a per-step `is_eos`
flag.

## 6. Whitespace fallback removed (v0.1.3 fail-loud change)

Earlier versions had a silent `try/except` that fell back to
whitespace tokenization if `llama-tokenize` raised. This produced
scores in mixed units (whitespace tokens vs model tokens) that the
caller had no way to detect.

v0.1.3 removes this fallback entirely. If the tokenizer subprocess
fails — for any reason: the binary is missing, the model is
unsupported, or the input is malformed — the GTM axis raises a
`RuntimeError` with the original tokenizer error attached. This is
intentional: a wrong score is worse than no score.

If you actually need a whitespace-only run for debugging, call
`_tokenize_words` directly; it remains exported as a stable utility
(it just is not used by the diff path anymore).
