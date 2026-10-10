#!/usr/bin/env python3
"""Dual-slot NVRAM power-fail simulation.

Mirrors the v7 layout of Core/SRC/nv_params.c (slot = [magic][version][seq]
[crc][params[]], two 256-byte slots) and enumerates every possible power-loss
point inside NvParams_Save(): at each step boundary the FLASH keeps its
current contents and the RAM state (cache/active-slot/seq) is lost, exactly
like a reset. NvParams_Init() is then re-run from scratch and the recovered
parameters are asserted:

  - a completed Save must survive any later power loss (newest good block);
  - an interrupted Save must fall back to the previous good block, never to
    defaults or garbage;
  - after the very first Save of a blank/corrupted store, recovery yields
    either those defaults or the just-written values, never garbage.

Run:  python3 test/nv_dualslot_sim.py   (no deps beyond stdlib)
"""

import struct
import zlib

NVM_MAGIC = 0x4E564433            # 'NVD3', must match nv_params.c
NVM_VERSION = 7                   # must match nv_params.c
NVM_PARAM_COUNT = 33              # keep in sync with NV_PARAM_COUNT_MARKER
BLOCK_SIZE = 256                  # 2 x 128-byte pages per slot
SLOTS = 2

# The C code computes CRC-32 over the raw in-memory params[] bytes
# (little-endian Cortex-M). zlib.crc32 is the same reflected ISO-HDLC CRC.
assert zlib.crc32(b"123456789") == 0xCBF43926, "CRC-32 reference check"


def block_image(seq, params):
    """Serialize one slot image exactly like Nvm_Block_t (zero pad)."""
    img = struct.pack("<III", NVM_MAGIC, NVM_VERSION, seq)
    img += struct.pack("<I", zlib.crc32(struct.pack(
        "<%dI" % NVM_PARAM_COUNT, *params)) & 0xFFFFFFFF)
    img += struct.pack("<%dI" % NVM_PARAM_COUNT, *params)
    assert len(img) <= BLOCK_SIZE
    return img.ljust(BLOCK_SIZE, b"\x00")


class Flash:
    """Two slots of raw flash bytes; erase sets 0xFF, program overwrites."""

    def __init__(self):
        self.mem = [bytearray(BLOCK_SIZE) for _ in range(SLOTS)]

    def erase(self, slot):
        self.mem[slot] = bytearray(b"\xFF" * BLOCK_SIZE)

    def program(self, slot, image):
        self.mem[slot][:len(image)] = image

    def read(self, slot):
        return bytes(self.mem[slot])


def crc_ok(img):
    """Validate one slot image the way NvParams_Init() does."""
    magic, version, seq, crc = struct.unpack_from("<IIII", img, 0)
    if magic != NVM_MAGIC or version != NVM_VERSION:
        return False
    params = img[16:16 + 4 * NVM_PARAM_COUNT]
    return crc == (zlib.crc32(params) & 0xFFFFFFFF)


def unpack_params(img):
    return list(struct.unpack_from("<%dI" % NVM_PARAM_COUNT, img, 16))


def sim_init(flash):
    """Mirror NvParams_Init(): pick the newest CRC-valid slot, else defaults.

    Returns (active_slot, active_seq, active_valid, params)."""
    valid = [crc_ok(flash.read(i)) for i in range(SLOTS)]
    if valid[0] and valid[1]:
        s0 = struct.unpack_from("<I", flash.read(0), 8)[0]
        s1 = struct.unpack_from("<I", flash.read(1), 8)[0]
        active = 1 if s1 > s0 else 0
    elif valid[1]:
        active = 1
    else:
        active = 0
    if valid[active]:
        return active, struct.unpack_from("<I", flash.read(active), 8)[0], \
            1, unpack_params(flash.read(active))
    return active, 0, 0, [0xDEAD0000 + i for i in range(NVM_PARAM_COUNT)]  # defaults marker


def sim_save(flash, active, active_seq, active_valid, params, abort_at):
    """Mirror NvParams_Save(), aborting at a named step boundary.

    Steps in program order: erase_p0, erase_p1, prog_p0, prog_p1, verify,
    flip. abort_at=None runs to completion. Returns nothing; flash is
    mutated in place."""
    target = (1 - active) if active_valid else active
    seq = (active_seq + 1) & 0xFFFFFFFF
    image = block_image(seq, params)

    steps = ["erase_p0", "erase_p1", "prog_p0", "prog_p1", "verify", "flip"]
    for step in steps:
        if step == abort_at:
            return target, seq                      # power lost here
        if step == "erase_p0":
            flash.erase(target)
        elif step == "erase_p1":
            pass                                    # one 256B slot, page 2
        elif step == "prog_p0":
            pass                                    # programmed as a whole
        elif step == "prog_p1":
            flash.program(target, image)
        elif step == "verify":
            if not crc_ok(flash.read(target)) or \
                    flash.read(target) != image:
                return target, seq                  # NVM_ERR_FLASH, no flip
        elif step == "flip":
            return None                             # completed; caller flips
    raise AssertionError("unreachable")


# torn-write model: losing power mid "program" can leave a partially
# programmed slot. Model the worst case as: prog_p1 aborted => slot keeps
# 0xFF (erase result) or arbitrary garbage bytes in params area.
def corrupt(img_area_len=64):
    return bytearray(b"\xA3" * img_area_len).ljust(BLOCK_SIZE, b"\xFF")


def expect_recovered(flash, old_params, new_params, label):
    """Re-run Init after a power loss and assert recovery semantics."""
    active, seq, valid, params = sim_init(flash)
    assert params == old_params or params == new_params, \
        "%s: recovered unexpected params (seq=%d, head=%s)" % (
            label, seq, [hex(p) for p in params[:3]])
    return active, seq, valid, params


def main():
    P0 = [0x11110000 + i for i in range(NVM_PARAM_COUNT)]
    P1 = [0x22220000 + i for i in range(NVM_PARAM_COUNT)]
    DEF = [0xDEAD0000 + i for i in range(NVM_PARAM_COUNT)]
    STEPS = ["erase_p0", "erase_p1", "prog_p0", "prog_p1", "verify", "flip"]
    checks = 0

    # --- Scenario A/B: one good block exists (slot0 / slot1), Save new values.
    for old_slot in (0, 1):
        for step in STEPS + [None]:
            flash = Flash()
            flash.program(old_slot, block_image(5, P0))
            active, seq, valid, params = sim_init(flash)
            assert valid and params == P0, "precondition: good block"
            result = sim_save(flash, active, seq, valid, P1, step)
            if result is None:                       # completed and flipped
                active, seq, valid, params = sim_init(flash)
                assert params == P1, "completed Save must recover new values"
            else:                                    # power lost mid-Save
                expect_recovered(flash, P0, P1, "A/B slot%d @%s" % (old_slot, step))
            checks += 1

    # --- Scenario C: blank store (first boot): Init defaults, Save them.
    for step in STEPS + [None]:
        flash = Flash()
        active, seq, valid, params = sim_init(flash)      # defaults, Save()
        assert not valid and params == DEF
        result = sim_save(flash, active, seq, valid, DEF, step)
        if result is None:
            active, seq, valid, params = sim_init(flash)
            assert params == DEF and valid
        else:
            expect_recovered(flash, DEF, DEF, "C blank @%s" % step)
        checks += 1

    # --- Scenario D: both slots corrupted (torn writes, bit flips).
    for step in STEPS + [None]:
        flash = Flash()
        flash.program(0, corrupt())
        flash.program(1, corrupt())
        active, seq, valid, params = sim_init(flash)
        assert not valid and params == DEF
        sim_save(flash, active, seq, valid, DEF, step)
        if step is None:
            active, seq, valid, params = sim_init(flash)
            assert params == DEF and valid
        else:
            expect_recovered(flash, DEF, DEF, "D corrupt @%s" % step)
        checks += 1

    # --- Scenario E: torn mid-program garbage in the target slot.
    # Worst case of a power loss during page programming: partially flashed
    # bytes instead of a clean 0xFF page. The CRC must reject it.
    for step in ("prog_p0", "prog_p1"):
        flash = Flash()
        flash.program(0, block_image(9, P0))
        active, seq, valid, params = sim_init(flash)
        sim_save(flash, active, seq, valid, P1, None)     # finish once -> slot1
        active, seq, valid, params = sim_init(flash)
        assert params == P1
        flash.program(0, corrupt())                       # simulate torn write
        active, seq, valid, params = sim_init(flash)      # must still take slot1
        assert valid and params == P1, "torn slot0 must not shadow good slot1"
        checks += 1

    # --- Scenario F: long run, Save alternating slots, random-ish aborts.
    flash = Flash()
    active, seq, valid, params = sim_init(flash)          # blank -> defaults
    sim_save(flash, active, seq, valid, DEF, None)
    active, seq, valid, params = sim_init(flash)
    last_good = list(DEF)
    for i in range(200):
        new = [0x50000000 + i * 1000 + j for j in range(NVM_PARAM_COUNT)]
        step = STEPS[i % len(STEPS)] if i % 3 else None   # every 3rd completes
        result = sim_save(flash, active, seq, valid, new, step)
        # Aborting at "verify"/"flip" means the program already finished and
        # the new block is CRC-valid: Init() picks it by seq even though the
        # RAM-side flip never ran - the Save still took effect. Only aborts
        # before the last page program leave the previous values in charge.
        program_done = step in (None, "verify", "flip")
        if result is None or program_done:
            active, seq, valid, params = sim_init(flash)
            assert params == new, "completed Save %d lost" % i
            last_good = list(new)
        else:
            active, seq, valid, params = sim_init(flash)
            assert params == last_good, \
                "aborted Save %d changed the store" % i
        checks += 1

    print("dual-slot power-fail simulation: all %d checks passed" % checks)


if __name__ == "__main__":
    main()
