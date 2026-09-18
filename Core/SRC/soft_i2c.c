/**
  ******************************************************************************
  * @file    soft_i2c.c
  * @brief   Bit-banged (software) I2C master driver implementation.
  *
  * Strategy: SCL is a push-pull output driven by the MCU; SDA is an open-drain
  * output with an (external or internal) pull-up. Driving SDA *high* therefore
  * actually *releases* the line, and the in-flight level is read back through
  * the GPIO input data register (IDR). This lets a single slave (or a master's
  * own sensing) pull SDA low for ACK/data without any per-bit GPIO mode
  * switching, keeping the timing deterministic at 100 kHz / 400 kHz.
  *
  * The whole public API disables IRQs for the duration of a transaction so the
  * QuarkTS (or any other) scheduler tick cannot corrupt the bit timing.
  ******************************************************************************
  */
#include "soft_i2c.h"
#include <stddef.h>

/*============================================================================*/
/* Tuning                                                                      */
/*============================================================================*/
#ifndef SOFT_I2C_BUS_SPEED_HZ
    #define SOFT_I2C_BUS_SPEED_HZ      (100000UL)   /* 100 kHz (standard mode) */
#endif

#ifndef SOFT_I2C_CPU_MHZ
    #define SOFT_I2C_CPU_MHZ           (8u)         /* HSI after Studio_RCC_Init */
#endif

/* Soft delay: ~ (loads/cmp/branch + NOP) per iteration -> conservative /3.   */
#define SOFT_I2C_LOOPS_PER_US          ((SOFT_I2C_CPU_MHZ) / 3u)

/* One bit period = 1 / fSCL seconds; we delay half a period (high/low) and a  */
/* short settle window before sampling a read bit.                            */
#define SOFT_I2C_HALF_BIT_US           (uint32_t)(1000000UL / 2u / SOFT_I2C_BUS_SPEED_HZ)
#define SOFT_I2C_QUARTER_BIT_US        (uint32_t)(1000000UL / 4u / SOFT_I2C_BUS_SPEED_HZ)

/* R/W bit appended to a 7-bit address.                                       */
#define SOFT_I2C_WRITE_BIT             0U
#define SOFT_I2C_READ_BIT              1U

/*============================================================================*/
/* Low level pin primitives                                                    */
/*============================================================================*/
static void SoftI2C_DelayUs(volatile uint32_t us)
{
    volatile uint32_t loops = us * SOFT_I2C_LOOPS_PER_US;

    while (loops-- > 0U) {
        __NOP();
    }
}

/* SCL ----------------------------------------------------------------------*/
static inline void SoftI2C_SCL_High(void) { LL_GPIO_SetOutputPin(SOFT_IIC_SCL_Port, SOFT_IIC_SCL_Pin); }
static inline void SoftI2C_SCL_Low(void)  { LL_GPIO_ResetOutputPin(SOFT_IIC_SCL_Port, SOFT_IIC_SCL_Pin); }

/* SDA (open drain: high == released/hi-Z) ----------------------------------*/
static inline void SoftI2C_SDA_High(void) { LL_GPIO_SetOutputPin(SOFT_IIC_SDA_Port, SOFT_IIC_SDA_Pin); }
static inline void SoftI2C_SDA_Low(void)  { LL_GPIO_ResetOutputPin(SOFT_IIC_SDA_Port, SOFT_IIC_SDA_Pin); }

/* Read the current bus level on SDA (valid because it is open-drain).       */
static inline uint8_t SoftI2C_SDA_Read(void)
{
    return (uint8_t)(LL_GPIO_IsInputPinSet(SOFT_IIC_SDA_Port, SOFT_IIC_SDA_Pin) ? 1U : 0U);
}

/*============================================================================*/
/* Init / DeInit                                                              */
/*============================================================================*/
void SoftI2C_Init(void)
{
    LL_GPIO_InitTypeDef gpio = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);

    /* SCL: push-pull output, idle high.                                      */
    LL_GPIO_SetOutputPin(SOFT_IIC_SCL_Port, SOFT_IIC_SCL_Pin);
    gpio.Pin = SOFT_IIC_SCL_Pin;
    gpio.Mode = LL_GPIO_MODE_OUTPUT;
    gpio.Pull = LL_GPIO_PULL_NO;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    LL_GPIO_Init(SOFT_IIC_SCL_Port, &gpio);

    /* SDA: open-drain output, idle high (released). Pull-up enabled to relax */
    /* the board pull-up requirement.                                         */
    LL_GPIO_SetOutputPin(SOFT_IIC_SDA_Port, SOFT_IIC_SDA_Pin);
    gpio.Pin = SOFT_IIC_SDA_Pin;
    gpio.Mode = LL_GPIO_MODE_OUTPUT;
    gpio.Pull = LL_GPIO_PULL_UP;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_OPENDRAIN;
    LL_GPIO_Init(SOFT_IIC_SDA_Port, &gpio);
}

void SoftI2C_DeInit(void)
{
    LL_GPIO_SetOutputPin(SOFT_IIC_SDA_Port, SOFT_IIC_SDA_Pin);
    LL_GPIO_SetOutputPin(SOFT_IIC_SCL_Port, SOFT_IIC_SCL_Pin);
}

/*============================================================================*/
/* Sequence primitives                                                        */
/*============================================================================*/
SoftI2C_Status_t SoftI2C_Start(void)
{
    /* Bus-free check: both lines must be high before we can claim the bus.  */
    if ( (0U == SoftI2C_SDA_Read()) || (0U == LL_GPIO_IsInputPinSet(SOFT_IIC_SCL_Port, SOFT_IIC_SCL_Pin)) ) {
        return SOFT_I2C_ERR_BUSY;
    }

    /* START: SDA falls while SCL is high.                                   */
    SoftI2C_SDA_High();
    SoftI2C_SCL_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SDA_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SCL_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);

    return SOFT_I2C_OK;
}

void SoftI2C_Stop(void)
{
    /* STOP: SDA rises while SCL is high.                                    */
    SoftI2C_SDA_Low();
    SoftI2C_DelayUs(SOFT_I2C_QUARTER_BIT_US);
    SoftI2C_SCL_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SDA_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    /* Leave both lines idle high.                                           */
    SoftI2C_SCL_High();
    SoftI2C_SDA_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
}

SoftI2C_Status_t SoftI2C_RepStart(void)
{
    /* For a repeated START SCL is already low: raise SDA, then SCL, then    */
    /* drop SDA while SCL stays high.                                        */
    SoftI2C_SDA_High();
    SoftI2C_DelayUs(SOFT_I2C_QUARTER_BIT_US);
    SoftI2C_SCL_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SDA_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SCL_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);

    return SOFT_I2C_OK;
}

/*============================================================================*/
/* Byte / ACK primitive                                                       */
/*============================================================================*/
static inline void SoftI2C_ClockOut(void)
{
    SoftI2C_SCL_High();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    SoftI2C_SCL_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
}

/* After the 8 data bits the master releases SDA and pumps the 9th clock;     */
/* a slave drives SDA low to ACK. return qTrue if the slave ACKed.            */
static inline uint8_t SoftI2C_ReadAckBit(void)
{
    uint8_t nack;

    SoftI2C_SDA_High();                 /* release SDA for the slave        */
    SoftI2C_SCL_High();
    SoftI2C_DelayUs(SOFT_I2C_QUARTER_BIT_US);
    nack = SoftI2C_SDA_Read();          /* 0 = ACK, 1 = NACK                */
    SoftI2C_SCL_Low();
    SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);

    return nack;
}

SoftI2C_Status_t SoftI2C_WriteByte(uint8_t data)
{
    uint8_t i;

    for (i = 0U; i < 8U; i++) {
        if ((data & 0x80U) != 0U) {
            SoftI2C_SDA_High();
        } else {
            SoftI2C_SDA_Low();
        }
        data <<= 1U;
        SoftI2C_ClockOut();
    }

    return (0U == SoftI2C_ReadAckBit()) ? SOFT_I2C_OK : SOFT_I2C_ERR_NACK;
}

SoftI2C_Status_t SoftI2C_ReadByte(uint8_t *out, SoftI2C_Ack_t ack)
{
    uint8_t data = 0U;
    uint8_t i;

    if (NULL == out) {
        return SOFT_I2C_ERR_PARAM;
    }

    for (i = 0U; i < 8U; i++) {
        data <<= 1U;
        SoftI2C_SDA_High();             /* release for the slave              */
        SoftI2C_SCL_High();
        SoftI2C_DelayUs(SOFT_I2C_QUARTER_BIT_US);
        if (SoftI2C_SDA_Read() != 0U) {
            data |= 0x01U;
        }
        SoftI2C_SCL_Low();
        SoftI2C_DelayUs(SOFT_I2C_HALF_BIT_US);
    }

    /* Drive the 9th clock. ACK  -> keep reading; NACK -> master stops.      */
    if (SOFT_I2C_ACK == ack) {
        SoftI2C_SDA_Low();
    } else {
        SoftI2C_SDA_High();
    }
    SoftI2C_ClockOut();
    SoftI2C_SDA_High();                 /* back to released                  */

    *out = data;
    return SOFT_I2C_OK;
}

/*============================================================================*/
/* Higher-level transactions                                                  */
/*============================================================================*/
SoftI2C_Status_t SoftI2C_Master_Write(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
    SoftI2C_Status_t stg = SOFT_I2C_OK;
    uint16_t i;
    uint32_t primask;

    if ((NULL == buf) && (len > 0U)) {
        return SOFT_I2C_ERR_PARAM;
    }

    __disable_irq();
    primask = __get_PRIMASK();

    if (SOFT_I2C_OK != (stg = SoftI2C_Start())) {
        __set_PRIMASK(primask);
        return stg;
    }
    if (SOFT_I2C_OK != SoftI2C_WriteByte((uint8_t)((addr7 << 1U) | SOFT_I2C_WRITE_BIT))) {
        stg = SOFT_I2C_ERR_NACK;
    } else {
        for (i = 0U; i < len; i++) {
            if (SOFT_I2C_OK != SoftI2C_WriteByte(buf[i])) {
                stg = SOFT_I2C_ERR_NACK;
                break;
            }
        }
    }

    SoftI2C_Stop();
    __set_PRIMASK(primask);
    return stg;
}

SoftI2C_Status_t SoftI2C_Master_Read(uint8_t addr7, uint8_t *buf, uint16_t len)
{
    SoftI2C_Status_t stg = SOFT_I2C_OK;
    uint16_t i;
    uint32_t primask;

    if ((NULL == buf) && (len > 0U)) {
        return SOFT_I2C_ERR_PARAM;
    }

    __disable_irq();
    primask = __get_PRIMASK();

    if (SOFT_I2C_OK != (stg = SoftI2C_Start())) {
        __set_PRIMASK(primask);
        return stg;
    }
    if (SOFT_I2C_OK != SoftI2C_WriteByte((uint8_t)((addr7 << 1U) | SOFT_I2C_READ_BIT))) {
        stg = SOFT_I2C_ERR_NACK;
    } else {
        for (i = 0U; i < len; i++) {
            /* ACK all bytes except the last one.                            */
            SoftI2C_ReadByte(&buf[i], (i < (len - 1U)) ? SOFT_I2C_ACK : SOFT_I2C_NACK);
        }
    }

    SoftI2C_Stop();
    __set_PRIMASK(primask);
    return stg;
}

SoftI2C_Status_t SoftI2C_WriteReg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    SoftI2C_Status_t stg = SOFT_I2C_OK;
    uint16_t i;
    uint32_t primask;

    if ((NULL == buf) && (len > 0U)) {
        return SOFT_I2C_ERR_PARAM;
    }

    __disable_irq();
    primask = __get_PRIMASK();

    if (SOFT_I2C_OK != (stg = SoftI2C_Start())) {
        __set_PRIMASK(primask);
        return stg;
    }
    if (SOFT_I2C_OK != SoftI2C_WriteByte((uint8_t)((addr7 << 1U) | SOFT_I2C_WRITE_BIT))) {
        stg = SOFT_I2C_ERR_NACK;
    }
    else if (SOFT_I2C_OK != SoftI2C_WriteByte(reg)) {
        stg = SOFT_I2C_ERR_NACK;
    }
    else {
        for (i = 0U; i < len; i++) {
            if (SOFT_I2C_OK != SoftI2C_WriteByte(buf[i])) {
                stg = SOFT_I2C_ERR_NACK;
                break;
            }
        }
    }

    SoftI2C_Stop();
    __set_PRIMASK(primask);
    return stg;
}

SoftI2C_Status_t SoftI2C_ReadReg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
    SoftI2C_Status_t stg = SOFT_I2C_OK;
    uint16_t i;
    uint32_t primask;

    if ((NULL == buf) && (len > 0U)) {
        return SOFT_I2C_ERR_PARAM;
    }

    __disable_irq();
    primask = __get_PRIMASK();

    if (SOFT_I2C_OK != (stg = SoftI2C_Start())) {
        __set_PRIMASK(primask);
        return stg;
    }
    if (SOFT_I2C_OK != SoftI2C_WriteByte((uint8_t)((addr7 << 1U) | SOFT_I2C_WRITE_BIT))) {
        stg = SOFT_I2C_ERR_NACK;
    }
    else if (SOFT_I2C_OK != SoftI2C_WriteByte(reg)) {
        stg = SOFT_I2C_ERR_NACK;
    }
    /* Repeated START -> re-address the slave for reading.                   */
    else if (SOFT_I2C_OK != (stg = SoftI2C_RepStart())) {
        /* fall through to STOP */
    }
    else if (SOFT_I2C_OK != SoftI2C_WriteByte((uint8_t)((addr7 << 1U) | SOFT_I2C_READ_BIT))) {
        stg = SOFT_I2C_ERR_NACK;
    }
    else {
        for (i = 0U; i < len; i++) {
            SoftI2C_ReadByte(&buf[i], (i < (len - 1U)) ? SOFT_I2C_ACK : SOFT_I2C_NACK);
        }
    }

    SoftI2C_Stop();
    __set_PRIMASK(primask);
    return stg;
}

/*============================================================================*/
/* Convenience helpers                                                        */
/*============================================================================*/
SoftI2C_Status_t SoftI2C_WriteReg8(uint8_t addr7, uint8_t reg, uint8_t val)
{
    return SoftI2C_WriteReg(addr7, reg, &val, 1U);
}

SoftI2C_Status_t SoftI2C_ReadReg8(uint8_t addr7, uint8_t reg, uint8_t *val)
{
    return SoftI2C_ReadReg(addr7, reg, val, 1U);
}

SoftI2C_Status_t SoftI2C_ReadReg16(uint8_t addr7, uint8_t reg, uint16_t *val)
{
    uint8_t bytes[2];
    SoftI2C_Status_t stg;

    if (NULL == val) {
        return SOFT_I2C_ERR_PARAM;
    }

    stg = SoftI2C_ReadReg(addr7, reg, bytes, sizeof(bytes));
    if (SOFT_I2C_OK == stg) {
        *val = (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
    }
    return stg;
}