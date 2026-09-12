"""Read local ONNX metadata without importing ONNX or executing the model.

Field numbers follow this repository's onnx/onnx-ml.proto. This small wire
reader is an inspection aid, not an ONNX checker, shape inferencer or runtime.
Usage: python inspect_onnx_metadata.py MODEL [--output metadata.json]
"""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError("Truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("Oversized protobuf varint")


def fields(data):
    result = defaultdict(list)
    offset = 0
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if not number:
            raise ValueError("Invalid protobuf field number")
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                length, offset = varint(data, offset)
            else:
                length = 8 if wire == 1 else 4
            if offset + length > len(data):
                raise ValueError("Truncated protobuf field")
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError(f"Unsupported protobuf wire type {wire}")
        result[number].append(value)
    return result


def first(message, number, default=b""):
    return message.get(number, [default])[0]


def string(message, number):
    return first(message, number).decode("utf-8")


def value_info(data):
    info = fields(data)
    tensor = fields(first(fields(first(info, 2)), 1))
    shape = fields(first(tensor, 2))
    dims = []
    for raw in shape.get(1, []):
        dim = fields(raw)
        dims.append(first(dim, 1) if 1 in dim else string(dim, 2) or None)
    return {"name": string(info, 1), "element_type": first(tensor, 1, 0), "shape": dims}


def inspect(path):
    raw = path.read_bytes()
    model = fields(raw)
    graph = fields(first(model, 7))
    operators = Counter()
    node_summaries = []
    for encoded in graph.get(1, []):
        node = fields(encoded)
        op = string(node, 4)
        operators[op] += 1
        node_summaries.append({"name": string(node, 3), "op": op,
                               "outputs": [v.decode("utf-8") for v in node.get(2, [])]})
    metadata = {}
    for encoded in model.get(14, []):
        pair = fields(encoded)
        metadata[string(pair, 1)] = string(pair, 2)
    initializer_elements = 0
    initializer_dtypes = Counter()
    for encoded in graph.get(5, []):
        tensor = fields(encoded)
        dims = []
        for item in tensor.get(1, []):
            if isinstance(item, int):
                dims.append(item)
            else:
                offset = 0
                while offset < len(item):
                    value, offset = varint(item, offset)
                    dims.append(value)
        elements = 1
        for dim in dims:
            elements *= dim
        initializer_elements += elements
        initializer_dtypes[str(first(tensor, 2, 0))] += 1
    return {
        "inspection_scope": "protobuf metadata only; no ONNX checker or inference executed",
        "file": path.resolve().as_posix(), "size_bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "ir_version": first(model, 1, 0),
        "producer_name": string(model, 2), "producer_version": string(model, 3),
        "opsets": [{"domain": string(fields(v), 1), "version": first(fields(v), 2, 0)}
                   for v in model.get(8, [])],
        "graph_name": string(graph, 2),
        "inputs": [value_info(v) for v in graph.get(11, [])],
        "outputs": [value_info(v) for v in graph.get(12, [])],
        "metadata": metadata, "node_count": len(node_summaries),
        "operator_counts": dict(sorted(operators.items())),
        "initializer_count": len(graph.get(5, [])),
        "initializer_element_count_not_trainable_parameter_count": initializer_elements,
        "initializer_dtype_counts": dict(initializer_dtypes),
        "last_nodes": node_summaries[-12:],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rendered = json.dumps(inspect(args.model), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
