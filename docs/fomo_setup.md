# Edge Impulse FOMO person detection — setup

Everything around the model is in this repository: the motion cascade, JPEG decode,
bilinear resize to the model input, contrast normalization, the tracker, the
three-state decision (`NONE` / `UNCERTAIN` / `CONFIDENT`) and the Telegram/MQTT
notification paths. **The model itself is not.**

## Why the model is not in this repo

An Edge Impulse Arduino export is ~24 MB and 1300+ generated files (the EI SDK plus a
vendored TensorFlow Lite Micro). Committing that would bloat every clone forever, bury
real changes in generated diffs, and publish one specific trained network as if it were
part of the firmware. `lib/ei-person-fomo/` is therefore in `.gitignore`, and you bring
your own model — either trained on your own footage, or from a public Edge Impulse
project.

With `-DINCLUDE_PERSON_DETECT` enabled (the default) and no model present, the build
fails on purpose with a message pointing here. A silent no-op would be worse: you would
flash a camera that quietly never detects anyone. To build without person detection,
comment out `-DINCLUDE_PERSON_DETECT` in `platformio.ini`.

## 1. Train a FOMO model on Edge Impulse Studio

1. Create a free account at <https://studio.edgeimpulse.com>.
2. **Create impulse → Image data → Object detection (FOMO)**.
3. **Image processing block**:
   - Color depth: **Grayscale**
   - Image width/height: **96×96** (recommended) or **64×64** (faster, less accurate).
4. **Data acquisition**: upload your own images or use the public
   [Person COCO subset](https://studio.edgeimpulse.com/public/123244/latest)
   as a starting point. Label bounding boxes with class name `person`.
5. **Training**: FOMO MobileNetV2 0.35, 60+ epochs, default learning rate.
   Aim for precision/recall ≥ 0.8 on the test set before deploying.

## 2. Deploy as an Arduino library

1. Go to **Deployment** → **Arduino library** → **Build**.
2. Download the generated ZIP (`ei-<project>-arduino-1.x.x.zip`).
3. Extract it so that the contents land directly inside
   `lib/ei-person-fomo/`. The final layout must be:

   ```
   lib/ei-person-fomo/
     library.properties           # replace the placeholder
     src/
       ei_person_fomo.h           # keep this file (or replace with EI's)
       <your-project>_inferencing.h
       edge-impulse-sdk/
       model-parameters/
       tflite-model/
   ```

4. Open `lib/ei-person-fomo/src/ei_person_fomo.h` and uncomment the
   `#include` line, adjusting the header name to match your project:

   ```cpp
   #include <your-ei-project_inferencing.h>
   ```

## 3. Rebuild and flash

```bash
pio run -t upload -t uploadfs --upload-port /dev/ttyACM0
```

Watch the serial log. Boot line should now read:

```
[PersonDet] Person detection initialized — EI FOMO model 96x96, 1 class(es)
```

instead of the placeholder message. Confidence threshold, temporal frames
and cooldown are all live-tunable from the Settings page.

## Performance notes

- **Inference time**: ~80–180 ms per frame on ESP32-S3 at 240 MHz for
  96×96 grayscale FOMO MobileNetV2 0.35. Expect ~3 fps end-to-end when
  the motion cascade gate is open.
- **PSRAM**: the EI SDK allocates tensor arena in PSRAM; budget
  ~300 kB on top of the decode/gray/model buffers already allocated here.
- **Cascade**: person detection runs only when motion detection raises
  the semaphore. This keeps average CPU down; only active frames cost
  the inference time.
- **Input normalization**: the model input is contrast-stretched to its 1st/99th
  percentile before inference (`stretchContrast()` in `src/person_detection.cpp`).
  Night frames from this sensor occupy a narrow slice of the range, and without the
  stretch scores sit just above the `UNCERTAIN` threshold. The stretch is skipped when
  the histogram is already wide (>200) or degenerate (<20), so it cannot amplify pure
  sensor noise into fake detail.

## Troubleshooting

- **`run_classifier` returns `EI_IMPULSE_OUT_OF_MEMORY`**: your model
  input is too large for the tensor arena. Retrain at 64×64 or use a
  smaller backbone.
- **No detections at all**: lower `confidence_threshold` in Settings,
  or inspect `getPersonDetectResult().raw_detections` via `/api/status`.
- **Build fails with multiple TensorFlow-Lite copies**: the EI SDK
  bundles TFLM; if you add another TFLM library, they'll collide.
  Remove the duplicate.
