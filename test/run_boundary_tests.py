"""Run production C functions with hardware stubs; no translated algorithms.

Usage: python test/run_boundary_tests.py --cc /path/to/zig cc
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def read(name):
    return (ROOT / name).read_text(encoding='utf-8')


def function(source, name):
    match = re.search(r'^.*\b' + name + r'\([^;]*?\)\s*\n\{', source, re.M)
    assert match, name
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end] + '\n'


COMMON = '''
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint32_t tick;
static uint32_t QOS_Tick_Get(void) { return tick; }
static uint32_t __get_PRIMASK(void) { return 0; }
static void __disable_irq(void) {}
static void __set_PRIMASK(uint32_t p) { (void)p; }
'''


def cases():
    yield 'pid_output_tracking', COMMON + read('Drivers/Embedded-PID/src/pid.c') + '\n' + read('Core/SRC/tec_pid.c') + '''
int main(void) {
    TecPid_t pid;
    assert(TecPid_Init(&pid, .01f, .001f, 0, 1, -12000,
                      0, 100, -10, 10, .2f) == 0);
    TecPid_SetSetpoint(&pid, -5000);
    for (int i = 0; i < 100; ++i) TecPid_Update(&pid, -45000);
    assert(TecPid_GetOutput(&pid) == 100);
    TecPid_TrackOutput(&pid, -12000, 12);
    assert(TecPid_GetOutput(&pid) == 12 && pid.last_measure == -12000);
    assert(pid.lpf.lpf.y == 0);
    TecPid_SetSetpoint(&pid, -12000);
    TecPid_Update(&pid, -12000);
    assert(TecPid_GetOutput(&pid) == 12);
    TecPid_TrackOutput(&pid, -12000, 1000);
    assert(TecPid_GetOutput(&pid) == 100);
    TecPid_TrackOutput(&pid, -12000, -1);
    assert(TecPid_GetOutput(&pid) == 0);
}
'''
    src = read('Core/SRC/sdm845_boot.c')
    prefix = COMMON + '#include "sdm845_boot.h"\n' + '''
typedef int qSM_Status_t;
typedef struct { int Signal; void *NextState; } Handler;
typedef Handler *qSM_Handler_t;
#define QSM_SIGNAL_ENTRY 1
#define SDM845_TIMEOUT 2
#define qSM_STATUS_SIGNAL_HANDLED 1
#define qSM_STATUS_EXIT_SUCCESS 0
#define SDM845_BOOT_MAX_RETRIES 3
#define SDM845_T_STAGE5_SETTLE_MS 10000
#define USART2_TXSRC_PY32 0
#define GPIOB 0
#define SOM_BOOTED_Pin 0
static int stBootSuccess, stPwrPress, stStage2Wait, stStage3Wait,
    stStage4Wait, stStage5Wait, stBootReset, stStrapFail;
static struct { uint8_t stateId, retryCount; } s_boot;
static int seen;
static const char SDM845_CMD_ACCOUNT[] = "account";
static const char SDM845_CMD_PASSWORD[] = "password";
static const char SDM845_CMD_AMIXER[] = "amixer";
static int LL_GPIO_IsInputPinSet(int p, int n) { return seen; }
static int Sdm845_Detect_IsStage1Seen(void) { return seen; }
static int Sdm845_Detect_IsBooted(void) { return seen; }
static int Sdm845_Detect_IsStage3Seen(void) { return seen; }
static int Sdm845_Detect_IsStage4Seen(void) { return seen; }
static int Sdm845_Detect_IsStage5Seen(void) { return seen; }
static void Pin_PwrKey(int p) {}
static void Pin_RstKey(int p) {}
static void USART2_SetTxSource(int p) {}
static void Sdm845_Detect_ResetStage345(void) {}
static void sdm845_stage_send(const char *p, size_t n, unsigned ms) {}
static void sdm845_stage_send_retry(void) {}
'''
    functions = ''.join(function(src, 'st_' + state + '_Cb') for state in
                        ('PowerOnWait', 'BootCheck', 'Stage2Wait',
                         'Stage3Wait', 'Stage4Wait', 'Stage5Wait'))
    yield 'boot_timeout', prefix + functions + '''
int main(void) {
    qSM_Status_t (*cb[])(qSM_Handler_t) = {st_Stage2Wait_Cb, st_Stage3Wait_Cb,
                                         st_Stage4Wait_Cb, st_Stage5Wait_Cb};
    void *next[] = {&stStage3Wait, &stStage4Wait, &stStage5Wait, &stBootSuccess};
    Handler h = {SDM845_TIMEOUT, NULL};
    for (int i = 0; i < 4; ++i) {
        seen = 0; cb[i](&h); assert(h.NextState == &stStrapFail);
        seen = 1; cb[i](&h); assert(h.NextState == next[i]);
    }
    seen = 0; s_boot.retryCount = 0; st_BootCheck_Cb(&h);
    assert(h.NextState == &stBootReset);
    s_boot.retryCount = 3; st_BootCheck_Cb(&h);
    assert(h.NextState == &stStrapFail);
    seen = 1; st_BootCheck_Cb(&h); assert(h.NextState == &stStage2Wait);
    st_PowerOnWait_Cb(&h); assert(h.NextState == &stBootSuccess);
    seen = 0; st_PowerOnWait_Cb(&h); assert(h.NextState == &stPwrPress);
}
'''
    src = read('Core/SRC/aht20_task.c')
    prefix = COMMON + '''
typedef int qSM_Status_t;
typedef struct { int Signal; void *NextState; } Handler;
typedef Handler *qSM_Handler_t;
#define QSM_SIGNAL_ENTRY 1
#define AHT20_TIMEOUT 2
#define qSM_STATUS_SIGNAL_HANDLED 1
#define qSM_STATUS_EXIT_SUCCESS 0
#define AHT20_OK 0
#define AHT20_ERR_NOT_READY -5
static int stCheckStatus, stSampleWait, stWaitMeasure, stTrigger;
static struct {
    int32_t latestTempC, latestDewPointC;
    uint32_t sampleTick;
    uint8_t sampleValid;
    int lastStatus;
} s_aht;
static int result;
static int AHT20_Init(void) { return result; }
static int AHT20_StartMeasure(void) { return result; }
'''
    yield 'aht_recovery', prefix + ''.join(function(src, name) for name in
        ('st_SpecialInit_Cb', 'st_Trigger_Cb', 'st_SampleWait_Cb', 'aht_sample_fresh',
         'AHT20_Get_LastTempMilliCelsius', 'AHT20_Get_LastDewPointMilliCelsius', 'AHT20_Get_LastStatus')) + '''
int main(void) {
    Handler h = {QSM_SIGNAL_ENTRY, NULL};
    result = -2; st_SpecialInit_Cb(&h);
    h.Signal = AHT20_TIMEOUT; st_SpecialInit_Cb(&h);
    assert(h.NextState == &stSampleWait);
    st_SampleWait_Cb(&h); assert(h.NextState == &stCheckStatus);
    h.Signal = QSM_SIGNAL_ENTRY; st_Trigger_Cb(&h);
    h.Signal = 0; st_Trigger_Cb(&h); assert(h.NextState == &stSampleWait);
    result = 0; h.Signal = QSM_SIGNAL_ENTRY; st_Trigger_Cb(&h);
    h.Signal = 0; st_Trigger_Cb(&h); assert(h.NextState == &stWaitMeasure);
    s_aht.sampleValid = 1;
    s_aht.latestTempC = 25000; s_aht.latestDewPointC = 10000;
    s_aht.sampleTick = UINT32_MAX - 999; tick = 23999;
    assert(AHT20_Get_LastTempMilliCelsius() == 25000);
    tick = 24000;
    assert(AHT20_Get_LastTempMilliCelsius() == INT32_MIN);
    assert(AHT20_Get_LastDewPointMilliCelsius() == INT32_MIN);
    assert(AHT20_Get_LastStatus() == AHT20_ERR_NOT_READY);
    assert(s_aht.lastStatus == AHT20_OK); /* getters cannot alter FSM outcome */
}
'''
    src = read('Core/SRC/tres_i2c_slave.c')
    yield 'temperature_expiry', COMMON + '''
static uint8_t s_temp_valid;
static uint32_t s_temp_tick;
''' + function(src, 'TRES_I2C_Slave_GetSdm845TempValid') + '''
int main(void) {
    assert(!TRES_I2C_Slave_GetSdm845TempValid());
    s_temp_valid = 1; s_temp_tick = UINT32_MAX - 999; tick = 3999;
    assert(TRES_I2C_Slave_GetSdm845TempValid());
    tick = 4000; assert(!TRES_I2C_Slave_GetSdm845TempValid());
    tick = s_temp_tick; assert(!TRES_I2C_Slave_GetSdm845TempValid());
    s_temp_valid = 1; s_temp_tick = tick;
    assert(TRES_I2C_Slave_GetSdm845TempValid());
}
'''
    src = read('Core/SRC/tres_cmd.c')
    src = re.sub(r'^#include.*$', '', src, flags=re.M)
    seq = re.search(r'typedef enum\s*\{.*?\}\s*\w+;', read('Core/INC/tres_seq.h'), re.S)[0]
    yield 'command_ack', COMMON + '#include "tres_cmd.h"\n' + seq + '''
typedef int qTask_t;
typedef void *qEvent_t;
#define qHigh_Priority 0
#define qPeriodic 0
#define qDisabled 0
static int save_result, reload_result, save_calls;
static int reset_calls;
static void NVIC_SystemReset(void) { ++reset_calls; }
static int NvParams_Save(void) { ++save_calls; return save_result; }
static int NvParams_Reload(void) { return reload_result; }
static void NvParams_RestoreDefaults(void) {}
static uint8_t TRES_Seq_GetState(void) { return TRES_RUNNING; }
static int TRES_Seq_HostReady(void) { return 1; }
static void TRES_Seq_RequestReset(void) {}
static void TRES_Seq_RequestEnterDfu(void) {}
static int qTask_Notification_Send(qTask_t *t, void *p) { return 1; }
static int qOS_Add_Task(qTask_t *t, void (*cb)(qEvent_t), int p,
    float period, int mode, int en, void *d) { return 1; }
''' + src + '''
int main(void) {
    TRES_Cmd_Init();
    TRES_Cmd_PostFromISR(TRES_CMD_SAVE_PARAMS, 0);
    assert(TRES_Cmd_StatusByte() & 0x20);
    assert(TRES_Cmd_Completed() == 0);
    TRES_Cmd_PostFromISR(TRES_CMD_RESET, 0);
    assert(TRES_Cmd_LastErr() == TRES_ERR_BUSY);
    Parser_TaskCb(NULL);
    assert(save_calls == 1 && TRES_Cmd_Completed() == 1);
    assert(!(TRES_Cmd_StatusByte() & 0x20));
    save_result = -3;
    TRES_Cmd_PostFromISR(TRES_CMD_SAVE_PARAMS, 0); Parser_TaskCb(NULL);
    assert(TRES_Cmd_LastErr() == TRES_ERR_FLASH);
    save_result = 0;
    TRES_Cmd_PostFromISR(TRES_CMD_SAVE_PARAMS, 0); Parser_TaskCb(NULL);
    assert(TRES_Cmd_LastErr() == 0 && TRES_Cmd_Completed() == 3);
    reload_result = -2;
    TRES_Cmd_PostFromISR(TRES_CMD_RELOAD_PARAMS, 0); Parser_TaskCb(NULL);
    assert(TRES_Cmd_LastErr() == TRES_ERR_FLASH);
    s_completed = 255;
    TRES_Cmd_PostFromISR(TRES_CMD_REPORT_FAIL, 0); Parser_TaskCb(NULL);
    assert(TRES_Cmd_Completed() == 0 && TRES_Cmd_Retries() == 1);
    /* RESET_MCU: legal in every state, resets immediately and deliberately
     * without an acknowledgement (the MCU is gone before one could be sent).
     * The harness stubs the CMSIS reset so the path is at least exercised -
     * without this stub the whole case fails to link. */
    TRES_Cmd_PostFromISR(TRES_CMD_RESET_MCU, 0); Parser_TaskCb(NULL);
    assert(reset_calls == 1 && TRES_Cmd_LastErr() == TRES_ERR_NONE);
}
'''
    src = read('Core/SRC/usart2_dma.c')
    state = src[src.index('static volatile uint8_t s_rx_buf'):src.index('/*---------------------------------------------------------------------------*/', src.index('static volatile uint8_t s_rx_buf'))]
    yield 'dma_overrun', COMMON + '''
#define USART2_DMA_RX_BUF_LEN 512u
#define DMA1 0
#define LL_DMA_CHANNEL_1 1
static uint32_t ht, tc, te, remaining = USART2_DMA_RX_BUF_LEN;
static uint32_t LL_DMA_IsActiveFlag_HT1(int d) { return ht; }
static uint32_t LL_DMA_IsActiveFlag_TC1(int d) { return tc; }
static uint32_t LL_DMA_IsActiveFlag_TE1(int d) { return te; }
static void LL_DMA_ClearFlag_HT1(int d) { ht = 0; }
static void LL_DMA_ClearFlag_TC1(int d) { tc = 0; }
static void LL_DMA_ClearFlag_TE1(int d) { te = 0; }
static uint32_t LL_DMA_GetDataLength(int d, int c) { return remaining; }
''' + state + function(src, 'usart2_dma_write_pos') + function(src, 'USART2_DMA_Read') + '''
int main(void) {
    uint8_t out[512];
    s_rx_buf[0] = 42; remaining = 511;
    assert(USART2_DMA_Read(out, 512) == 1 && out[0] == 42);
    ht = 1; DMA1_Channel1_IRQHandler();
    tc = 1; remaining = 511; DMA1_Channel1_IRQHandler();
    assert(USART2_DMA_Read(out, 512) == 0); /* exactly one full ring */
    assert(USART2_DMA_TakeRxLoss() && !USART2_DMA_TakeRxLoss());
    assert(USART2_DMA_GetOverruns() == 1);
    ht = tc = 1; remaining = 510;
    assert(USART2_DMA_Read(out, 512) == 0); /* delayed IRQ / multiple wraps */
    assert(USART2_DMA_TakeRxLoss());
    s_rx_buf[2] = 77; remaining = 509;
    assert(USART2_DMA_Read(out, 512) == 1 && out[0] == 77);
    te = 1;
    assert(USART2_DMA_Read(out, 512) == 0 && USART2_DMA_TakeRxLoss());
}
'''
    src = read('Core/SRC/nv_params.c')
    layout = src[src.index('typedef struct {'):src.index('/* Production defaults')]
    yield 'nv_record_selection', COMMON + '#include "nv_params.h"\n' + '''
#define NVM_PAGE_BYTES 128u
#define NVM_MAGIC 0x4E564433u
#define NVM_VERSION 5u
#define NVM_SLOTS 2u
#define NVM_OK 0
#define NVM_ERR_CRC -2
''' + layout + '''
static Nvm_Block_t storage[2];
#define NVM_SLOT_ADDR(i) ((uintptr_t)&storage[i])
''' + function(src, 'nv_crc32') + function(src, 'nv_block_crc') + function(src, 'NvParams_Reload') + '''
static void record(unsigned i, uint32_t seq, uint32_t value, uint32_t version) {
    storage[i].data.magic = NVM_MAGIC;
    storage[i].data.version = version;
    storage[i].data.seq = seq;
    for (unsigned j = 0; j < NVM_PARAM_COUNT; ++j) storage[i].data.params[j] = value;
    storage[i].data.crc = version == 5 ? nv_block_crc(&storage[i].data) :
        nv_crc32((uint8_t *)storage[i].data.params, sizeof(storage[i].data.params));
}
int main(void) {
    s_cache[0] = 123;
    assert(NvParams_Reload() != 0 && s_cache[0] == 123);
    record(0, UINT32_MAX, 100, 5); record(1, 0, 200, 5);
    assert(NvParams_Reload() == 0 && s_cache[0] == 200);
    storage[1].data.seq ^= 1;
    assert(NvParams_Reload() == 0 && s_cache[0] == 100);
    record(0, 999999, 300, 4); record(1, 1, 400, 5);
    assert(NvParams_Reload() == 0 && s_cache[0] == 400);
    memset(&storage[1], 0xff, sizeof(storage[1]));
    assert(NvParams_Reload() == 0 && s_cache[0] == 300);
}
'''
    src = read('Core/SRC/mcp4725.c')
    yield 'dac_eeprom_packet', COMMON + '''
typedef int MCP4725_Status_t;
#define MCP4725_MAX_CODE 4095u
#define MCP4725_CMD_EEPROM 0x60u
#define MCP4725_I2C_ADDR 0x60u
static uint8_t sent[3];
static int SoftI2C_Master_Write(int a, uint8_t *data, unsigned n) {
    assert(n == 3); memcpy(sent, data, n); return 0;
}
static int mcp_status(int status) { return status; }
''' + function(src, 'MCP4725_WriteEEPROM') + '''
int main(void) {
    assert(MCP4725_WriteEEPROM(0xabc) == 0);
    assert(sent[0] == 0x60 && sent[1] == 0xab && sent[2] == 0xc0);
    MCP4725_WriteEEPROM(0xffff);
    assert(sent[1] == 0xff && sent[2] == 0xf0);
}
'''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', nargs='+', default=['cc'])
    parser.add_argument("--only")
    args = parser.parse_args(argv)
    with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
        for name, code in cases():
            if args.only and name != args.only:
                continue
            source = Path(tmp) / (name + '.c')
            exe = source.with_suffix('.exe')
            source.write_text(code, encoding='utf-8')
            subprocess.run(args.cc + ['-std=c11', '-I', str(ROOT / 'Core/INC'),
                                     '-I', str(ROOT / 'Drivers/Embedded-PID/src'),
                                     str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            print(name + ': passed', flush=True)


if __name__ == '__main__':
    main()
