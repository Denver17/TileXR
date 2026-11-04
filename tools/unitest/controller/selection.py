"""Validate requested real invocation identities; do not select profiler events."""


def normalize(value, mode, iterations, batches):
    if not isinstance(value, dict) or value.get("kind") not in ("all", "selected"):
        raise ValueError("profiling.selection requires kind all or selected")
    if value["kind"] == "all":
        if set(value) != {"kind"}:
            raise ValueError("all selection does not accept rounds")
        return {"kind": "all"}
    if set(value) != {"kind", "rounds"} or not isinstance(value["rounds"], list) or not value["rounds"]:
        raise ValueError("selected profiling requires nonempty rounds")
    seen, result = set(), []
    for item in value["rounds"]:
        expected = {"batch", "iteration"} if mode == "perf" else {"iteration"}
        if not isinstance(item, dict) or set(item) != expected:
            raise ValueError("capture round requires " + "/".join(sorted(expected)))
        iteration = item["iteration"]
        batch = item.get("batch", 0)
        if (type(iteration) is not int or not 1 <= iteration <= iterations or
                type(batch) is not int or (mode == "perf" and not 1 <= batch <= batches)):
            raise ValueError("capture round outside resolved 1-based iteration/batch bounds")
        if (batch, iteration) in seen:
            raise ValueError("duplicate capture round")
        seen.add((batch, iteration))
        result.append(dict(item))
    return {"kind": "selected", "rounds": sorted(result, key=lambda item: (item.get("batch", 0), item["iteration"]))}


def selected(selection, batch, first, count):
    """Selected iteration numbers within one real batch/checkpoint segment."""
    if selection["kind"] == "all":
        return list(range(first, first + count))
    return [item["iteration"] for item in selection["rounds"]
            if item.get("batch", 0) == batch and first <= item["iteration"] < first + count]
