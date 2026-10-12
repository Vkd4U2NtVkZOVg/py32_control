/* Included after the actual firmware control source by run_tec_fault_tests.py.
 * Hardware and PID math are stubbed; assertions exercise the real callback. */
static void reset_fixture(void)
{
    cold = 45000; hot = 35000; rpm = 1000; fail_dac = 0; demand = 50;
    adc_ok = 1; adc_code = 0; fan_duty_seen = 0; fan_enabled = 0;
    for (unsigned i = 0; i < NVM_PARAM_COUNT; ++i) params[i] = s_defaults[i];
    TecControl_Init();
    s_p.temp_valid_min_millic = -20000;
    s_p.temp_valid_max_millic = 120000;
    s_p.hot_overtemp_millic = 80000;
    s_p.hot_danger_millic = 90000;
    s_p.control_period_ms = 500;
    s_p.fan_duty_base = s_p.fan_fault_min_duty = 30;
    s_p.fan_min_rpm = 500;
    s_p.en_off_pct = 8;
    s_p.dac_startup_pct = 10;
    s_p.ramp_step_percent = 5;
    s_p.dac_min_ctrl_mv = 760;
    s_p.dac_max_ctrl_mv = 1600;
    s_p.ff_enable = 0;
    s_p.cur_danger_ma = 3000;
}
int main(int argc, char **argv)
{
    uint32_t ma = 0;
    (void)argv;
    if (argc > 1) {
    /* All values pass the calibration envelope, but conversion exceeds u32. */
    reset_fixture(); adc_code = 4095;
    params[NV_TEC_CURRENT_ADC_VREF_MV] = 4096;
    params[NV_TEC_CURRENT_SHUNT_UOHM] = 125;
    params[NV_TEC_CURRENT_AMP_GAIN] = 1;
    params[NV_TEC_CURRENT_DIVIDER_NUM] = 1;
    params[NV_TEC_CURRENT_DIVIDER_DEN] = 131072;
    assert(!Studio_ADC1_ReadCurrentMilliamp(&ma) && ma == UINT32_MAX);
    TecControl_Cb(NULL);
    assert(!s_tec_enabled && fan_duty_seen == 100 && (s_alarm & TEC_ALARM_SENSOR));
        puts("Current conversion overflow tests passed");
        return 0;
    }
    reset_fixture();
    adc_code = 4000;
    assert(Studio_ADC1_ReadCurrentMilliamp(&ma) == 1);
    assert(ma == 3516);   /* ADC=4000, NVRAM divider 1:2 (div-calibration fix) */
    reset_fixture();
    rpm = 0;
    for (int i = 0; i < 24; ++i) TecControl_Cb(NULL);
    assert(s_fan_stalled && !s_tec_enabled);
    unsigned edges = enable_edges;
    for (int i = 0; i < 100; ++i) {
        TecControl_ClearAlarm();
        TecControl_Cb(NULL);
        assert(!s_tec_enabled && (s_alarm & TEC_ALARM_FAN_FAULT));
    }
    assert(enable_edges == edges);
    rpm = 1000;
    TecControl_Cb(NULL);
    assert(s_tec_enabled && !s_fan_stalled);
    /* Deadband cycling must not repeatedly award a startup grace. */
    reset_fixture(); rpm = 0;
    for (int i = 0; i < 80; ++i) {
        demand = (i % 2) ? 0 : 50;
        TecControl_Cb(NULL);
    }
    assert(s_fan_stalled && !s_tec_enabled);
    reset_fixture(); TecControl_Cb(NULL);
    cold = -40000; TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_SENSOR));
    cold = 200000; TecControl_Cb(NULL);
    assert(!s_tec_enabled);
    cold = 30000; TecControl_Cb(NULL);
    assert(s_tec_enabled && reset_measure == -30000);
    reset_fixture(); TecControl_Cb(NULL);
    fail_dac = 1; TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_DAC_FAULT));
    edges = enable_edges;
    for (int i = 0; i < 10; ++i) TecControl_Cb(NULL);
    assert(enable_edges == edges && !s_tec_enabled);
    fail_dac = 0; TecControl_Cb(NULL);
    assert(s_tec_enabled);
    /* ADC timeout must cool an already hot heatsink, not return at old PWM. */
    reset_fixture(); TecControl_Cb(NULL);
    hot = 95000; adc_ok = 0; TecControl_Cb(NULL);
    assert(!s_tec_enabled && fan_enabled && fan_duty_seen == 100);
    assert((s_alarm & (TEC_ALARM_SENSOR | TEC_ALARM_OVERTEMP)) ==
           (TEC_ALARM_SENSOR | TEC_ALARM_OVERTEMP));
    adc_ok = 1; hot = 35000; TecControl_Cb(NULL);
    assert(s_tec_enabled);
    /* A valid but implausible sample takes the same safe path. */
    adc_code = 4000; s_p.current_valid_max_ma = 3200;
    TecControl_Cb(NULL);
    assert(!s_tec_enabled && fan_duty_seen == 100 && (s_alarm & TEC_ALARM_SENSOR));
    /* Genuine in-window overcurrent still uses its own latch/alarm. */
    reset_fixture(); adc_code = 4000;
    TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_OVERCURRENT));
    /* Clamp the 64-bit foldback before narrowing. */
    reset_fixture(); hot = 100000;
    s_p.hot_overtemp_millic = 0; s_p.hot_danger_millic = 110000;
    s_p.hot_fb_gain_q16 = UINT32_MAX; s_p.hot_fb_max_millic = 200000;
    TecControl_Cb(NULL);
    assert(target_mc == 205000);
    puts("TEC fault callback tests passed");
    return 0;
}
