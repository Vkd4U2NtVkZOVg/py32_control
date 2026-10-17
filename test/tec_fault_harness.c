/* Included after the actual firmware control source by run_tec_fault_tests.py.
 * Hardware and PID math are stubbed; assertions exercise the real callback. */
static void reset_fixture(void)
{
    dew = 10000;
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
    unsigned edges;
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
    /* The NVRAM base airflow must be published as the fan's duty floor on
     * every parameter load (boot and live reload alike). */
    assert(fan_min_duty == 30);
    adc_code = 4000;
    assert(Studio_ADC1_ReadCurrentMilliamp(&ma) == 1);
    assert(ma == 3516);   /* ADC=4000, NVRAM divider 1:2 (div-calibration fix) */
#if TEC_FAN_FAULT_ENABLE
    reset_fixture();
    rpm = 0;
    for (int i = 0; i < 24; ++i) TecControl_Cb(NULL);
    assert(s_fan_stalled && !s_tec_enabled);
    edges = enable_edges;
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
#else
    reset_fixture(); rpm = 0; tach_reads = 0;
    for (int i = 0; i < 200; ++i) {
        TecControl_Cb(NULL);
        assert(s_tec_enabled && !(s_alarm & TEC_ALARM_FAN_FAULT));
    }
    assert(tach_reads == 0); /* bench mode must not consult tach feedback */
    assert(fan_duty_seen == tec_fan_duty_from_hot(hot));
#endif
    /* Overtemperature alone must still shut down in both build modes. */
    reset_fixture(); hot = 95000; TecControl_Cb(NULL);
    assert(!s_tec_enabled && fan_duty_seen == 100 && (s_alarm & TEC_ALARM_OVERTEMP));
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
    /* An uncalibrated ADC is a fail-safe condition, and this harness compiles
     * the REAL current path (the temperature getters are stubbed above, so
     * their calibration gate is not reachable from here). An invalid sample
     * must be reported - never a silent 0 mA - because 0 mA is a legitimate
     * TEC-off reading and would let the overcurrent guard run blind. */
    reset_fixture();
    s_calib_failed = 1u;
    assert(Studio_ADC1_ReadCurrentMilliamp(&ma) == 0u && ma == 0u);
    TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_SENSOR));
    s_calib_failed = 0u;
    /* Clamp the 64-bit foldback before narrowing. */
    reset_fixture(); hot = 100000;
    s_p.hot_overtemp_millic = 0; s_p.hot_danger_millic = 110000;
    s_p.hot_fb_gain_q16 = UINT32_MAX; s_p.hot_fb_max_millic = 200000;
    TecControl_Cb(NULL);
    /* The foldback adds the clamped lift to the DYNAMIC base target, so the
     * expected value follows s_p.target_base_millic (= 45000), not a fixed
     * constant: base + hot_fb_max = 45000 + 200000 = 245000. */
    assert(target_mc == 245000);
    /* Entering the hold from high power keeps driving (no min-duty clamp). */
    reset_fixture(); demand = 80;
    for (int i = 0; i < 30; ++i) TecControl_Cb(NULL);
    assert(s_dac_percent == 80);
    edges = enable_edges; cold = 12000;
    TecControl_Cb(NULL);
    assert(s_dew_hold && s_tec_enabled && s_dac_percent == 80);
    for (int i = 0; i < 30; ++i) TecControl_Cb(NULL);
    assert(s_dac_percent == 80 && s_tec_enabled && enable_edges == edges);
    /* Dew hold with zero demand: EN deadband cuts TEC_VEN (no duty floor). */
    reset_fixture(); cold = 12000; demand = 0;
    for (int i = 0; i < 20; ++i) TecControl_Cb(NULL);
    assert(s_dew_hold && !s_tec_enabled && s_dac_percent == 0);
    assert(s_alarm & TEC_ALARM_DEWPOINT);
    edges = enable_edges;
    for (int i = 0; i < 100; ++i) {
        cold = (i % 2) ? 11900 : 12500;
        TecControl_Cb(NULL);
        assert(s_dew_hold && !s_tec_enabled && s_dac_percent == 0);
    }
    assert(enable_edges == edges); /* no enable chatter at the floor */
    cold = 13000; demand = 80; TecControl_Cb(NULL);
    assert(!s_dew_hold && s_tec_enabled);
    assert(s_dac_percent == 15); /* first-enable anchor 10 + ramp 5 */
    assert(target_mc >= 12000);
    /* Missing dew sample latches the hold; FF stays disabled during it. */
    dew = INT32_MIN; s_p.ff_enable = 1; s_p.ff_gain = 100;
    for (int i = 0; i < 30; ++i) TecControl_Cb(NULL);
    assert(s_dew_hold && s_tec_enabled && s_dac_percent == 80);
    assert(s_alarm & TEC_ALARM_SENSOR);
    /* Other faults still win over the dew hold. */
    hot = 95000; TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_OVERTEMP));
    hot = 35000; adc_code = 4000; TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_OVERCURRENT));
    adc_code = 0; fail_dac = 1; TecControl_Cb(NULL);
    assert(!s_tec_enabled && (s_alarm & TEC_ALARM_DAC_FAULT));

    /* A parameter written over the I2C window must take effect at runtime:
     * no reboot, and within one control cycle of the generation moving. */
    reset_fixture(); demand = 50;
    assert((int32_t)params[NV_TEC_EN_OFF_PCT] == 8);
    for (int i = 0; i < 10; ++i) TecControl_Cb(NULL);
    assert(s_tec_enabled && s_dac_percent == 50);
    params[NV_TEC_EN_OFF_PCT] = 60;     /* duty 50 is now inside the deadband */
    nv_cache_gen++;                     /* the host write bumps the cache gen */
    TecControl_Cb(NULL);
    assert(!s_tec_enabled);             /* live-reloaded: EN dropped this cycle */

    /* An unrelated slot must NOT re-initialise the PID: TecPid_Init() clears
     * the integrator and history, so resetting a running controller on every
     * parameter write is exactly what the guard has to prevent. */
    {
        unsigned before = pid_inits;
        for (int i = 0; i < 8; ++i) {
            params[NV_TEC_DEWPOINT_MARGIN_MILLIC]++;   /* unrelated to the PID */
            nv_cache_gen++;
            TecControl_Cb(NULL);
        }
        assert(pid_inits == before);
    }
    /* A real gain change must go the other way: the new gains have to reach the
     * controller (re-initialising, which is the documented cost). */
    {
        unsigned before = pid_inits;
        /* This block asserts that duty 50 drives the TEC, so the EN deadband
         * has to sit below it again: the deadband case above left the live
         * en_off_pct at 60, which the reload carries across fixtures. */
        params[NV_TEC_EN_OFF_PCT] = 8;
        params[NV_TEC_PID_KP_Q16] = NVM_Q16(2.0f);
        nv_cache_gen++;
        TecControl_Cb(NULL);
        assert(pid_inits == before + 1);
        assert(s_tec_enabled);          /* duty 50 is outside the deadband again */
    }

    /* The loop period is owned by the scheduler, so a mid-run write to it must
     * NOT reach the fan-stall timers and rescale their millisecond maths. */
    assert(s_p.control_period_ms == 500);
    params[NV_TEC_CONTROL_PERIOD_MS] = 5000;
    nv_cache_gen++;
    TecControl_Cb(NULL);
    assert(s_p.control_period_ms == 500);
    params[NV_TEC_CONTROL_PERIOD_MS] = 500;
    nv_cache_gen++;

    /* Every value derived from a parameter must be computed AFTER the reload:
     * a host write to EN_OFF_PCT must move the EN-deadband cut within one
     * cycle. demand=50 sits below the new threshold 60, so the live write
     * drops TEC_VEN immediately (covered above); here the same write moves
     * the dew-hold release band: cold below dew+margin keeps the hold, and
     * the raised target still applies after the reload. */
    reset_fixture(); demand = 0; cold = 12000;   /* at the floor: dew hold */
    for (int i = 0; i < 20; ++i) TecControl_Cb(NULL);
    assert(s_dew_hold && !s_tec_enabled && s_dac_percent == 0); /* demand 0 < 8 */
    params[NV_TEC_EN_OFF_PCT] = 20;
    nv_cache_gen++;
    TecControl_Cb(NULL);                          /* first cycle after the write */
    assert(s_dew_hold && !s_tec_enabled && s_dac_percent == 0); /* still cut */
    demand = 15;                                   /* now below EN_OFF 20 */
    TecControl_Cb(NULL);
    assert(!s_tec_enabled);
    demand = 25;                                   /* above EN_OFF 20 -> re-enable */
    TecControl_Cb(NULL);
    assert(s_tec_enabled);

    /* Cold-side EMA: the PID feedback follows the smoothed cold temperature,
     * not a single noisy ADC sample. The expected step is derived from
     * TEC_COLD_EMA_DIV rather than hardcoded, so retuning the filter (DIV 8,
     * 16, 32 ...) cannot turn this regression red: a one-cycle 1 C spike must
     * move the PID input by only 1/DIV of it. */
    reset_fixture(); cold = 45000;
    for (int i = 0; i < 8; ++i) TecControl_Cb(NULL);
    assert(pid_measure == -45000.0f);              /* EMA settled at seed */
    cold = 46000;                                   /* one-cycle spike */
    TecControl_Cb(NULL);
    assert(pid_measure == -(45000.0f + (float)(1000 / (int32_t)TEC_COLD_EMA_DIV)));
    cold = 45000;
    for (int i = 0; i < 40; ++i) TecControl_Cb(NULL);
    assert(pid_measure > -45020.0f && pid_measure <= -45000.0f);

    /* Fixed target offset (TARGET_OFFSET): an UNSIGNED 0..20000 magnitude that
     * the firmware SUBTRACTS - it buys a bounded excursion below the dew-point
     * floor rather than raising the target. Floor here is dew(50000) +
     * margin(2000) = 52000, above the 45000 base target, so the floor clamp
     * sets 52000 first; the offset then subtracts from that.
     * 0 keeps the legacy behaviour. */
    reset_fixture(); cold = 12000; dew = 50000;
    s_p.target_offset_millic = 0;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert(s_dew_hold && target_mc == 52000);        /* legacy: target == floor */
    s_p.target_offset_millic = 5000;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert(s_dew_hold && target_mc == 47000);        /* 5 C BELOW the floor */
    s_p.target_offset_millic = TEC_TARGET_OFFSET_MAX_MILLIC;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert(target_mc == 52000 - TEC_TARGET_OFFSET_MAX_MILLIC);   /* cap = 20 C */

    /* The hot-side foldback lift is applied AFTER the offset, so no target
     * offset can cancel the heatsink derating. hot 85000 is 5 C above the
     * 80000 knee; gain 2.0 -> lift 10000, which must survive in full. */
    reset_fixture(); cold = 30000; dew = 10000; hot = 85000;
    s_p.target_offset_millic = 0;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert((s_alarm & TEC_ALARM_HOT_WARNING) && target_mc == 55000);
    s_p.target_offset_millic = TEC_TARGET_OFFSET_MAX_MILLIC;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert((s_alarm & TEC_ALARM_HOT_WARNING) && target_mc == 35000);
    /* Undo the offset again: the lift is still exactly the full 10000, i.e.
     * nothing was absorbed while the offset was active. */
    s_p.target_offset_millic = 0;
    for (int i = 0; i < 3; ++i) TecControl_Cb(NULL);
    assert(target_mc == 55000);

    /* Range gate: below 0 or above the cap falls back to the 0 default, so a
     * corrupt/misscaled slot cannot sink the target arbitrarily. */
    params[NV_TEC_TARGET_OFFSET_MILLIC] = 0u;
    tec_params_load(&s_p);
    assert(s_p.target_offset_millic == 0);
    params[NV_TEC_TARGET_OFFSET_MILLIC] = TEC_TARGET_OFFSET_MAX_MILLIC;
    tec_params_load(&s_p);
    assert(s_p.target_offset_millic == TEC_TARGET_OFFSET_MAX_MILLIC);
    params[NV_TEC_TARGET_OFFSET_MILLIC] = (uint32_t)(int32_t)-1;   /* -1 */
    tec_params_load(&s_p);
    assert(s_p.target_offset_millic == 0);
    params[NV_TEC_TARGET_OFFSET_MILLIC] = TEC_TARGET_OFFSET_MAX_MILLIC + 1u;
    tec_params_load(&s_p);
    assert(s_p.target_offset_millic == 0);

    /* Cold-sensor fault recovery must reseed the EMA, not just the PID: the
     * filter is frozen for the whole fault, so without the reseed the first
     * post-recovery cycle would reset the PID to the real temperature and
     * then immediately feed it the pre-fault value. */
    reset_fixture(); cold = 30000;
    for (int i = 0; i < 20; ++i) TecControl_Cb(NULL);
    assert(pid_measure == -30000.0f);
    cold = -40000;                                  /* open sensor, 40 cycles */
    for (int i = 0; i < 40; ++i) TecControl_Cb(NULL);
    cold = 45000;                                   /* recovered, really 45 C */
    TecControl_Cb(NULL);
    assert(reset_measure == -45000.0f);             /* PID reset from raw */
    assert(pid_measure == -45000.0f);               /* EMA reseeded to match */

    /* Same contract on the fan ramp: an invalid hot reading freezes its EMA
     * (the fault path returns before the filter update), so recovery must
     * reseed it or the fan restarts from a pre-fault temperature. */
    reset_fixture(); hot = 35000;
    for (int i = 0; i < 20; ++i) TecControl_Cb(NULL);
    assert(fan_duty_seen == 30);                    /* base airflow */
    hot = 200000;                                   /* out of the valid window */
    for (int i = 0; i < 40; ++i) TecControl_Cb(NULL);
    assert(fan_duty_seen == 100);                   /* fault: full airflow */
    hot = 50000;                                    /* recovered: ramp says 65 */
    TecControl_Cb(NULL);
    assert(fan_duty_seen == tec_fan_duty_from_hot(50000));
    assert(fan_duty_seen == 65);

    /* Fan noise shaping (EMA + hysteresis). With hot fixed at 42 C the pure
     * ramp gives 30 + (2/20)*70 = 37 %. A small hot-side nudge that moves
     * the demand by ~1 % must NOT move the fan (inside the 2 % deadband); a
     * real +2 C step must cross the band and move the duty up. */
    reset_fixture(); hot = 42000;
    for (int i = 0; i < 8; ++i) TecControl_Cb(NULL);
    assert(fan_duty_seen == 37);
    hot = 42300;                                  /* +0.3 C -> ~+1 % demand */
    for (int i = 0; i < 8; ++i) TecControl_Cb(NULL);
    assert(fan_duty_seen == 37);                  /* inside the hysteresis band */
    hot = 44000;                                  /* +2 C from the settled EMA */
    for (int i = 0; i < 30; ++i) TecControl_Cb(NULL);
    assert(fan_duty_seen > 37);                   /* crossed the band */

    puts("TEC fault callback tests passed");
    return 0;
}
