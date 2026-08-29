# TileXR CI Operations

This runbook covers the pull-request gate for `LingquLab/TileXR`. Repository
changes alone do not activate the gate: the maintainer team, Actions policy,
runner group, `blue` installation, trial runs, and required check are separate
operator steps. Do not make `PR Gate` required until a real successful check
run has supplied its GitHub Actions integration ID.

## Workflow and trust model

[`pr-ci.yml`](../.github/workflows/pr-ci.yml) runs for `pull_request` events
targeting `main`. It runs `Host Checks` in an Ubuntu 20.04 container, calls the
copy of [`npu-ci.yml`](../.github/workflows/npu-ci.yml) on `main` for a ready,
non-draft pull request, and reports the stable aggregate check `PR Gate`. A
draft runs only host checks. A newer commit, conversion to draft, or PR closure
cancels the older run for the same pull request. The replacement run for a
closed pull request performs no test jobs.

The reusable NPU workflow is the only workflow allowed to use the `TileXR-NPU`
organization runner group. It checks out
`refs/pull/NUMBER/merge` below the runner workspace and verifies its SHA before
the administrator-owned controller runs. The merge checkout is untrusted; the
controller and CANN toolchain are sealed, read-only installations outside it.
The check result belongs to that merge commit; a newer commit cancels the old
run and must produce its own successful `PR Gate` result.

Workflow permissions are `contents: read`. Checkout credentials are not
persisted, no repository or environment secret is exposed, and
`pull_request_target` is not used. The workflow does not pass `github.token` to
the controller. Untrusted child phases do not inherit Actions runtime or OIDC
tokens, or GitHub command-file paths. Before any pull-request code runs, the
Linux controller makes itself non-dumpable so same-account child processes
cannot read the controller's inherited environment through `/proc`.

This is defense in depth, not a sandbox. Pull-request code is built and run as
`tilexr-ci` with NPU access. Approval of an external-fork workflow explicitly
authorizes the reviewed merge commit to execute on the shared host. The
checkout is not trusted as controller code, but authorized developers are
trusted not to attack the persistent runner. The gate does not prevent a
deliberate write outside the checkout. Build steps may use NPUs; hardware
execution still waits for all eight devices to become idle and stops on a
foreign process collision.

## Blue layout

| Purpose | Account, group, or path |
| --- | --- |
| Service account | `tilexr-ci`, primary group `tilexr-ci`, device group `HwHiAiUser` |
| Home | `/home/tilexr-ci` |
| Sealed CANN 9.1 | `/home/tilexr-ci/toolchains/cann/9.1.0` |
| Sealed Bisheng compiler | `/home/tilexr-ci/toolchains/cann/9.1.0/cann/tools/bisheng_compiler/bin/bisheng` |
| Sealed controller | `/home/tilexr-ci/control/v4` |
| Active controller link | `/home/tilexr-ci/control/current` |
| Actions runner | `/home/tilexr-ci/actions-runner` |
| Runner workspace root | `/home/tilexr-ci/actions-runner/_work` |
| Fixed repository workspace | `/home/tilexr-ci/actions-runner/_work/TileXR/TileXR` |
| Provisioning staging | `/home/tilexr-ci/install-work` |

The account has a locked password, `nologin` shell, no SSH key, no sudo rule,
and no Docker membership. The
[`job_completed.sh`](../scripts/ci/control/job_completed.sh) hook is installed
as `/home/tilexr-ci/control/current/job_completed.sh`; its path is stored in the
root-owned runner `.env`. The hook removes children of the fixed repository
workspace only after Actions post-job steps complete. It refuses symbolic-link
redirection and never removes the CANN tree.

The root-owned CANN parent directories and installed toolchain use the
installer-required `0755` directory permissions. Toolchain files are readable
and executable but remain non-writable to the CI account and other users.

Host checks use `.ci-build/tilexr-host-default` by default. A
`TILEXR_CI_BUILD_ROOT` override is accepted only as an appropriately named
direct child of the real repository build parent or the real `RUNNER_TEMP`.
The NPU workflow uses `${RUNNER_TEMP}/tilexr-ci-scratch` for temporary build and
test logs. Neither workflow uploads artifacts. The trusted controller reports
its result through its exit code and the Actions log; it never writes a GitHub
step summary.

## Time and resource policy

The single self-hosted runner provides the cross-PR queue. The build runs before
waiting for NPUs and has a two-hour timeout. Build steps may use NPUs. Idle
waiting has a six-hour budget. Devices 0 through 7 must be healthy and
process-free for two consecutive samples 60 seconds apart.

Hardware execution has a two-hour total timeout; each multi-rank launch is
bounded to ten minutes. A foreign NPU process observed after acquisition stops
only the tracked CI process tree and reports a collision. CI never calls
`npu-smi release` and never signals another account's process. The complete NPU
job has an 11-hour Actions timeout to cover build, queueing, hardware, and
cleanup.

## Test matrix

[`host_checks.sh`](../scripts/ci/host_checks.sh) runs these seven recorded cases:

- shell syntax for tracked CI and affected test scripts;
- the complete standalone `tests/ci` CTest suite;
- comm logging and spdlog compile binaries;
- EP host tests;
- the data-as-flag header compile test;
- collectives vLLM patch tests;
- collectives profile-report tests.

On `blue`, [`build_blue.sh`](../scripts/ci/control/build_blue.sh) performs a
clean CANN 9.1 top-level configure, build, install, and CTest run; builds the
comm, UDMA, SDMA, EP, and memory test trees; requires the hardware demo
binaries; and validates installed headers, libraries, dependencies, and
RPATH/RUNPATH. Stub `libascend_hal.so` resolution through CANN `devlib` is a
failure.

The build phase logs these named cases:

- top level: `top-level-ctest`;
- comm: `comm-log` and `comm-spdlog-compile`;
- UDMA: `udma-transport-layout` and `udma-registry`;
- SDMA: `sdma-metadata`, `sdma-api-invalid`, `sdma-transport-disabled`,
  and `sdma-header-compile`;
- EP: `ep-layout` and `ep-host-validation`.

The top-level configure/build, suite builds, top-level reinstall, required-file
checks, and dependency/RPATH checks are also mandatory. They fail the build
phase directly.

[`run_hardware.sh`](../scripts/ci/control/run_hardware.sh) runs:

- health checks for devices 0 through 7;
- single-rank and eight-rank UDMA compatibility/fallback tests on 910B3;
- SDMA-disabled communicator validation;
- the eight-rank peer-memory DataCopy demo;
- the eight-rank EP dispatch/combine demo;
- eight-rank AllGather, AllReduce, ReduceScatter, and Broadcast correctness,
  with Broadcast roots 0 and 7;
- checked multi-size performance smoke for AllGather, AllReduce, ReduceScatter,
  and Broadcast, from 4 bytes through 1 MiB;
- the SDMA demo on every device for 64 bytes, 4096 bytes, and 1 MiB.

Explicitly out of scope are A5 / Ascend950 UDMA data-plane validation,
multi-host collectives and EP, vLLM model inference, and performance-regression
thresholds. A 910B3 UDMA fallback pass must not be reported as UDMA data-plane
coverage. AllToAll hardware validation is also out of scope on 910B3 because
the current kernel launch path requires a `TOPO_910_93` device.

## Results and failures

Host and NPU results are available in the Actions log. `Host Checks` also writes
a short, non-authoritative GitHub step summary. No CI artifacts are uploaded.

Failure classes and controller exit codes are:

- `code-or-test-failure` (`20`): a build, test, or required target failed;
- `resource-timeout` (`21`): the six-hour NPU idle budget expired;
- `resource-collision` (`22`): a foreign process appeared during hardware or
  another resource-ownership policy was violated;
- `runner-or-toolchain-failure` (`23`): controller, CANN, runner, host-health
  infrastructure, or final cleanup failed, including a
  leftover NPU process owned by `tilexr-ci`;
- `cancelled-or-obsolete` (`130`): Actions cancelled the run or the checked-out
  merge SHA did not match the event.

Resolve code failures in the pull request. For resource or infrastructure
failures, inspect the Actions log, confirm all NPUs are healthy and no CI
process remains, then rerun the failed job. Do not retry a resource collision
until the competing workload is understood.

## Local repository validation

Run the repository validation entrypoints from a clean TileXR checkout, then
check the resulting diff and worktree:

```bash
bash scripts/ci/host_checks.sh
ruby tests/ci/test_workflows.rb
git diff --check
git status --short
```

[`gate.py`](../scripts/ci/control/gate.py) requires Python 3.9 or newer plus
Linux `prctl`, `/proc`, pidfd, and subreaper semantics for live orchestration.
Provisioning verifies those Python bindings on `blue`. Its unit tests contain
macOS- and Python-3.8-compatible fakes, but a local macOS run is not a
substitute for the Ubuntu host checks or the real `blue` acceptance and
hardware runs.

## Provision and verify blue

Provision only from a fresh, detached checkout of a reviewed `main` commit on
`blue`. Record the commit SHA and verify `git rev-parse HEAD` before running any
root command. Do not reuse a mutable development checkout. Set `BOOTSTRAP` to
the absolute path of this verified checkout.

Run these idempotent scripts in order; each supports `--dry-run` and requires
root for live changes:

1. [`account.sh`](../scripts/ci/provision/account.sh) creates the bounded
   `tilexr-ci` account and directories.
2. [`cann.sh`](../scripts/ci/provision/cann.sh) installs and seals CANN 9.1.
3. [`control.sh`](../scripts/ci/provision/control.sh) installs the reviewed
   controller version.
4. [`runner.sh`](../scripts/ci/provision/runner.sh) installs and registers the
   Actions runner.
5. [`verify.sh`](../scripts/ci/provision/verify.sh) performs the final account,
   toolchain, service, and eight-device acceptance checks.

For live provisioning, `runner.sh` reads the short-lived registration token
from standard input and does not log it or place it in process arguments. A
dry run does not require a token. Generate the token just in time and pipe it
directly to the remote script:

```bash
gh api --method POST orgs/LingquLab/actions/runners/registration-token --jq .token |
  ssh blue "cd '${BOOTSTRAP}' && sudo bash scripts/ci/provision/runner.sh"
```

GitHub downloads, runner registration, service traffic, and job Git operations
use the local proxy at `http://127.0.0.1:3128`. The proxy and Git HTTP/1.1
settings are stored in the root-owned runner `.env`. `verify.sh` performs a
bounded public `git ls-remote` check through the same proxy.

## GitHub policy and runner group

An active `LingquLab` organization owner configures and verifies the repository
through [Actions settings](https://github.com/LingquLab/TileXR/settings/actions),
the [`ci-maintainers` team](https://github.com/orgs/LingquLab/teams/ci-maintainers),
the [organization runner groups](https://github.com/organizations/LingquLab/settings/actions/runner-groups),
and the [repository rulesets](https://github.com/LingquLab/TileXR/settings/rules).
The required state is:

- `ci-maintainers` contains exactly `Kur0x` and `chaowick`, both as team
  maintainers; the team has `push` access only to `LingquLab/TileXR`;
- [`CODEOWNERS`](../.github/CODEOWNERS) assigns the CI trust boundary to
  `@LingquLab/ci-maintainers`;
- Actions are limited to repository-local actions and the GitHub-owned actions
  referenced by the workflows, with external actions pinned to full commit
  SHAs;
- workflow permissions are read-only, workflows cannot approve pull requests,
  and every external contributor requires workflow approval;
- `TileXR-NPU` uses selected-repository visibility with only
  `LingquLab/TileXR`, allows this public repository, and is restricted to
  `LingquLab/TileXR/.github/workflows/npu-ci.yml@refs/heads/main`;
- `blue-tilexr-npu8` is the group's only runner and reports the expected
  `Linux`, `ARM64`, `tilexr`, `ascend910b`, and `npu8` labels.

## Enable the required check

Enable this only after a trial pull request has produced one successful
`PR Gate` check from the GitHub Actions App. In the active default-branch
[ruleset](https://github.com/LingquLab/TileXR/settings/rules), add exactly one
required status check named `PR Gate` and require the branch to be up to date
before merging.

Before saving, compare the complete ruleset and preserve every unrelated rule
and required check. In particular, keep two approvals, code-owner review,
resolved-conversation enforcement, deletion and non-fast-forward protection,
and an empty bypass list. After saving, reopen the ruleset and verify these
properties and the single `PR Gate` entry again.

## Controller upgrade

Publish every controller change under a new immutable version directory and
never replace an installed version. The workflow and job-completed hook both
use the root-managed `control/current` link. Change that link only while the
runner is stopped and has no active job:

1. From the exact reviewed controller commit, run
   `sudo bash scripts/ci/provision/control.sh --stage-only`. The script installs
   and validates the new package without changing `control/current`.
2. Let queued and active jobs drain, stop the runner service, and confirm that
   no runner worker or gate process remains.
3. Run `control.sh` without `--stage-only` to atomically update
   `control/current`.
4. Restart the runner and run `verify.sh` once as the upgrade acceptance test.

If the staged controller fails before activation, leave `control/current`
unchanged. If activation has already occurred, stop the idle runner before
restoring the previous `current` target. Do not change `current` while a job is
running.

## Runner upgrade

The runner is configured with `--disableupdate`. Upgrade it only when
`blue-tilexr-npu8` is online, not busy, and has no queued or active workflow.
Use a fresh reviewed `main` checkout, stop the existing service, and rerun
[`runner.sh`](../scripts/ci/provision/runner.sh) with a new registration token as
described above. Then run [`verify.sh`](../scripts/ci/provision/verify.sh) and
confirm the runner is online with the expected labels. Record the previous and
new runner versions and the verification output.

## Rollback

If CI infrastructure blocks normal development, remove only `PR Gate` from the
active default-branch ruleset. Before saving, verify every unrelated status
check and protection is unchanged; reopen the ruleset afterward and verify it
again. Then stop and disable the runner while preserving the account, sealed
controller, and CANN toolchain:

```bash
ssh blue 'service="$(sudo cat /home/tilexr-ci/actions-runner/.service)"; \
  sudo systemctl disable --now "${service}"'
```

Do not delete `/home/tilexr-ci/toolchains/cann/9.1.0` as part of an incident
rollback. The workflows may remain in the repository; they hold no deployment
authority or persistent secret. Re-enable the service and rerun
[`verify.sh`](../scripts/ci/provision/verify.sh) only after the cause is
resolved.
