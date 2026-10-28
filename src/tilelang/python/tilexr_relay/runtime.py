"""Compiler-free ctypes facade for installed Relay plans and executions.

Own native Host handles and descriptor metadata, not caller device storage.
Prepared bindings/executions separate setup from synchronous launch; related
handles share serialization/poison state. Native code loads trusted immutable
ELFs, and optional execution symbols are resolved only when requested.
"""

import ctypes as C
import threading
from dataclasses import dataclass, field

from .contracts import Binding, checked_end, checked_uint
from .planning import RelayPlan


class _BindingSpec(C.Structure):
    _fields_ = [("bytes", C.c_uint64), ("alignment", C.c_uint32),
                ("dtypeTag", C.c_uint32), ("layoutTag", C.c_uint32),
                ("reserved", C.c_uint32)]


class _Region(C.Structure):
    _fields_ = [("offset", C.c_uint64), ("bytes", C.c_uint64),
                ("alignment", C.c_uint32), ("space", C.c_uint32),
                ("initialization", C.c_uint32), ("reserved", C.c_uint32)]


class _Channel(C.Structure):
    _fields_ = [("firstSlot", C.c_uint32), ("slotCount", C.c_uint32),
                ("participants", C.c_uint32), ("maxCredits", C.c_uint32)]


class _Slot(C.Structure):
    _fields_ = [("source", C.c_uint32), ("index", C.c_uint32),
                ("offset", C.c_uint64), ("bytes", C.c_uint64),
                ("alignment", C.c_uint32), ("reserved", C.c_uint32)]


class _PlanDesc(C.Structure):
    _fields_ = [("structSize", C.c_uint32), ("version", C.c_uint32),
                ("abiVersion", C.c_uint32), ("protocolVersion", C.c_uint32),
                ("fileName", C.c_char_p), ("sha256", C.c_char_p),
                ("entry", C.c_char_p), ("target", C.c_char_p),
                ("blocks", C.c_uint32), ("rankCount", C.c_uint32),
                ("workspaceBytes", C.c_uint64), ("peerPayloadBytes", C.c_uint64),
                ("bindingCount", C.c_uint32), ("regionCount", C.c_uint32),
                ("channelCount", C.c_uint32), ("reserved", C.c_uint32),
                ("bindings", C.POINTER(_BindingSpec)), ("regions", C.POINTER(_Region)),
                ("channels", C.POINTER(_Channel)), ("slots", _Slot * 16)]


class _Binding(C.Structure):
    _fields_ = [("data", C.c_void_p), ("bytes", C.c_uint64),
                ("dtypeTag", C.c_uint32), ("layoutTag", C.c_uint32)]


class NativePlanDescription:
    """Keep borrowed strings/arrays alive until native Create deep-copies them."""

    def __init__(self, plan):
        if not isinstance(plan, RelayPlan):
            raise ValueError("expected a validated RelayPlan")
        if C.sizeof(C.c_void_p) != 8:
            raise RuntimeError("Relay v1 requires a 64-bit Host ABI")
        artifact = plan.artifact
        self.strings = tuple(value.encode("utf-8") for value in
                             (artifact.file_name, artifact.sha256, artifact.entry, artifact.target))
        self.bindings = (_BindingSpec * len(plan.binding_specs))(*[
            _BindingSpec(item.bytes, item.alignment, item.dtype_tag, item.layout_tag, 0)
            for item in plan.binding_specs])
        self.regions = (_Region * len(plan.regions))(*[
            _Region(item.offset, item.bytes, item.alignment, item.space, item.initialization, 0)
            for item in plan.regions])
        self.channels = (_Channel * len(plan.channels))(*[
            _Channel(item.first_slot, item.slot_count, item.participants, item.max_credits)
            for item in plan.channels])
        slots = (_Slot * 16)(*[_Slot(item.source, item.index, item.offset, item.bytes,
                                   item.alignment, 0) for item in plan.slots])
        self.descriptor = _PlanDesc(C.sizeof(_PlanDesc), 1, artifact.abi_version,
                                    artifact.protocol_version, *self.strings,
                                    plan.blocks, plan.rank_count, plan.workspace_bytes,
                                    plan.peer_payload_bytes, len(self.bindings),
                                    len(self.regions), len(self.channels), 0,
                                    self.bindings, self.regions, self.channels, slots)


class RelayError(RuntimeError):
    def __init__(self, operation, code, *, poisoned=False):
        self.operation = operation
        self.code = code
        self.poisoned = poisoned
        suffix = "; process exit required before device storage reuse" if poisoned else ""
        super().__init__(f"{operation} failed with code {code}{suffix}")


class RelayRuntime:
    def __init__(self, library_path):
        self._library = C.CDLL(str(library_path))
        self._execution_api = None
        signatures = {
            "TileXRRelayPlanCreate": [C.POINTER(_PlanDesc), C.POINTER(C.c_void_p)],
            "TileXRRelayPlanGetWorkspaceSize": [C.c_void_p, C.POINTER(C.c_uint64)],
            "TileXRRelayLaunch": [C.c_void_p, C.c_void_p, C.POINTER(_Binding),
                                  C.c_uint32, C.c_void_p, C.c_uint64, C.c_void_p],
            "TileXRRelayPlanDestroy": [C.c_void_p],
        }
        for name, arguments in signatures.items():
            function = getattr(self._library, name)
            function.argtypes = arguments
            function.restype = C.c_int

    def _load_execution_api(self):
        """Resolve optional symbols only when the new execution API is requested."""
        if self._execution_api is None:
            signatures = {
                "TileXRRelayExecutionCreate": [C.c_void_p, C.c_void_p, C.POINTER(_Binding),
                                               C.c_uint32, C.c_void_p, C.c_uint64,
                                               C.POINTER(C.c_void_p)],
                "TileXRRelayExecutionLaunch": [C.c_void_p, C.c_void_p],
                "TileXRRelayExecutionDestroy": [C.c_void_p],
            }
            try:
                functions = tuple(getattr(self._library, name) for name in signatures)
            except AttributeError as error:
                raise RuntimeError("loaded Relay library does not support prepared execution") from error
            for function, arguments in zip(functions, signatures.values()):
                function.argtypes = arguments
                function.restype = C.c_int
            self._execution_api = functions
        return self._execution_api

    def create_plan(self, plan):
        description = NativePlanDescription(plan)
        handle = C.c_void_p()
        result = self._library.TileXRRelayPlanCreate(C.byref(description.descriptor), C.byref(handle))
        if result:
            raise RelayError("TileXRRelayPlanCreate", result)
        return RuntimePlan(self, plan, handle)


@dataclass(frozen=True, init=False, eq=False)
class PreparedBindings:
    """Validated borrowed addresses bound to exactly one RuntimePlan.

    Obtain this value with ``RuntimePlan.prepare_bindings``. It retains only
    Host metadata/the native argument array, not allocations or tensors. Caller
    storage must remain live and unchanged until each synchronous launch returns.
    There is no communicator, stream or generation cached here; native validation
    still executes on every launch.
    """

    bindings: tuple
    workspace: int
    workspace_bytes: int
    _owner: object = field(repr=False)
    _native: object = field(repr=False)

    def __init__(self):
        raise TypeError("use RuntimePlan.prepare_bindings to create PreparedBindings")

    @classmethod
    def _create(cls, owner, bindings, workspace, workspace_bytes):
        value = object.__new__(cls)
        native = (_Binding * len(bindings))(*[
            _Binding(b.data, b.bytes, b.dtype_tag, b.layout_tag) for b in bindings])
        for name, item in (("bindings", bindings), ("workspace", workspace),
                           ("workspace_bytes", workspace_bytes), ("_owner", owner),
                           ("_native", native)):
            object.__setattr__(value, name, item)
        return value


class RuntimePlan:
    """Host handle ownership only; buffers/comm/stream remain caller-owned.

    Any native launch error conservatively poisons this adapter. Native callers
    may distinguish prelaunch rejection, but this C interface returns only a code.
    No destructor frees storage or implicitly performs runtime teardown.
    """

    def __init__(self, runtime, description, handle):
        self.runtime = runtime
        self.description = description
        self._handle = handle
        self._state = "ready"
        self._lock = threading.Lock()

    @property
    def state(self):
        return self._state

    def _enter(self):
        if not self._lock.acquire(blocking=False):
            raise RuntimeError("concurrent plan use/destruction is unsupported")

    def _require_live(self):
        if self._state == "destroyed":
            raise RuntimeError("plan has been destroyed")

    def _require_launchable(self):
        self._require_live()
        if self._state == "poisoned":
            raise RuntimeError("plan poisoned; process exit required before storage reuse")

    @property
    def workspace_bytes(self):
        self._enter()
        try:
            self._require_live()
            size = C.c_uint64()
            result = self.runtime._library.TileXRRelayPlanGetWorkspaceSize(self._handle, C.byref(size))
            if result:
                raise RelayError("TileXRRelayPlanGetWorkspaceSize", result)
            return size.value
        finally:
            self._lock.release()

    def launch(self, comm, bindings, workspace=0, workspace_bytes=0, stream=0):
        self._enter()
        try:
            self._require_launchable()
            values = tuple(bindings)
            self._validate_execution(comm, stream)
            prepared = self._prepare_values(values, workspace, workspace_bytes)
            self._invoke(comm, prepared, stream)
        finally:
            self._lock.release()

    def prepare_bindings(self, bindings, workspace=0, workspace_bytes=0):
        """Validate/pack static binding metadata outside the steady-state timer."""
        self._enter()
        try:
            self._require_launchable()
            return self._prepare_values(tuple(bindings), workspace, workspace_bytes)
        finally:
            self._lock.release()

    def launch_prepared(self, comm, prepared, stream):
        self._enter()
        try:
            self._require_launchable()
            self._validate_execution(comm, stream)
            self._validate_prepared(prepared)
            self._invoke(comm, prepared, stream)
        finally:
            self._lock.release()

    def prepare_execution(self, comm, prepared):
        """Freeze borrowed addresses/context in native Host state, without an epoch.

        The communicator, context and allocations must remain live/address-stable
        until the execution is destroyed. No tensor or device allocation is owned.
        """
        self._enter()
        try:
            self._require_launchable()
            checked_uint(comm, name="communicator")
            if not comm:
                raise ValueError("communicator must be nonnull")
            self._validate_prepared(prepared)
            create, launch, destroy = self.runtime._load_execution_api()
            handle = C.c_void_p()
            result = create(self._handle, comm, prepared._native, len(prepared.bindings),
                            prepared.workspace, prepared.workspace_bytes, C.byref(handle))
            if result:
                raise RelayError("TileXRRelayExecutionCreate", result)
            return RuntimeExecution(self, handle, launch, destroy)
        finally:
            self._lock.release()

    def _validate_prepared(self, prepared):
        if not isinstance(prepared, PreparedBindings) or prepared._owner is not self:
            raise ValueError("prepared bindings belong to a different plan")

    def _prepare_values(self, bindings, workspace, capacity):
        self._validate_bindings(bindings, workspace, capacity)
        return PreparedBindings._create(self, bindings, workspace, capacity)

    def _invoke(self, comm, prepared, stream):
        self._invoke_launch("TileXRRelayLaunch", self.runtime._library.TileXRRelayLaunch,
                            self._handle, comm, prepared._native, len(prepared.bindings),
                            prepared.workspace, prepared.workspace_bytes, stream)

    def _invoke_launch(self, operation, function, *arguments):
        self._state = "running"
        try:
            result = function(*arguments)
        except BaseException:
            self._state = "poisoned"
            raise
        if result:
            self._state = "poisoned"
            raise RelayError(operation, result, poisoned=True)
        self._state = "ready"

    @staticmethod
    def _validate_execution(comm, stream):
        for name, value in (("communicator", comm), ("stream", stream)):
            checked_uint(value, name=name)
        if not comm or not stream:
            raise ValueError("communicator and stream must be nonnull")

    def _validate_bindings(self, bindings, workspace, capacity):
        checked_uint(workspace, name="workspace")
        checked_uint(capacity, name="workspace capacity")
        plan = self.description
        if len(bindings) != len(plan.binding_specs):
            raise ValueError("binding count mismatch")
        ranges = []
        for actual, required in zip(bindings, plan.binding_specs):
            if not isinstance(actual, Binding):
                raise ValueError("expected Binding values")
            if actual.bytes < required.bytes or actual.data % required.alignment:
                raise ValueError("binding capacity/alignment mismatch")
            if (actual.dtype_tag, actual.layout_tag) != (required.dtype_tag, required.layout_tag):
                raise ValueError("binding dtype/layout mismatch")
            checked_end(actual.data, actual.bytes)
            ranges.append((actual.data, actual.data + required.bytes))
        if capacity < plan.workspace_bytes or (plan.workspace_bytes and not workspace):
            raise ValueError("workspace capacity mismatch")
        if plan.workspace_bytes:
            checked_end(workspace, capacity)
            required_alignment = max((r.alignment for r in plan.regions if r.space == 0), default=32)
            if workspace % required_alignment:
                raise ValueError("workspace alignment mismatch")
            ranges.append((workspace, workspace + plan.workspace_bytes))
        ranges.sort()
        if any(right[0] < left[1] for left, right in zip(ranges, ranges[1:])):
            raise ValueError("live binding/workspace ranges overlap")

    def destroy(self):
        self._enter()
        try:
            if self._state == "destroyed":
                return
            result = self.runtime._library.TileXRRelayPlanDestroy(self._handle)
            if result:
                raise RelayError("TileXRRelayPlanDestroy", result)
            self._state = "destroyed"
            self._handle = C.c_void_p()
        finally:
            self._lock.release()

    def __enter__(self):
        self._require_live()
        return self

    def __exit__(self, exception_type, exception, traceback):
        try:
            self.destroy()
        except RelayError as cleanup_error:
            if exception is None:
                raise
            # Preserve the launch/body error; host cleanup must not replace it.
            exception.relay_cleanup_error = cleanup_error


class RuntimeExecution:
    """Host-only execution handle sharing its parent's lock and poison state.

    Explicit parent destruction invalidates launch, but this handle can still be
    destroyed afterward. There is no implicit teardown or device-buffer ownership.
    """

    def __init__(self, parent, handle, launch, destroy):
        self._parent = parent
        self._handle = handle
        self._launch_native = launch
        self._destroy_native = destroy
        self._destroyed = False

    @property
    def state(self):
        if self._destroyed:
            return "destroyed"
        if self._parent.state == "destroyed":
            return "invalidated"
        return self._parent.state

    def _require_live(self):
        if self._destroyed:
            raise RuntimeError("execution has been destroyed")

    def launch(self, stream):
        parent = self._parent
        parent._enter()
        try:
            self._require_live()
            parent._require_launchable()
            checked_uint(stream, name="stream")
            if not stream:
                raise ValueError("stream must be nonnull")
            parent._invoke_launch("TileXRRelayExecutionLaunch", self._launch_native,
                                  self._handle, stream)
        finally:
            parent._lock.release()

    def destroy(self):
        parent = self._parent
        parent._enter()
        try:
            if self._destroyed:
                return
            result = self._destroy_native(self._handle)
            if result:
                raise RelayError("TileXRRelayExecutionDestroy", result)
            self._destroyed = True
            self._handle = C.c_void_p()
        finally:
            parent._lock.release()

    def __enter__(self):
        self._parent._enter()
        try:
            self._require_live()
            self._parent._require_launchable()
            return self
        finally:
            self._parent._lock.release()

    def __exit__(self, exception_type, exception, traceback):
        try:
            self.destroy()
        except RelayError as cleanup_error:
            if exception is None:
                raise
            exception.relay_cleanup_error = cleanup_error
