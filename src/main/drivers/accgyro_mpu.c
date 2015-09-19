/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "build_config.h"
#include "debug.h"

#include "common/maths.h"

#include "nvic.h"

#include "system.h"
#include "gpio.h"
#include "exti.h"
#include "bus_i2c.h"

#include "sensor.h"
#include "accgyro.h"
#include "accgyro_mpu3050.h"
#include "accgyro_mpu6050.h"
#include "accgyro_mpu.h"

//#define DEBUG_MPU_DATA_READY_INTERRUPT



uint8_t mpuLowPassFilter = INV_FILTER_42HZ;

void mpu6050FindRevision(void);

mpuDetectionResult_t mpuDetectionResult;

typedef struct mpuConfiguration_s {
    uint8_t gyroReadXRegister; // Y and Z must registers follow this, 2 words each
} mpuConfiguration_t;

static mpuConfiguration_t mpuConfiguration;
static const extiConfig_t *mpuIntExtiConfig = NULL;


#define MPU_ADDRESS             0x68

// MPU6050
#define MPU_RA_WHO_AM_I         0x75
#define MPU_RA_WHO_AM_I_LEGACY  0x00
#define MPU_RA_XA_OFFS_H        0x06    //[15:0] XA_OFFS
#define MPU_RA_PRODUCT_ID       0x0C    // Product ID Register
#define MPU_RA_ACCEL_XOUT_H     0x3B
#define MPU_RA_GYRO_XOUT_H      0x43

// WHO_AM_I register contents for MPU3050, 6050 and 6500
#define MPU6500_WHO_AM_I_CONST              (0x70)
#define MPUx0x0_WHO_AM_I_CONST              (0x68)

#define MPU_INQUIRY_MASK   0x7E

mpuDetectionResult_t *detectMpu(const extiConfig_t *configToUse)
{
    memset(&mpuDetectionResult, 0, sizeof(mpuDetectionResult));
    memset(&mpuConfiguration, 0, sizeof(mpuConfiguration));

    mpuIntExtiConfig = configToUse;

    bool ack;
    uint8_t sig;
    uint8_t inquiryResult;

    // MPU datasheet specifies 30ms.
    delay(35);

    ack = i2cRead(MPU_ADDRESS, MPU_RA_WHO_AM_I, 1, &sig);
    if (!ack)
        return &mpuDetectionResult;

    mpuConfiguration.gyroReadXRegister = MPU_RA_GYRO_XOUT_H;

    // If an MPU3050 is connected sig will contain 0.
    ack = i2cRead(MPU_ADDRESS, MPU_RA_WHO_AM_I_LEGACY, 1, &inquiryResult);
    inquiryResult &= MPU_INQUIRY_MASK;
    if (ack && inquiryResult == MPUx0x0_WHO_AM_I_CONST) {
        mpuDetectionResult.sensor = MPU_3050;
        mpuConfiguration.gyroReadXRegister = MPU3050_GYRO_OUT;
        return &mpuDetectionResult;
    }

    sig &= MPU_INQUIRY_MASK;

    if (sig != MPUx0x0_WHO_AM_I_CONST)
        return &mpuDetectionResult;

    mpuDetectionResult.sensor = MPU_60x0;

    mpu6050FindRevision();

    return &mpuDetectionResult;
}


void mpu6050FindRevision(void)
{
    bool ack;
    UNUSED(ack);

    uint8_t readBuffer[6];
    uint8_t revision;
    uint8_t productId;

    // There is a map of revision contained in the android source tree which is quite comprehensive and may help to understand this code
    // See https://android.googlesource.com/kernel/msm.git/+/eaf36994a3992b8f918c18e4f7411e8b2320a35f/drivers/misc/mpu6050/mldl_cfg.c

    // determine product ID and accel revision
    ack = i2cRead(MPU_ADDRESS, MPU_RA_XA_OFFS_H, 6, readBuffer);
    revision = ((readBuffer[5] & 0x01) << 2) | ((readBuffer[3] & 0x01) << 1) | (readBuffer[1] & 0x01);
    if (revision) {
        /* Congrats, these parts are better. */
        if (revision == 1) {
            mpuDetectionResult.resolution = MPU_HALF_RESOLUTION;
        } else if (revision == 2) {
            mpuDetectionResult.resolution = MPU_FULL_RESOLUTION;
        } else {
            failureMode(FAILURE_ACC_INCOMPATIBLE);
        }
    } else {
        ack = i2cRead(MPU_ADDRESS, MPU_RA_PRODUCT_ID, 1, &productId);
        revision = productId & 0x0F;
        if (!revision) {
            failureMode(FAILURE_ACC_INCOMPATIBLE);
        } else if (revision == 4) {
            mpuDetectionResult.resolution = MPU_HALF_RESOLUTION;
        } else {
            mpuDetectionResult.resolution = MPU_FULL_RESOLUTION;
        }
    }
}

void MPU_DATA_READY_EXTI_Handler(void)
{
    if (EXTI_GetITStatus(mpuIntExtiConfig->exti_line) == RESET) {
        return;
    }

    EXTI_ClearITPendingBit(mpuIntExtiConfig->exti_line);

#ifdef DEBUG_MPU_DATA_READY_INTERRUPT
    // Measure the delta in micro seconds between calls to the interrupt handler
    static uint32_t lastCalledAt = 0;
    static int32_t callDelta = 0;

    uint32_t now = micros();
    callDelta = now - lastCalledAt;

    //UNUSED(callDelta);
    debug[0] = callDelta;

    lastCalledAt = now;
#endif
}

void configureMPUDataReadyInterruptHandling(void)
{
#ifdef USE_MPU_DATA_READY_SIGNAL

#ifdef STM32F10X
    // enable AFIO for EXTI support
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
#endif

#ifdef STM32F303xC
    /* Enable SYSCFG clock otherwise the EXTI irq handlers are not called */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
#endif

#ifdef STM32F10X
    gpioExtiLineConfig(mpuIntExtiConfig->exti_port_source, mpuIntExtiConfig->exti_pin_source);
#endif

#ifdef STM32F303xC
    gpioExtiLineConfig(mpuIntExtiConfig->exti_port_source, mpuIntExtiConfig->exti_pin_source);
#endif

#ifdef ENSURE_MPU_DATA_READY_IS_LOW
    uint8_t status = GPIO_ReadInputDataBit(mpuIntExtiConfig->gpioPort, mpuIntExtiConfig->gpioPin);
    if (status) {
        return;
    }
#endif

    registerExti15_10_CallbackHandler(MPU_DATA_READY_EXTI_Handler);

    EXTI_ClearITPendingBit(mpuIntExtiConfig->exti_line);

    EXTI_InitTypeDef EXTIInit;
    EXTIInit.EXTI_Line = mpuIntExtiConfig->exti_line;
    EXTIInit.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTIInit.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTIInit.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTIInit);

    NVIC_InitTypeDef NVIC_InitStructure;

    NVIC_InitStructure.NVIC_IRQChannel = mpuIntExtiConfig->exti_irqn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = NVIC_PRIORITY_BASE(NVIC_PRIO_MPU_DATA_READY);
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = NVIC_PRIORITY_SUB(NVIC_PRIO_MPU_DATA_READY);
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
#endif
}

void mpuIntExtiInit(void)
{
    gpio_config_t gpio;

    static bool mpuExtiInitDone = false;

    if (mpuExtiInitDone || !mpuIntExtiConfig) {
        return;
    }

#ifdef STM32F303
        if (mpuIntExtiConfig->gpioAHBPeripherals) {
            RCC_AHBPeriphClockCmd(mpuIntExtiConfig->gpioAHBPeripherals, ENABLE);
        }
#endif
#ifdef STM32F10X
        if (mpuIntExtiConfig->gpioAPB2Peripherals) {
            RCC_APB2PeriphClockCmd(mpuIntExtiConfig->gpioAPB2Peripherals, ENABLE);
        }
#endif

    gpio.pin = mpuIntExtiConfig->gpioPin;
    gpio.speed = Speed_2MHz;
    gpio.mode = Mode_IN_FLOATING;
    gpioInit(mpuIntExtiConfig->gpioPort, &gpio);

    configureMPUDataReadyInterruptHandling();

    mpuExtiInitDone = true;
}

void configureMPULPF(uint16_t lpf)
{
    if (lpf == 256)
        mpuLowPassFilter = INV_FILTER_256HZ_NOLPF2;
    else if (lpf >= 188)
        mpuLowPassFilter = INV_FILTER_188HZ;
    else if (lpf >= 98)
        mpuLowPassFilter = INV_FILTER_98HZ;
    else if (lpf >= 42)
        mpuLowPassFilter = INV_FILTER_42HZ;
    else if (lpf >= 20)
        mpuLowPassFilter = INV_FILTER_20HZ;
    else if (lpf >= 10)
        mpuLowPassFilter = INV_FILTER_10HZ;
    else if (lpf > 0)
        mpuLowPassFilter = INV_FILTER_5HZ;
    else
        mpuLowPassFilter = INV_FILTER_256HZ_NOLPF2;
}

bool mpuAccRead(int16_t *accData)
{
    uint8_t buf[6];

    bool ack = i2cRead(MPU_ADDRESS, MPU_RA_ACCEL_XOUT_H, 6, buf);
    if (!ack) {
        return false;
    }

    accData[0] = (int16_t)((buf[0] << 8) | buf[1]);
    accData[1] = (int16_t)((buf[2] << 8) | buf[3]);
    accData[2] = (int16_t)((buf[4] << 8) | buf[5]);

    return true;
}

bool mpuGyroRead(int16_t *gyroADC)
{
    uint8_t buf[6];

    bool ack = i2cRead(MPU_ADDRESS, mpuConfiguration.gyroReadXRegister, 6, buf);
    if (!ack) {
        return false;
    }

    gyroADC[0] = (int16_t)((buf[0] << 8) | buf[1]);
    gyroADC[1] = (int16_t)((buf[2] << 8) | buf[3]);
    gyroADC[2] = (int16_t)((buf[4] << 8) | buf[5]);

    return true;
}