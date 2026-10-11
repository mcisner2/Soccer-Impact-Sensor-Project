// icm20649Imu.cpp

#include "config.h"

#if !USE_MOCK_IMU

#include <Arduino.h>
#include <Wire.h>
#include "icm20649Imu.h"

namespace
{
    // bank 0: accel x/y/z, gyro x/y/z, temp, 14 bytes in a row, big-endian (same block Adafruit_ICM20X reads)
    constexpr uint8_t dataStartRegister = 0x2D;
    constexpr uint8_t dataLength = 14;
}

bool Icm20649Imu::begin()
{
    // checks WHO_AM_I, resets, wakes the chip. false here = spec ERR_IMU_COMM (0x20)
    if (!chip.begin_I2C(config::imuI2cAddress, &Wire))
    {
        return false;
    }

    chip.setAccelRange(ICM20649_ACCEL_RANGE_30_G);
    chip.setGyroRange(ICM20649_GYRO_RANGE_4000_DPS);
    chip.setGyroRateDivisor(config::gyroRateDivisor);
    chip.setAccelRateDivisor(config::accelRateDivisor);

    Wire.setClock(config::i2cClockHz);

    samplePeriodUs = 1000000UL / config::imuOdrHz;
    lastSampleUs = micros();
    return true;
}

size_t Icm20649Imu::readSamples(RawImuSample* out, size_t maxCount)
{
    uint32_t elapsedUs = micros() - lastSampleUs;
    if (maxCount == 0 || elapsedUs < samplePeriodUs)
    {
        return 0;
    }

    // Registers only ever hold the newest sample. If we're late by 3 periods, 2 samples are gone for good.
    // This is the whole reason the spec wants the FIFO: it would have kept them.
    uint32_t periods = elapsedUs / samplePeriodUs;
    lastSampleUs += periods * samplePeriodUs;
    missed += periods - 1;

    if (!readRegisters(out[0]))
    {
        errors++;
        return 0;
    }
    return 1;
}

bool Icm20649Imu::readRegisters(RawImuSample& sample)
{
    Wire.beginTransmission(config::imuI2cAddress);
    Wire.write(dataStartRegister);
    if (Wire.endTransmission(false) != 0)  // false = repeated start, the read follows without releasing the bus
    {
        return false;
    }

    if (Wire.requestFrom(config::imuI2cAddress, dataLength) != dataLength)
    {
        return false;
    }

    uint8_t bytes[dataLength];
    for (uint8_t i = 0; i < dataLength; i++)
    {
        bytes[i] = Wire.read();
    }

    // chip sends high byte first, opposite of our little-endian packets
    for (int axis = 0; axis < 3; axis++)
    {
        sample.accel[axis] = (int16_t)((bytes[axis * 2] << 8) | bytes[axis * 2 + 1]);
        sample.gyro[axis] = (int16_t)((bytes[6 + axis * 2] << 8) | bytes[6 + axis * 2 + 1]);
    }
    sample.tempRaw = (int16_t)((bytes[12] << 8) | bytes[13]);
    return true;
}

#endif
