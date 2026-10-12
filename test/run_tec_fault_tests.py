"""Compile/run actual control callback with peripheral and PID stubs.

Usage: python test/run_tec_fault_tests.py --cc gcc
Cross-compiler syntax check only: add --syntax-only.
No firmware algorithms are translated into Python.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cc', nargs='+', default=['cc'])
parser.add_argument('--syntax-only', action='store_true')
args = parser.parse_args()
control = (root/'Core/SRC/tec_control.c').read_text(encoding='utf-8')
control = re.sub(r'^#include.*$', '', control, flags=re.M)
adc = (root/'Core/SRC/adc.c').read_text(encoding='utf-8')
adc = adc[adc.index('uint8_t Studio_ADC1_ReadCurrentMilliamp(uint32_t *out_ma)'):adc.index('int32_t Studio_ADC1_ReadHotMilliCelsius(void)')]
# Use the production defaults, enum, and exact current-conversion function.
nv = (root/'Core/SRC/nv_params.c').read_text(encoding='utf-8')
defaults = re.search(r'static const uint32_t s_defaults\[.*?\n};', nv, re.S).group()
prefix = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include "nv_params.h"
#include "tec_control.h"
#define NVM_Q16(x) ((uint32_t)((x) * NVM_Q16_SCALE))
typedef int qTask_t;
typedef int qEvent_t;
typedef float qTime_t;
typedef int TecPid_t;
typedef int MCP4725_Status_t;
#define MCP4725_OK 0
#define MCP4725_VDD_MV 3300
#define LL_ADC_CHANNEL_6 0   /* stubbed adc1_convert ignores the channel */
#define GPIOA 0
#define TEC_VEN_Pin 0
#define qLowest_Priority 0
#define qPeriodic 0
#define qEnabled 0
static int32_t cold = 45000;
static uint32_t rpm = 1000;
static int fail_dac, demand = 50;
static int32_t hot = 35000;
static uint8_t adc_ok = 1, fan_duty_seen, fan_enabled;
static uint16_t adc_code = 0;
static uint32_t params[NVM_PARAM_COUNT];
static float reset_measure;
static unsigned enable_edges;
static void LL_GPIO_SetOutputPin(int a, int b) { (void)a; (void)b; ++enable_edges; }
static void LL_GPIO_ResetOutputPin(int a, int b) { (void)a; (void)b; }
static int MCP4725_SetVoltage_mV(uint16_t mv) { (void)mv; return fail_dac; }
static void Fan_SetEnabled(uint8_t on) { fan_enabled = on; }
static void Fan_SetDutyPercent(uint8_t duty) { fan_duty_seen = duty; }
static uint32_t Fan_Get_LastRPM(void) { return rpm; }
static int32_t Studio_ADC1_ReadHotMilliCelsius(void) { return hot; }
static int32_t Studio_ADC1_ReadColdMilliCelsius(void) { return cold; }
static int32_t AHT20_Get_LastTempMilliCelsius(void) { return 25000; }
static uint16_t adc1_convert(uint32_t channel, uint8_t *ok)
{ (void)channel; *ok = adc_ok; return adc_code; }
static int Sdm845_Boot_IsThermalReady(void) { return 1; }
static int TecPid_Init(TecPid_t *p, float a, float b, float c, float d,
 float e, float f, float g, float h, float i, float j) { return 0; }
static void TecPid_Reset(TecPid_t *p, float m) { reset_measure = m; }
static void TecPid_SetSetpoint(TecPid_t *p, float s) {}
static void TecPid_Update(TecPid_t *p, float m) {}
static float TecPid_GetOutput(TecPid_t *p) { return (float)demand; }
static int qOS_Add_Task(qTask_t *t, void (*cb)(qEvent_t), int pr,
 qTime_t period, int mode, int enabled, void *data) { return 0; }
'''
# Callback uses qEvent_t as supplied by QuarkTS; fixtures pass a null event.
prefix = prefix.replace('typedef int qEvent_t;', 'typedef void *qEvent_t;')
nv_stubs = '''
uint32_t NvParams_Get(uint8_t id) { return params[id]; }
uint32_t NvParams_GetDefault(uint8_t id) { return s_defaults[id]; }
'''
source = prefix + defaults + nv_stubs + adc + control
source += (root/'test/tec_fault_harness.c').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(dir=root) as directory:
    source_path = Path(directory)/'tec_fault_test.c'
    source_path.write_text(source, encoding='utf-8')
    command = args.cc + ['-std=c99', '-I', str(root/'Core/INC')]
    if args.syntax_only:
        command += ['-fsyntax-only', str(source_path)]
    else:
        executable = Path(directory)/'tec_fault_test.exe'
        command += [str(source_path), '-o', str(executable)]
    subprocess.run(command, check=True)
    if not args.syntax_only:
        subprocess.run([str(executable)], check=True)
        subprocess.run([str(executable), "overflow"], check=True)
    else:
        print('C harness syntax check passed; assertions were NOT executed')
