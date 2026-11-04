# SST and CANN Runbook

## Remote Inspection

Use `paramiko` or SSH. Gather:

```bash
id
pwd
. /etc/os-release && echo "$PRETTY_NAME"
uname -m
df -h "$HOME"
command -v gcc g++ make tar gzip curl python3 bison flex cmake git mpicc mpicxx || true
find ~/pkg -maxdepth 1 -type f -name '*.run' -printf '%f\n' | sort
```

If root is used only for system packages, switch back to the target user for user-home builds.

## SST 14.0.0 Ordinary-User Install

Target layout:

```bash
BASE=$HOME
SRC=$BASE/scratch/src
LOCAL=$BASE/local
SST_CORE_HOME=$LOCAL/sstcore-14.0.0
SST_ELEMENTS_HOME=$LOCAL/sstelements-14.0.0
LOGDIR=$BASE/sst-build-logs
```

Install OpenMPI with the system package manager if `mpicc` is absent. On CentOS Stream 9:

```bash
sudo dnf install -y openmpi openmpi-devel
```

If `sudo` is unavailable for the target user, install packages as root, then build SST as the target user.

Put the official tarballs under `$SRC`:

```bash
sstcore-14.0.0.tar.gz       sha1 3c3f51134cc92ac7d659543aefcd8d4fd89fea28
sstelements-14.0.0.tar.gz   sha1 9206c915f873221398409c7216edeff58ccbd9cd
```

Build as the ordinary user:

```bash
set -euo pipefail
mkdir -p "$SRC" "$LOCAL" "$LOGDIR"
cd "$SRC"
echo "3c3f51134cc92ac7d659543aefcd8d4fd89fea28  sstcore-14.0.0.tar.gz" | sha1sum -c -
echo "9206c915f873221398409c7216edeff58ccbd9cd  sstelements-14.0.0.tar.gz" | sha1sum -c -

MPI_BIN=/usr/lib64/openmpi/bin
MPI_LIB=/usr/lib64/openmpi/lib
export PATH="$MPI_BIN:$SST_CORE_HOME/bin:$SST_ELEMENTS_HOME/bin:$PATH"
export LD_LIBRARY_PATH="$MPI_LIB:$SST_CORE_HOME/lib:$SST_CORE_HOME/lib64:$SST_ELEMENTS_HOME/lib:$SST_ELEMENTS_HOME/lib64:${LD_LIBRARY_PATH:-}"
export MPICC="$MPI_BIN/mpicc"
export MPICXX="$MPI_BIN/mpicxx"
export CC=gcc
export CXX=g++
JOBS=${JOBS:-16}

rm -rf sstcore-14.0.0
tar xfz sstcore-14.0.0.tar.gz
cd sstcore-14.0.0
./configure --prefix="$SST_CORE_HOME" MPICC="$MPICC" MPICXX="$MPICXX" > "$LOGDIR/sstcore-configure.log" 2>&1
make -j"$JOBS" all > "$LOGDIR/sstcore-make.log" 2>&1
make install > "$LOGDIR/sstcore-install.log" 2>&1

cd "$SRC"
rm -rf sst-elements-library-14.0.0
tar xfz sstelements-14.0.0.tar.gz
cd sst-elements-library-14.0.0
./configure --prefix="$SST_ELEMENTS_HOME" --with-sst-core="$SST_CORE_HOME" MPICC="$MPICC" MPICXX="$MPICXX" > "$LOGDIR/sstelements-configure.log" 2>&1
make -j"$JOBS" all > "$LOGDIR/sstelements-make.log" 2>&1
make install > "$LOGDIR/sstelements-install.log" 2>&1
```

Create `~/sst-env.sh`:

```bash
export MPIHOME=/usr/lib64/openmpi
export SST_CORE_HOME=$HOME/local/sstcore-14.0.0
export SST_ELEMENTS_HOME=$HOME/local/sstelements-14.0.0
export PATH=/usr/lib64/openmpi/bin:$SST_CORE_HOME/bin:$SST_ELEMENTS_HOME/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib64/openmpi/lib:$SST_CORE_HOME/lib:$SST_CORE_HOME/lib64:$SST_ELEMENTS_HOME/lib:$SST_ELEMENTS_HOME/lib64:${LD_LIBRARY_PATH:-}
export MPICC=/usr/lib64/openmpi/bin/mpicc
export MPICXX=/usr/lib64/openmpi/bin/mpicxx
export OMPI_MCA_btl='^openib'
export OMPI_MCA_btl_openib_warn_no_device_params_found=0
```

Verify:

```bash
. ~/sst-env.sh
sst --version
sst-info simpleElementExample > ~/sst-build-logs/sst-info-simpleElementExample.log 2>&1
grep -q 'ELEMENT LIBRARY 0 = simpleElementExample' ~/sst-build-logs/sst-info-simpleElementExample.log
sst-test-core > ~/sst-build-logs/sst-test-core.log 2>&1
cd ~/scratch/src/sst-elements-library-14.0.0/src/sst/elements/simpleElementExample/tests
sst example0.py > ~/sst-build-logs/simpleElementExample-example0.log 2>&1
grep -q 'Simulation is complete' ~/sst-build-logs/simpleElementExample-example0.log
```

## CANN 9.1.0 Install

The simulator baseline is CANN **9.1.0**. Prefer the pre-installed `tilexr-sim-9.1` image (CANN already at `/opt/sim/Ascend/cann-9.1.0`) over a manual install. Use manual install only when no image is available or a non-container host is required.

### Primary path: use the pre-installed image

The image already provides CANN 9.1.0 and the environment script:

```bash
source /opt/sim/cann-env.sh
bisheng --version    # or: ccec --version
```

Key image facts:

- `CANN_ROOT=/opt/sim/Ascend/cann-9.1.0` (single unified package; no 8.3-style split `toolkit/runtime/compiler/opp/...` directories)
- `ASCEND_HOME_PATH=$CANN_ROOT` is already exported by `/opt/sim/cann-env.sh`
- Packages download as `Ascend-cann_9.1.0_linux-x86_64.run` (aarch64: `Ascend-cann_9.1.0_linux-aarch64.run`)

### Backup path: manual install of CANN 9.1.0

Only for hosts without the image. Inspect the package first:

```bash
cd ~/pkg
./Ascend-cann_9.1.0_linux-x86_64.run --help | sed -n '1,120p'
```

If running as root and `/root/log` is a file (a known 8.x-era install blocker that some 9.1.0 builds may still hit):

```bash
mv /root/log /root/log.backup-before-cann-$(date +%Y%m%d%H%M%S)
mkdir -p /root/log/makeself
chmod 755 /root/log /root/log/makeself
```

Install into `~/Ascend`:

```bash
PREFIX=$HOME/Ascend
LOGDIR=$HOME/cann-install-logs
mkdir -p "$PREFIX" "$LOGDIR"
cd "$HOME/pkg"

./Ascend-cann_9.1.0_linux-x86_64.run --full --quiet --install-path="$PREFIX" 2>&1 | tee "$LOGDIR/cann.log"
```

Create `~/cann-env.sh` (single-root layout; the 8.3 split layout does not apply to 9.1.0):

```bash
export ASCEND_HOME_PATH=$HOME/Ascend/latest
export ASCEND_OPP_PATH=$HOME/Ascend/latest/opp
export TOOLCHAIN_HOME=$HOME/Ascend/latest/toolkit
export PATH=$HOME/Ascend/latest/bin:$HOME/Ascend/latest/compiler/bin:$HOME/Ascend/latest/compiler/ccec_compiler/bin:$HOME/Ascend/latest/toolkit/bin:$HOME/Ascend/latest/hccl/bin:$PATH
export LD_LIBRARY_PATH=$HOME/Ascend/latest/lib64:$HOME/Ascend/latest/runtime/lib64:$HOME/Ascend/latest/runtime/lib64/stub:$HOME/Ascend/latest/x86_64-linux/devlib:$HOME/Ascend/latest/compiler/lib64:$HOME/Ascend/latest/compiler/lib64/plugin/opskernel:$HOME/Ascend/latest/compiler/lib64/plugin/nnengine:$HOME/Ascend/latest/hccl/lib64:$HOME/Ascend/latest/hccl/lib64/plugin/opskernel:$HOME/Ascend/latest/opp/lib64:${LD_LIBRARY_PATH:-}
export PYTHONPATH=$HOME/Ascend/latest/python/site-packages:$HOME/Ascend/latest/compiler/python/site-packages:$HOME/Ascend/latest/hccl/python/site-packages:$HOME/Ascend/latest/fwkplugin/python/site-packages:${PYTHONPATH:-}
```

Verify:

```bash
. ~/cann-env.sh
atc --help >/tmp/atc-help.txt 2>&1
grep -q 'usage: atc' /tmp/atc-help.txt
ccec --version
grep -q '9.1.0' "$HOME/Ascend/9.1.0/version.info"
```

If a user-provided `tests/set_env.sh` points to an old path, either update it or override:

```bash
export ASCEND_TOOLKIT_HOME=$HOME/Ascend/9.1.0/x86_64-linux
export TIKCPP_INCLUDE_PATH=${ASCEND_TOOLKIT_HOME}/ascendc/include/basic_api:${ASCEND_TOOLKIT_HOME}/tikcpp/tikcfw
export CPLUS_INCLUDE_PATH=$TIKCPP_INCLUDE_PATH:$TIKCPP_INCLUDE_PATH/impl:$TIKCPP_INCLUDE_PATH/interface:${ASCEND_TOOLKIT_HOME}/include:${ASCEND_TOOLKIT_HOME}/include/ascendc/basic_api:${CPLUS_INCLUDE_PATH:-}
```

Run the `ccec` smoke compile requested by the user from the tests directory:

```bash
cd <home>/tests
source set_env.sh
<ascend-home>/x86_64-linux/ccec_compiler/bin/ccec \
  -std=c++17 -g -c -xcce -O2 test.cc -o ./smoke_aivUrma.o \
  --cce-aicore-arch=dav-c310 --cce-aicore-only \
  -mllvm -cce-aicore-function-stack-size=0x4000 \
  -mllvm -cce-aicore-addr-transform \
  -mllvm -cce-aicore-or-combine=false \
  -mllvm -instcombine-code-sinking=false \
  -Xclang -fcce-vf-vl=256 \
  --cce-auto-sync=off \
  -mllvm -cce-aicore-jump-expand=true \
  -mllvm -cce-aicore-mask-opt=false \
  -ferror-limit=0 -w -DENABLE_MOVVP -D PERF_API_NORM \
  -mllvm -api-deps-filter
test -s ./smoke_aivUrma.o
```
