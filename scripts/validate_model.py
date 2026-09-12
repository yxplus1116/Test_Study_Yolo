"""Compare the native FP16 TensorRT path against ONNX Runtime CPU, using identical inputs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import numpy as np
import onnx
import onnxruntime as ort

def boxes(raw, confidence=.5, nms=.45):
    raw = raw[0]
    labels = raw[:, 5:].argmax(axis=1)
    scores = raw[:, 4] * raw[np.arange(len(raw)), 5 + labels]
    indices = np.flatnonzero(scores >= confidence)
    indices = indices[np.argsort(-scores[indices], kind="stable")]
    xyxy = np.concatenate((raw[:, :2] - raw[:, 2:4]/2, raw[:, :2] + raw[:, 2:4]/2), axis=1)
    kept = []
    for index in indices:
        if any(labels[index] == labels[j] and iou(xyxy[index], xyxy[j]) > nms for j in kept):
            continue
        kept.append(int(index))
    return [(int(labels[i]), float(scores[i]), xyxy[i]) for i in kept]

def iou(a, b):
    overlap = np.maximum(0, np.minimum(a[2:], b[2:])-np.maximum(a[:2], b[:2])).prod()
    union = np.maximum(0, a[2:]-a[:2]).prod() + np.maximum(0, b[2:]-b[:2]).prod() - overlap
    return float(overlap / union) if union > 0 else 0.0

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    model = root / "workspace/cf.onnx"
    onnx.checker.check_model(str(model), full_check=True)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
    env = os.environ.copy()
    env["PATH"] = ";".join(str(root / p) for p in [
        ".local/tensorrt/tensorrt_libs", ".local/cuda/bin", ".local/opencv-dist/opencv/build/x64/vc16/bin"
    ]) + ";" + env["PATH"]
    report = {"onnx_checker": "passed", "reference": ort.__version__, "cases": []}
    for source in sorted((root / "workspace/inference").glob("*.jpg")):
        out = root / ".local/validation" / source.stem
        subprocess.run([str(root/"dist/TestStudyYolo.exe"), "--image", str(source), "--label", "1",
            "--headless", "--dump-tensors", str(out), "--output", str(out)], cwd=root, env=env, check=True)
        data = np.fromfile(out/"input.f32", dtype=np.float32).reshape(1, 3, 640, 640)
        reference = session.run(["output"], {"images": data})[0]
        actual = np.fromfile(out/"output.f32", dtype=np.float32).reshape(reference.shape)
        assert np.isfinite(data).all() and 0 <= data.min() <= data.max() <= 1
        assert np.isfinite(reference).all() and np.isfinite(actual).all()
        delta = np.abs(actual-reference)
        expected_boxes, actual_boxes = boxes(reference), boxes(actual)
        matches = []
        available = list(actual_boxes)
        for label, score, box in expected_boxes:
            candidates = [(iou(box, b), i) for i, (l, _, b) in enumerate(available) if l == label]
            best, index = max(candidates, default=(0.0, -1))
            matches.append(best)
            if index >= 0:
                available.pop(index)
        case = {
            "image": source.name, "shape": list(reference.shape),
            "coordinate_mae": float(delta[..., :4].mean()),
            "coordinate_max_error": float(delta[..., :4].max()),
            "score_mae": float(delta[..., 4:].mean()),
            "score_max_error": float(delta[..., 4:].max()),
            "reference_boxes": len(expected_boxes), "tensorrt_boxes": len(actual_boxes),
            "minimum_matched_iou": min(matches) if matches else None,
        }
        case["passed"] = (
            case["coordinate_mae"] < .2 and case["score_mae"] < .002
            and len(expected_boxes) == len(actual_boxes)
            and all(value > .97 for value in matches)
        )
        report["cases"].append(case)
        print(json.dumps(case), flush=True)
    report["passed"] = all(case["passed"] for case in report["cases"])
    destination = root / ".local/validation/model-comparison.json"
    destination.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not report["passed"]:
        raise SystemExit("Numerical comparison failed; inspect " + str(destination))
    print("All ONNX/TensorRT comparisons passed. This validates implementation consistency, not model accuracy.")

if __name__ == "__main__":
    main()
