/**
  ******************************************************************************
  * @file    fsm_demo.h
  * @brief   QuarkTS Finite State Machine usage example (PY32F030 / PF3 LED)
  ******************************************************************************
  */
#ifndef __FSM_DEMO_H__
#define __FSM_DEMO_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and register the demo state-machine (plus a scripted
 *        controller task that drives it). Must be called after qOS_Setup().
 */
void FSM_Example_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __FSM_DEMO_H__ */