#!/usr/bin/env python3
"""Replay the observed on-target failure and verify the fix (offline FSM sim).

Scenario A (before fix): TX DMA starved (no DMA2_MAP route) + stale stage1/2
latches -> the FSM sails 2-3-4-5-6, times out, retries, ends in STRAP_FAIL.
Scenario B (after fix): TX works + ResetAll at PWR_PRESS -> BOOT_SUCCESS.
Scenario C (after fix, TX still dead): honest 15s timeouts, no fast-forward.
"""
MAX = 3

def run(tx_works, detect_reset_all, glitch_reprint):
    latches = {'stage1': 0, 'stage2': 0, 'booted': 0,
               'stage3': 0, 'stage4': 0, 'stage5': 0}
    retry = 0
    st = 'TP'
    seq = ['TP']
    t = 0

    def reset345():
        for k in ('stage3', 'stage4', 'stage5'):
            latches[k] = 0

    def reset_all():
        for k in latches:
            latches[k] = 0

    def stage_timeout():
        """Real FSM: timeout -> BOOT_RESET (retry++) if retry<MAX else STRAP_FAIL."""
        nonlocal retry, st
        if retry < MAX:
            retry += 1
            st = 'BR'
        else:
            st = 'SF'
        seq.append(st)
        t = 0

    while True:
        t += 1
        if st in ('BS', 'SF'):
            return seq
        if t > 200:
            return seq + ['LOOP']
        if st == 'TP':
            if t >= 1:
                st = 'PON'; seq.append(st); t = 0; continue
        elif st == 'PON':
            st = 'PP'; seq.append(st); t = 0
            if detect_reset_all:
                reset_all()
            continue
        elif st == 'PP':
            st = 'BC'; seq.append(st); t = 0; continue
        elif st == 'BC':
            if latches['stage1']:
                st = 'S2'; seq.append(st); t = 0; continue
            if t > 15:
                stage_timeout(); continue
            latches['stage1'] = 1
        elif st == 'S2':
            if latches['booted']:
                st = 'S3'; seq.append(st); t = 0; reset345(); continue
            if t > 60:
                stage_timeout(); continue
            latches['stage2'] = 1; latches['booted'] = 1
        elif st == 'S3':
            if tx_works and t >= 1:
                latches['stage3'] = 1          # echo of the sent account
            elif glitch_reprint and t >= 1:
                latches['stage3'] = 1          # hostname re-print false hit
            if latches['stage3']:
                st = 'S4'; seq.append(st); t = 0; continue
            if t > 15:
                stage_timeout(); continue
        elif st == 'S4':
            if tx_works and t >= 2:
                latches['stage4'] = 1          # Welcome to AGNOS
            if latches['stage4']:
                st = 'S5'; seq.append(st); t = 0; continue
            if t > 15:
                stage_timeout(); continue
        elif st == 'S5':
            if tx_works and t >= 2:
                latches['stage5'] = 1          # Front Left: Playback
            if latches['stage5']:
                st = 'BS'; seq.append(st); t = 0; continue
            if t > 15:
                stage_timeout(); continue
        elif st == 'BR':
            if t > 15:
                st = 'PS'; seq.append(st); t = 0; continue
        elif st == 'PS':
            if t > 3:
                st = 'PP'; seq.append(st); t = 0
            if detect_reset_all:
                reset_all()
            continue


a = run(False, False, True)
print("A (before fix, TX dead + stale latches):", '->'.join(a))
assert a == ['TP', 'PON', 'PP', 'BC', 'S2', 'S3', 'S4', 'BR', 'PS',
             'PP', 'BC', 'S2', 'S3', 'S4', 'BR', 'PS',
             'PP', 'BC', 'S2', 'S3', 'S4', 'BR', 'PS',
             'PP', 'BC', 'S2', 'S3', 'S4', 'SF'], a
print("   reproduces the on-target log: 2-3-4-5-6, retries, STRAP_FAIL")

b = run(True, True, False)
print("B (after fix, TX works + ResetAll):     ", '->'.join(b))
assert b == ['TP', 'PON', 'PP', 'BC', 'S2', 'S3', 'S4', 'S5', 'BS'], b
print("   reaches BOOT_SUCCESS on the first attempt")

c = run(False, True, False)
print("C (after fix, TX still dead):           ", '->'.join(c))
assert 'S4' not in c and 'S5' not in c and c[-1] == 'SF', c
assert c.count('PP') == 4, c
print("   no fast-forward, honest 15s timeouts, STRAP_FAIL after 3 retries")
print("REPLAY OK")
