# A5 MoonEP Planner Tests

Source-only Host/reference checks:

```bash
bash tests/moonep_planner/build.sh source-only
```

On an A5 / Ascend950 host with CANN 9.1:

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
export ASCEND_DRIVER_PATH=/usr/local/Ascend/driver
export TILEXR_SOC_NAME=Ascend950
export PATH=/home/pkg/b061/cann-9.1.T560/bin:$PATH
bash tests/moonep_planner/build.sh full
bash tests/moonep_planner/demo/run_a5.sh 8 8192 8 896 biased 20 1000 8
bash tests/moonep_planner/demo/run_a5.sh 8 8192 16 896 biased 20 1000 8
```

The demo generates the same global routing independently on every process,
builds a CPU MoonEP reference plan, launches the A5 planner repeatedly, and
checks the asynchronous status and compares all four plan outputs element by
element. Dispatched capacity is exactly `S*K`; route groups are not padded.

## A5 Performance

The former padded-Planner numbers are not comparable with this compact V2 ABI.
Rerun the commands above before publishing V2 latency.

## A5 Correctness Matrix

The following boundary cases exercise an unaligned `B=7`, balanced routing,
all-local routing, all-remote routing, and duplicate destination encoding:

```bash
bash tests/moonep_planner/demo/run_a5.sh 8 64 4 56 balanced 2 3 8
bash tests/moonep_planner/demo/run_a5.sh 8 64 4 56 all_local 2 3 8
bash tests/moonep_planner/demo/run_a5.sh 8 64 4 56 all_remote 2 3 8
bash tests/moonep_planner/demo/run_a5.sh 8 64 4 56 duplicate 2 3 8
```

The last launcher argument is the physical device count. This oversubscribed
case maps two logical ranks to each of eight devices and automatically selects
32 AIV blocks per process:

```bash
bash tests/moonep_planner/demo/run_a5.sh 16 64 4 112 duplicate 2 3 8
```

Its logs include `logical_ranks=16`, `physical_devices=8`,
`oversubscribed=true`, and `performance_valid=false`; it is not 16-device
performance evidence. Override the bounded peer wait with
`TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS` when diagnosing slow peer mappings.

Inspect the installed dynamic tags after the full build:

```bash
readelf -d install/lib64/libtilexr-moonep-planner.so
nm -D install/lib64/libtilexr-moonep-planner.so | \
  grep -E 'rtDevBinaryRegister|rtFunctionRegister|rtKernelLaunchWithFlagV2'
```

The Host planner library loads its pure AICore ELF from
`lib/tilexr/kernels/tilexr_moonep_planner_v2_kernel.o` and must use only
`RPATH=$ORIGIN`; it must not contain a CANN `devlib` RPATH or RUNPATH. No
standalone Bisheng Host kernel SO is installed.
