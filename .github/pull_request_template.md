## What and why

<!-- What changes, and the reason. Link the issue, ADR or proposal. -->

## How it was verified

<!-- CI runs the first two. Tick what applies, and delete the rest. -->

- [ ] `docker build --target runtime .` passes: unit tests, end-to-end
      suite, h2spec 146/146 and fuzzing
- [ ] `KVASM_REQUIRE_DOCKER=1 dotnet test --solution KvAsm.Client.slnx`
      (run from `client/dotnet`) passes
- [ ] Protocol change (`h2n`, `hpack`, `grpc` or `proto`): fuzz soak
      `make itest FUZZ_ITERATIONS=100000 FUZZ_SEED=…` passed (seed: )
- [ ] Contract change (`kvasm.proto`): the Go example's stubs regenerated
      (`go generate` in `examples/go`) and committed
- [ ] Bug fix: the new test fails when the fix is reverted (mutation check)
- [ ] Performance change: paired A/B, ≥ 6 blocks, with the sign test, the
      outside load recorded, and the results committed under
      `benchmarks/results/`
- [ ] Docs updated: ADR, OPERATIONS/PROTOCOL, CHANGELOG, and
      `docs/CLAIMS.md` if a published number moved

## Notes for the reviewer

<!-- Risky spots, ownership/lifetime arguments, what you were unsure of. -->
