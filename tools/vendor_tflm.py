"""Assemble TensorFlow Lite Micro for the wake word (src/wakeword.cpp) into lib/, from pinned upstream releases.

    python tools/vendor_tflm.py

Writes, replacing what's there:
    lib/tflm/     esp-tflite-micro (Espressif's TFLM port) + TensorFlow's micro audio frontend + kissfft wrappers
    lib/esp-nn/   esp-nn, ESP32-S3 vector kernels used by esp-tflite-micro

All Apache-2.0, except kissfft (BSD-3-Clause); each keeps its licence file. The result is committed, so a build
needs no network. Run this again only to move to newer releases (change the pins below).
"""
from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TFLM_REPO, TFLM_TAG = "https://github.com/espressif/esp-tflite-micro.git", "v1.4.1"
ESPNN_REPO, ESPNN_TAG = "https://github.com/espressif/esp-nn.git", "v1.4.1"
TF_TAG = "v2.18.0"  # tensorflow/tensorflow, for tensorflow/lite/experimental/microfrontend/lib
FRONTEND = ["bits.h", "fft.cc", "fft.h", "fft_util.cc", "fft_util.h", "filterbank.c", "filterbank.h",
            "filterbank_util.c", "filterbank_util.h", "frontend.c", "frontend.h", "frontend_util.c", "frontend_util.h",
            "log_lut.c", "log_lut.h", "log_scale.c", "log_scale.h", "log_scale_util.c", "log_scale_util.h",
            "noise_reduction.c", "noise_reduction.h", "noise_reduction_util.c", "noise_reduction_util.h",
            "pcan_gain_control.c", "pcan_gain_control.h", "pcan_gain_control_util.c", "pcan_gain_control_util.h",
            "window.c", "window.h", "window_util.c", "window_util.h"]

TFLM_LIBRARY = {
    "name": "tflm",
    "version": TFLM_TAG.lstrip("v"),
    "description": "TensorFlow Lite Micro (esp-tflite-micro) and its micro audio frontend, for the wake word",
    "license": "Apache-2.0",
    "frameworks": "arduino",
    "platforms": "espressif32",
    "dependencies": [{"name": "esp-nn"}],
    "build": {
        "srcDir": ".",
        "includeDir": ".",
        "libArchive": True,
        "flags": ["-O3", "-DESP_NN", "-fno-rtti", "-fno-exceptions", "-fno-threadsafe-statics", "-Wno-unused-parameter", "-Wno-maybe-uninitialized",
                  "-Wno-missing-field-initializers", "-Wno-sign-compare", "-Wno-type-limits", "-Wno-nonnull",
                  "-Wno-double-promotion", "-Wno-shadow", "-Wno-return-type", "-Wno-strict-aliasing"],
        "srcFilter": [
            "-<*>",
            "+<tensorflow/lite/micro/*.cc>", "-<tensorflow/lite/micro/micro_time.cc>",
            "+<tensorflow/lite/micro/esp/micro_time.cc>",
            "+<tensorflow/lite/micro/kernels/*.cc>",
            "-<tensorflow/lite/micro/kernels/add.cc>", "-<tensorflow/lite/micro/kernels/conv.cc>",
            "-<tensorflow/lite/micro/kernels/depthwise_conv.cc>",
            "-<tensorflow/lite/micro/kernels/fully_connected.cc>", "-<tensorflow/lite/micro/kernels/mul.cc>",
            "-<tensorflow/lite/micro/kernels/pooling.cc>", "-<tensorflow/lite/micro/kernels/softmax.cc>",
            "+<tensorflow/lite/micro/kernels/esp_nn/*.cc>",
            "+<tensorflow/lite/micro/tflite_bridge/*.cc>",
            "+<tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc>",
            "+<tensorflow/lite/micro/memory_planner/linear_memory_planner.cc>",
            "+<tensorflow/lite/micro/arena_allocator/*.cc>",
            "+<tensorflow/lite/kernels/kernel_util.cc>",
            "+<tensorflow/lite/core/c/common.cc>",
            "+<tensorflow/lite/core/api/flatbuffer_conversions.cc>", "+<tensorflow/lite/core/api/tensor_utils.cc>",
            "+<tensorflow/lite/kernels/internal/common.cc>",
            "+<tensorflow/lite/kernels/internal/quantization_util.cc>",
            "+<tensorflow/lite/kernels/internal/portable_tensor_utils.cc>",
            "+<tensorflow/lite/kernels/internal/tensor_utils.cc>",
            "+<tensorflow/lite/kernels/internal/tensor_ctypes.cc>",
            "+<tensorflow/lite/kernels/internal/reference/portable_tensor_utils.cc>",
            "+<tensorflow/compiler/mlir/lite/core/api/error_reporter.cc>",
            "+<tensorflow/compiler/mlir/lite/schema/schema_utils.cc>",
            "+<tensorflow/lite/experimental/microfrontend/lib/*.c>",
            "+<tensorflow/lite/experimental/microfrontend/lib/*.cc>",
            "+<kissfft_int16/*.cc>",
        ],
    },
}

ESPNN_LIBRARY = {
    "name": "esp-nn",
    "version": ESPNN_TAG.lstrip("v"),
    "description": "Espressif's optimized neural network kernels (ESP32-S3 vector instructions)",
    "license": "Apache-2.0",
    "frameworks": "arduino",
    "platforms": "espressif32",
    "build": {
        "srcDir": ".",
        "includeDir": "include",
        "flags": ["-O2", "-mlongcalls", "-fno-unroll-loops", "-Wno-unused-function", "-Isrc/common"],
        "srcFilter": ["-<*>", "+<src/**/*_ansi.c>", "+<src/**/*_opt.c>", "+<src/common/esp_nn_multicore.c>",
                      "+<src/convolution/esp_nn_conv_mt_split.c>", "+<src/basic_math/esp_nn_mul_mt.c>",
                      "+<src/**/*_esp32s3.c>", "+<src/**/*_esp32s3.S>"],
    },
}

KISS_WRAPPER = """// kissfft built as 16-bit fixed point C++, as TensorFlow's micro frontend (fft.cc) expects.
#define FIXED_POINT 16
#include "{source}"
"""


def run(*args: str, cwd: Path | None = None) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def main() -> None:
    lib = ROOT / "lib"
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        run("git", "clone", "-q", "--depth", "1", "--branch", TFLM_TAG, TFLM_REPO, str(tmp_path / "tflm"))
        run("git", "clone", "-q", "--depth", "1", "--branch", ESPNN_TAG, ESPNN_REPO, str(tmp_path / "espnn"))

        tflm = lib / "tflm"
        shutil.rmtree(tflm, ignore_errors=True)
        tflm.mkdir(parents=True)
        for item in ("tensorflow", "third_party", "LICENSE"):
            src = tmp_path / "tflm" / item
            (shutil.copytree if src.is_dir() else shutil.copy2)(src, tflm / item)
        frontend = tflm / "tensorflow/lite/experimental/microfrontend/lib"
        frontend.mkdir(parents=True, exist_ok=True)
        base = f"https://raw.githubusercontent.com/tensorflow/tensorflow/{TF_TAG}/tensorflow/lite/experimental/microfrontend/lib"
        for name in FRONTEND:
            with urllib.request.urlopen(f"{base}/{name}", timeout=60) as resp:
                (frontend / name).write_bytes(resp.read())
        kiss = tflm / "kissfft_int16"
        kiss.mkdir()
        (kiss / "kiss_fft_int16.cc").write_text(KISS_WRAPPER.format(source="../third_party/kissfft/kiss_fft.c"))
        (kiss / "kiss_fftr_int16.cc").write_text(KISS_WRAPPER.format(source="../third_party/kissfft/tools/kiss_fftr.c"))
        (tflm / "library.json").write_text(json.dumps(TFLM_LIBRARY, indent=2) + "\n")
        (tflm / "VENDORED.md").write_text(
            f"Assembled by tools/vendor_tflm.py from esp-tflite-micro {TFLM_TAG} ({TFLM_REPO}) and TensorFlow "
            f"{TF_TAG}'s tensorflow/lite/experimental/microfrontend/lib (Apache-2.0, see LICENSE); kissfft is "
            f"BSD-3-Clause (third_party/kissfft/COPYING). Don't edit by hand: change the pins and run it again.\n")

        espnn = lib / "esp-nn"
        shutil.rmtree(espnn, ignore_errors=True)
        espnn.mkdir(parents=True)
        for item in ("include", "src", "LICENSE"):
            src = tmp_path / "espnn" / item
            (shutil.copytree if src.is_dir() else shutil.copy2)(src, espnn / item)
        (espnn / "library.json").write_text(json.dumps(ESPNN_LIBRARY, indent=2) + "\n")
        (espnn / "VENDORED.md").write_text(
            f"Assembled by tools/vendor_tflm.py from esp-nn {ESPNN_TAG} ({ESPNN_REPO}), Apache-2.0 (see LICENSE).\n")
    print(f"lib/tflm and lib/esp-nn ready ({TFLM_TAG}, {ESPNN_TAG}, frontend from TensorFlow {TF_TAG})")


if __name__ == "__main__":
    main()
