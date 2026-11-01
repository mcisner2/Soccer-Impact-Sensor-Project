# Soccer Impact Sensor (CSE 40522, Group 3)

Shin-guard wearable: ESP32-S3 Feather, ICM-20649 IMU, MTK3339 GPS. This repo holds the peak-detection and calibration work.

## Layout
- `peak_detection/` Python prototype, synthetic-data generator, grid search and stress tests
- `firmware/calibration/` C++ calibration module (drop-in for the team firmware) and a laptop host test
- `docs/` interface spec (v0.1) and task checklist

## Run
```
pip install numpy scipy matplotlib
python peak_detection/peak_detection_prototype.py
python peak_detection/refine_and_test.py
cd firmware/calibration && g++ -std=c++17 -Wall -Wextra -I. calibration.cpp calibration_host_test.cpp -o cal_test && ./cal_test
```
The host test needs `sampler.h` (the `ImuSample` struct) from the team firmware on the include path.

## Notes
Detector parameters were tuned on synthetic data only. The spec still says 200 Hz IMU, 10 Hz GPS and a 2:1 battery divider; the firmware uses 220 Hz, 5 Hz and 3:1.
