# EP development

- After changing Server Dedup code, attempt a real CANN 9.1 compilation in the configured `blue` build environment before delivery. Compile Server Dedup only for A5; the existing EP target selects `dav-c310-vec` / `CATLASS_ARCH=3510` independently of the build host's NPU.
- For kernel-only changes, build `tilexr_ep_dispatch_memory_server_dedup_kernel`. When profiling code changes, check both profiling OFF and ON in separate build directories. Include the owning Host library when Host or launch code changes.
- Retain the source revision, compiler version, configuration, exit status and build log. CPU substitutes and Host Clang syntax checks supplement this compile attempt; they do not replace it. If blue is unavailable, report the actual connection/toolchain failure or missing configuration instead of claiming a CANN build passed.
- Follow [the EP validation guide](../../docs/EP_VALIDATION_GUIDE.md) for the compile procedure. A blue A5 cross-build proves compilation only; runtime correctness and performance still require the user's A5 validation environment.

## Code style

- Follow the local `.clang-format` with clang-format 19.1.7: four-space indentation, explicit braces for control flow, and one statement per line. Format only the production files in the task scope; test sources are separate.
- Keep include order, preprocessor conditions, expression evaluation order and synchronization calls unchanged in style-only edits. Ascend C attributes must remain attached to their declarations. Verify added braces and rebuild the affected Host/kernel targets; CPU substitutes do not replace a CANN compile.
