# Periodic clock tracking

Approved configuration: initial rate fit over 10 seconds, subsequent collective
short samples approximately every 60 seconds, confirmed 1000 ns residual trigger,
rate gain 0.25, phase re-anchoring, and persisted samples/models after each call.
Use physical devices 0–13 for hardware validation, mapping logical rank 0 to
physical device 4. Clock correction always uses logical rank 0 as its reference.

1. Extend `src/diagnostics/host/clock_drift.*` with opt-in feedback after initial
   training. Confirm using two consecutive sample groups. Compute elapsed time
   from device timestamps since the last accepted model update. Leave frozen
   behavior available and preserve C++14/public structure layouts.
2. Integrate candidate model gathering/publication and per-rank persistence in
   `straggler_session.cpp`. Match tracking configuration in collective preflight.
   Reuse existing owner-thread, quiescence and communicator rules; no background
   sampler. Extend the existing integration runner with separate training and
   maintenance cadence and output sample/model/update records.
3. Verify feedback sign, phase correction, threshold confirmation, elapsed-time
   anchoring and disabled behavior with focused CPU tests. Run session regression
   and build checks. Review/simplify, commit and push the task branch without a PR.
   Green fetches the exact candidate, rebuilds in the existing approved deployment,
   reuses its 14-device HCCL qualification, and executes the new cadence for at
   least 300 seconds after initial training. Check measured call latency, causal
   pre-update residuals, actual model updates, persisted files, and device cleanup.

Feedback changes corrected timestamps at the update boundary. Samples and model
generations must be retained to identify that boundary. The 1 us trigger does
not guarantee every sample or unobserved instant remains below 1 us. Four-timestamp
estimates retain the symmetric-delay assumption. Do not add checkpoint loading,
an unrelated scheduling service, or a new communication transport.
