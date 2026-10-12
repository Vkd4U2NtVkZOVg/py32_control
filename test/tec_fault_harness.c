/* Included after the actual firmware control source by run_tec_fault_tests.py.
 * Hardware and PID math are stubbed; assertions exercise the real callback. */
static void reset_fixture(void)
{
    cold = 45000; rpm = 1000; fail_dac = 0; demand = 50;
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
int main(void)
{
    uint32_t ma = 0;
    reset_fixture();
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
    puts("TEC fault callback tests passed");
    return 0;
}
