# External AICore Kernel Packaging Design

## Goal

Stop embedding production AICore `.o` files in TileXR Host shared libraries.
Keep the existing pure-AICore registration and launch model, but package each
kernel as an installed runtime artifact that its owning Host library loads on
first use.

This change covers every production object currently embedded by Collectives,
EP, and MoonEP. EP's legacy Dispatch and Combine kernel shared libraries also
move to the same external pure-AICore object model. Test-only and diagnostic
binaries remain outside this scope.

## Package Layout

Production kernel objects are installed under:

```text
${CMAKE_INSTALL_LIBDIR}/tilexr/kernels/
```

Host libraries continue to be installed directly under
`${CMAKE_INSTALL_LIBDIR}`. The same relative layout is staged in the build tree
so in-tree executables exercise the installed lookup contract rather than an
absolute source or build path.

Kernel filenames remain stable and component-qualified. No embedded copy,
generated `.incbin` source, or build-directory fallback is retained.

## Runtime Loading

A private C++14 loader shared by the production Host components accepts:

- an address belonging to the calling Host shared library;
- the fixed basename of the required kernel object;
- an output view containing the loaded bytes and size.

The loader uses `dladdr` to locate the owning shared library and resolves the
kernel as `tilexr/kernels/<basename>` beneath that library's directory. It
rejects invalid basenames, unresolved library locations, missing or unreadable
files, empty files, and sizes that cannot be represented by the registration
interface.

Loaded bytes are cached by resolved path for process lifetime. This preserves
the embedded implementation's stable-memory lifetime across
`rtDevBinaryRegister`, kernel launches, and unregister operations, while
avoiding repeated filesystem reads. Cache mutation and first registration stay
thread-safe.

There is no environment-variable override or absolute build path in production
libraries. Deployment must preserve the installed library/kernel directory
layout.

## Component Integration

Collectives loads the generic collective object plus the 64/128-rank A5
AllToAll objects before registering their existing signatures. MoonEP keeps its
common registration helper and changes each production launch path to identify
a kernel filename rather than embedded data and size symbols.

EP is packaged as A5-only kernel support without a build-time SoC selector.
Every `TILEXR_BUILD_EP=ON` build produces exactly four A5 pure-AICore objects:

```text
tilexr_ep_dispatch_kernel.o
tilexr_ep_combine_kernel.o
tilexr_ep_dispatch_memory_kernel.o
tilexr_ep_combine_memory_kernel.o
```

The ordinary Dispatch and Combine objects contain their same-node and
cross-node registered signatures. No 910B EP kernel variant is produced.
`libtilexr-ep.so` does not link a device-kernel shared library; it loads and
registers the selected object on first use and launches it with
`rtKernelLaunchWithFlagV2`.

EP Host entry points classify the current device with `aclrtGetSocName()`.
Ascend950 and the supported A5 aliases may register and launch EP kernels. A
non-A5 or unknown SoC returns `TILEXR_ERROR_NOT_SUPPORT` before loading an
object, allocating launch state, or advancing a communication round. This
keeps the Host library loadable from the same installation on 910B while making
the absence of 910B EP kernels explicit.

The public TileXR APIs, kernel signatures, registration APIs, launch APIs,
algorithm selection, capability fallback, and communicator ownership do not
change. A selected kernel whose external object cannot be loaded returns an
error before launch; it does not silently switch algorithms after stateful
initialization has begun.

## Build Changes

Kernel compilation continues to produce pure AICore objects. CMake no longer
generates or compiles embedding sources. Each owning Host target depends on its
kernel targets, stages the objects into its runtime-relative build directory,
and installs them into the common kernel directory.

EP removes `TILEXR_SOC_NAME`, `TILEXR_EP_SOC_TYPE`, and the related conditional
target graph. Its four objects always use the A5 compiler settings
`dav-c310-vec` and `CATLASS_ARCH=3510`. Enabling EP therefore needs no SoC
environment variable or CMake option and produces one portable Host package
whose EP execution capability is decided only at runtime. The old
`libtilexr_ep_dispatch_kernel.so` and `libtilexr_ep_combine_kernel.so` build,
link, RPATH, and install rules are removed.

The generic MoonEP kernel helper is reduced to object compilation and linking;
its embedding arguments and embedding custom command are removed. Obsolete
production embedding scripts are deleted. Test-only embedding helpers are
unchanged.

## Failure Handling

File and path failures include the resolved filename and concrete cause in a
bounded Host diagnostic. Runtime registration and function-registration errors
retain their existing TileXR error mapping and cleanup. Failed loads are not
cached as successes, allowing a corrected deployment to retry within the same
process when the owning registration state permits it.

## Validation

Host validation covers:

1. successful relative-path loading and byte/size preservation;
2. invalid name, missing file, empty file, and oversize rejection;
3. existing registration and launch mocks using externally supplied bytes;
4. clean and incremental CANN builds of affected targets;
5. build-tree staging and installed kernel manifests;
6. ELF inspection showing no former embedded binary-data symbols in production
   Host libraries.
7. EP build manifests containing all four A5 objects and no 910B variant;
8. EP runtime classification and early non-A5 rejection before binary loading;
9. registration and launch mocks for every ordinary and Memory EP signature.

Documentation is updated to describe the external runtime artifacts and the
required deployment layout. Host and build checks do not prove AICore execution
or data-plane correctness; supported-hardware validation remains required for
those claims.
