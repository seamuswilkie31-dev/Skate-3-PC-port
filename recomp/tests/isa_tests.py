#!/usr/bin/env python3
"""Instruction tests for our XenonRecomp patches (recomp/xenonrecomp-patches).

Writes test inputs in the format XenonRecomp's test mode reads:
    <out>/bin/s3n_isa.o     big-endian ELF object, one function per test
    <out>/bin/s3n_isa.dis   "address <label>:" lines naming those functions
    <out>/s3n_isa.s         each test's register/memory inputs and expected outputs

Expected values come from the reference model below, written from the PowerPC and
VMX128 definitions rather than from the recompiler's C++, so the two check each other.
Flags follow the 32-bit conventions XenonRecomp uses everywhere (CA and CR0 come from
the low 32 bits). Test "canary" expects a wrong value on purpose: the runner checks it
fails, which proves the harness reports failures at all.

    python -I recomp/tests/isa_tests.py <out-dir>
"""

import math
import os
import struct
import sys

M16, M32, M64, M128 = 0xFFFF, 0xFFFFFFFF, (1 << 64) - 1, (1 << 128) - 1

# ---- encoders -----------------------------------------------------------------


def d_form(op, rt, ra, d):
    return op << 26 | rt << 21 | ra << 16 | (d & 0xFFFF)


def x_form(rt, ra, rb, xo, rc=0):  # also XO-form with OE=0
    return 31 << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc


def vx(xo, vd, va, vb):
    return 4 << 26 | vd << 21 | va << 16 | vb << 11 | xo


def vxr(xo, vd, va, vb, rc=0):
    return 4 << 26 | vd << 21 | va << 16 | vb << 11 | rc << 10 | xo


def vx128(op, xop, vd, va, vb):  # registers below 32 need no extension bits
    return op << 26 | (xop & 0x3D0) | vd << 21 | va << 16 | vb << 11


def li(rd, v): return d_form(14, rd, 0, v)
def addic(rd, ra, v): return d_form(12, rd, ra, v)
def cmpwi(crf, ra, v): return 11 << 26 | crf << 23 | ra << 16 | (v & 0xFFFF)
def mtctr(rs): return 0x7C0903A6 | rs << 21
def mfcr(rd): return 0x7C000026 | rd << 21
def addze(rd, ra): return x_form(rd, ra, 0, 202)


BLR = 0x4E800020
BO_DNZF, BO_DZF, BO_DNZT = 0, 2, 8
LT, GT, EQ = 0, 1, 2


def bc(bo, bi):
    """Branch to a label; resolved when the test is assembled."""
    return lambda pc, labels, target: 16 << 26 | bo << 21 | bi << 16 | ((labels[target] - pc) & 0xFFFC)


# ---- reference model ------------------------------------------------------------


def s32(x): return ((x & M32) ^ 0x80000000) - 0x80000000
def s64(x): return ((x & M64) ^ (1 << 63)) - (1 << 63)
def f32_bits(f): return struct.unpack(">I", struct.pack(">f", f))[0]
def f32_from(bits): return struct.unpack(">f", struct.pack(">I", bits))[0]
def f64_bytes(f): return list(struct.pack(">d", f))
def be_bytes(value, n): return list(value.to_bytes(n, "big"))


def cr_bits(field, lt=0, gt=0, eq=0, so=0):
    """What mfcr returns for one CR field (field 0 is the most significant nibble)."""
    nibble = lt << 3 | gt << 2 | eq << 1 | so
    return nibble << (4 * (7 - field))


def cr0_of(value):
    v = s32(value)
    return cr_bits(0, lt=v < 0, gt=v > 0, eq=v == 0)


def halves(words):
    return [h for w in words for h in (w >> 16, w & M16)]


def bytes_of(words):
    return [b for w in words for b in be_bytes(w, 4)]


def words_from_halves(hs):
    return [hs[i] << 16 | hs[i + 1] for i in range(0, 8, 2)]


def words_from_bytes(bs):
    return [int.from_bytes(bytes(bs[i:i + 4]), "big") for i in range(0, 16, 4)]


def sat(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def vctuxs(bits, scale):
    x = f32_from(bits) * 2.0 ** scale
    if math.isnan(x) or x <= 0:
        return 0
    return M32 if x >= 2.0 ** 32 else int(x)


def vcmpbfp(a_bits, b_bits):
    a, b = f32_from(a_bits), f32_from(b_bits)
    return (0 if a <= b else 0x80000000) | (0 if a >= -b else 0x40000000)


# Xbox 360 half floats have no INF/NaN: exponent 31 is an ordinary exponent.
def pack_half(bits):
    """Float -> half, truncating; NaN and |x| > 65504 saturate to 0x7FFF."""
    f = f32_from(bits)
    sign = 0x8000 if bits & 0x80000000 else 0
    a = abs(f)
    if math.isnan(f) or a > 65504.0:
        return sign | 0x7FFF
    if a < 2.0 ** -14:
        return sign | int(a / 2.0 ** -24)  # half denormal
    m, e = math.frexp(a)  # a = m * 2^e with 0.5 <= m < 1
    return sign | (e - 1 + 15) << 10 | int((2 * m - 1) * 1024)


def unpack_half(h):
    sign = -1.0 if h & 0x8000 else 1.0
    e, m = (h >> 10) & 31, h & 0x3FF
    return f32_bits(sign * (m * 2.0 ** -24 if e == 0 else (1 + m / 1024) * 2.0 ** (e - 15)))


# vpkd3d128: which packed word lands in each destination word, by pack mode and shift
# (None keeps the destination's old word). From the VMX128 pixel-writer layout.
PLACEMENT = {
    1: [[None, None, None, 3], [None, None, 3, None], [None, 3, None, None], [3, None, None, None]],
    2: [[None, None, 2, 3], [None, 2, 3, None], [2, 3, None, None], [3, None, None, None]],
    3: [[None, None, 2, 3], [None, 2, 3, None], [2, 3, None, None], [None, None, None, 2]],
}


def vpkd3d(kind, pack, shift, prev, floats):
    h = [pack_half(f) for f in floats]
    new = [0, 0, 0, h[0] << 16 | h[1]] if kind == 3 else [0, 0, h[0] << 16 | h[1], h[2] << 16 | h[3]]
    return [prev[k] if p is None else new[p] for k, p in enumerate(PLACEMENT[pack][shift])]


# ---- tests ----------------------------------------------------------------------

TESTS = []


def test(name, code, ins=(), outs=()):
    TESTS.append((name, list(code), list(ins), list(outs)))


def reg(r, value):
    if isinstance(value, float):
        return f"{r} {value!r}"
    return f"{r} 0x{value & M64:X}"


def vreg(v, words):
    return f"{v} [" + ", ".join(f"{w & M32:08X}" for w in words) + "]"


def mem(addr, data):
    return f"{addr:08X} " + " ".join(f"{b:02X}" for b in data)


def IN(*items): return [("REGISTER_IN", i) if not i.startswith("mem ") else ("MEMORY_IN", i[4:]) for i in items]
def OUT(*items): return [("REGISTER_OUT", i) if not i.startswith("mem ") else ("MEMORY_OUT", i[4:]) for i in items]


# Canary: must fail.
test("canary", [("li r3,1", li(3, 1)), ("blr", BLR)], outs=OUT(reg("r3", 2)))

# -- counter branches. CTR can't be read back (no mfctr), so outcomes show it.
for name, ctr, value, expected in [("bdzf_taken", 1, 5, 2), ("bdzf_ctr_not_zero", 2, 5, 1), ("bdzf_cond_true", 1, 0, 1)]:
    test(name, [("li r12," + str(ctr), li(12, ctr)), ("mtctr r12", mtctr(12)), ("li r11," + str(value), li(11, value)),
                ("cmpwi cr6,r11,0", cmpwi(6, 11, 0)), ("bdzf cr6.eq,L", (bc(BO_DZF, 4 * 6 + EQ), "L")),
                ("li r3,1", li(3, 1)), ("blr", BLR), "L", ("li r3,2", li(3, 2)), ("blr", BLR)],
         ins=IN(), outs=OUT(reg("r3", expected)))

# Decrement check: the first bdzf (CTR 2 -> 1) falls through, the second (1 -> 0) branches.
test("bdzf_decrements", [("li r12,2", li(12, 2)), ("mtctr r12", mtctr(12)), ("li r11,5", li(11, 5)),
                         ("cmpwi cr6,r11,0", cmpwi(6, 11, 0)), ("bdzf cr6.eq,L", (bc(BO_DZF, 26), "L")),
                         ("bdzf cr6.eq,L", (bc(BO_DZF, 26), "L")), ("li r3,1", li(3, 1)), ("blr", BLR),
                         "L", ("li r3,2", li(3, 2)), ("blr", BLR)], outs=OUT(reg("r3", 2)))

# Skate 3's dense-switch idiom: mtctr idx; cmpwi idx,0; bdzf; bdzf; bdzf; default.
for idx, expected in [(0, 99), (1, 10), (2, 20), (3, 30), (4, 99)]:
    test(f"bdzf_switch_{idx}", [("li r11," + str(idx), li(11, idx)), ("mtctr r11", mtctr(11)),
                                ("cmpwi cr6,r11,0", cmpwi(6, 11, 0)),
                                ("bdzf cr6.eq,C1", (bc(BO_DZF, 26), "C1")), ("bdzf cr6.eq,C2", (bc(BO_DZF, 26), "C2")),
                                ("bdzf cr6.eq,C3", (bc(BO_DZF, 26), "C3")), ("li r3,99", li(3, 99)), ("blr", BLR),
                                "C1", ("li r3,10", li(3, 10)), ("blr", BLR), "C2", ("li r3,20", li(3, 20)), ("blr", BLR),
                                "C3", ("li r3,30", li(3, 30)), ("blr", BLR)], outs=OUT(reg("r3", expected)))

# bdnzf must test the bit it names. r11 = 1: gt is true, eq false. The old code assumed eq
# and would branch here.
test("bdnzf_uses_its_bit", [("li r12,2", li(12, 2)), ("mtctr r12", mtctr(12)), ("li r11,1", li(11, 1)),
                            ("cmpwi cr6,r11,0", cmpwi(6, 11, 0)), ("bdnzf cr6.gt,L", (bc(BO_DNZF, 4 * 6 + GT), "L")),
                            ("li r3,1", li(3, 1)), ("blr", BLR), "L", ("li r3,2", li(3, 2)), ("blr", BLR)],
     outs=OUT(reg("r3", 1)))
for name, ctr, value, expected in [("bdnzt_taken", 2, 0, 2), ("bdnzt_ctr_zero", 1, 0, 1), ("bdnzt_cond_false", 2, 7, 1)]:
    test(name, [("li r12," + str(ctr), li(12, ctr)), ("mtctr r12", mtctr(12)), ("li r11," + str(value), li(11, value)),
                ("cmpwi cr6,r11,0", cmpwi(6, 11, 0)), ("bdnzt cr6.eq,L", (bc(BO_DNZT, 26), "L")),
                ("li r3,1", li(3, 1)), ("blr", BLR), "L", ("li r3,2", li(3, 2)), ("blr", BLR)],
         outs=OUT(reg("r3", expected)))

# -- carry arithmetic. CA is set with addic r9,r8,1 (r8 = -1 sets it, r8 = 0 clears it)
# and read back with addze r11,r0 (r0 = 0).


def with_carry(ca, body):
    return [("li r8," + ("-1" if ca else "0"), li(8, -1 if ca else 0)), ("addic r9,r8,1", addic(9, 8, 1))] + body + \
        [("addze r11,r0", addze(11, 0)), ("blr", BLR)]


for a, b in [(0xFFFFFFFF, 2), (5, 7), (0x80000000, 0x80000000)]:
    test(f"addc_{a:x}_{b:x}", with_carry(0, [("addc r3,r4,r5", x_form(3, 4, 5, 10))]),
         ins=IN(reg("r0", 0), reg("r4", a), reg("r5", b)),
         outs=OUT(reg("r3", (a + b) & M64), reg("r11", ((a & M32) + (b & M32)) >> 32)))
test("addc_record", with_carry(0, [("addc. r3,r4,r5", x_form(3, 4, 5, 10, 1)), ("mfcr r10", mfcr(10))]),
     ins=IN(reg("r0", 0), reg("r4", 0x7FFFFFFF), reg("r5", 1)),
     outs=OUT(reg("r3", 0x80000000), reg("r10", cr0_of(0x80000000)), reg("r11", 0)))

for a, ca in [(0, 0), (5, 0), (0, 1), (0xFFFFFFFF, 1)]:
    test(f"addme_{a:x}_ca{ca}", with_carry(ca, [("addme r3,r4", x_form(3, 4, 0, 234))]),
         ins=IN(reg("r0", 0), reg("r4", a)),
         outs=OUT(reg("r3", (a + ca - 1) & M64), reg("r11", ((a & M32) + ca + M32) >> 32)))

for a, ca in [(0, 1), (0, 0), (5, 1), (5, 0)]:
    test(f"subfze_{a:x}_ca{ca}", with_carry(ca, [("subfze r3,r4", x_form(3, 4, 0, 200))]),
         ins=IN(reg("r0", 0), reg("r4", a)),
         outs=OUT(reg("r3", (~a + ca) & M64), reg("r11", ((~a & M32) + ca) >> 32)))
test("subfze_record", with_carry(1, [("subfze. r3,r4", x_form(3, 4, 0, 200, 1)), ("mfcr r10", mfcr(10))]),
     ins=IN(reg("r0", 0), reg("r4", 5)),
     outs=OUT(reg("r3", (~5 + 1) & M64), reg("r10", cr0_of(~5 + 1))))

test("eqv", [("eqv r3,r4,r5", x_form(4, 3, 5, 284)), ("blr", BLR)],
     ins=IN(reg("r4", 0xF0F0), reg("r5", 0xFF00)), outs=OUT(reg("r3", ~(0xF0F0 ^ 0xFF00) & M64)))
test("eqv_record", [("eqv. r3,r4,r5", x_form(4, 3, 5, 284, 1)), ("mfcr r10", mfcr(10)), ("blr", BLR)],
     ins=IN(reg("r4", 0x1234), reg("r5", 0x1234)), outs=OUT(reg("r3", M64), reg("r10", cr0_of(M64))))

for a, b in [(-3, 0x4000000000000000), (0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF), (-1, -1), (12345, -67890)]:
    test(f"mulhd_{a & M64:x}_{b & M64:x}", [("mulhd r3,r4,r5", x_form(3, 4, 5, 73)), ("blr", BLR)],
         ins=IN(reg("r4", a), reg("r5", b)), outs=OUT(reg("r3", ((s64(a) * s64(b)) >> 64) & M64)))
for a, b in [(M64, M64), (0x123456789ABCDEF0, 0xFEDCBA9876543210), (1 << 63, 2)]:
    test(f"mulhdu_{a:x}_{b:x}", [("mulhdu r3,r4,r5", x_form(3, 4, 5, 9)), ("blr", BLR)],
         ins=IN(reg("r4", a), reg("r5", b)), outs=OUT(reg("r3", (a * b) >> 64)))

# -- update-form loads and stores. Each test gets its own memory.
addr = 0x10000


def area():
    global addr
    addr += 0x100
    return addr


a = area()
test("lhzu", [("lhzu r3,2(r4)", d_form(41, 3, 4, 2)), ("blr", BLR)],
     ins=IN(reg("r4", a), "mem " + mem(a, [0, 0, 0xAB, 0xCD])), outs=OUT(reg("r3", 0xABCD), reg("r4", a + 2)))
a = area()
test("lhau", [("lhau r3,2(r4)", d_form(43, 3, 4, 2)), ("blr", BLR)],
     ins=IN(reg("r4", a), "mem " + mem(a, [0, 0, 0x80, 0x01])), outs=OUT(reg("r3", -0x7FFF), reg("r4", a + 2)))
a = area()
test("sthu", [("sthu r5,-2(r4)", d_form(45, 5, 4, -2)), ("blr", BLR)],
     ins=IN(reg("r4", a + 4), reg("r5", 0x12345678)), outs=OUT(reg("r4", a + 2), "mem " + mem(a + 2, [0x56, 0x78])))
a = area()
test("lfsu", [("lfsu f1,4(r4)", d_form(49, 1, 4, 4)), ("blr", BLR)],
     ins=IN(reg("r4", a), "mem " + mem(a + 4, be_bytes(f32_bits(1.5), 4))), outs=OUT(reg("f1", 1.5), reg("r4", a + 4)))
a = area()
test("lfdu", [("lfdu f1,8(r4)", d_form(51, 1, 4, 8)), ("blr", BLR)],
     ins=IN(reg("r4", a), "mem " + mem(a + 8, f64_bytes(-2.5))), outs=OUT(reg("f1", -2.5), reg("r4", a + 8)))
a = area()
test("stfsu", [("stfsu f1,4(r4)", d_form(53, 1, 4, 4)), ("blr", BLR)],
     ins=IN(reg("r4", a), reg("f1", 0.75)), outs=OUT(reg("r4", a + 4), "mem " + mem(a + 4, be_bytes(f32_bits(0.75), 4))))
a = area()
test("stfdu", [("stfdu f1,8(r4)", d_form(55, 1, 4, 8)), ("blr", BLR)],
     ins=IN(reg("r4", a), reg("f1", -2.0)), outs=OUT(reg("r4", a + 8), "mem " + mem(a + 8, f64_bytes(-2.0))))

data = list(range(0x81, 0x91))  # distinctive bytes, top bit set to catch sign-extension bugs
for name, xo, size in [("lbzux", 119, 1), ("lhzux", 311, 2), ("lwzux", 55, 4)]:
    a = area()
    test(name, [(f"{name} r3,r4,r5", x_form(3, 4, 5, xo)), ("blr", BLR)],
         ins=IN(reg("r4", a), reg("r5", 4), "mem " + mem(a, data)),
         outs=OUT(reg("r3", int.from_bytes(bytes(data[4:4 + size]), "big")), reg("r4", a + 4)))
a = area()
test("lfsux", [("lfsux f2,r4,r5", x_form(2, 4, 5, 567)), ("blr", BLR)],
     ins=IN(reg("r4", a), reg("r5", 8), "mem " + mem(a + 8, be_bytes(f32_bits(-0.125), 4))),
     outs=OUT(reg("f2", -0.125), reg("r4", a + 8)))
for name, xo, size in [("stbux", 247, 1), ("sthux", 439, 2), ("stdux", 181, 8)]:
    a = area()
    value = 0x0102030405060708
    test(name, [(f"{name} r6,r4,r5", x_form(6, 4, 5, xo)), ("blr", BLR)],
         ins=IN(reg("r4", a), reg("r5", 4), reg("r6", value)),
         outs=OUT(reg("r4", a + 4), "mem " + mem(a + 4, be_bytes(value & ((1 << (8 * size)) - 1), size))))
a = area()
test("stfsux", [("stfsux f1,r4,r5", x_form(1, 4, 5, 695)), ("blr", BLR)],
     ins=IN(reg("r4", a), reg("r5", 12), reg("f1", 3.0)),
     outs=OUT(reg("r4", a + 12), "mem " + mem(a + 12, be_bytes(f32_bits(3.0), 4))))

# lvehx loads the aligned quadword; the halfword lands in its element.
a = area()
quad = list(range(16))
test("lvehx", [("lvehx v1,r4,r5", x_form(1, 4, 5, 39)), ("blr", BLR)],
     ins=IN(reg("r4", a), reg("r5", 6), "mem " + mem(a, quad)), outs=OUT(vreg("v1", words_from_bytes(quad))))

# -- vector ops. Words are listed in guest element order (element 0 first).
A = [0x80000001, 0x7FFFFFF0, 0x00000010, 0xFFFFFFFF]
B = [0x00000002, 0x00000020, 0xFFFFFFF0, 0x7FFFFFFF]
MASK = [0xFFFF0000, 0x00FF00FF, 0x0F0F0F0F, 0x00000000]

test("vnor128", [("vnor128 v1,v2,v3", vx128(5, 656, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", [~(a | b) & M32 for a, b in zip(A, B)])))
test("vsel128", [("vsel128 v1,v2,v3,v1", vx128(5, 848, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v1", MASK), vreg("v2", A), vreg("v3", B)),
     outs=OUT(vreg("v1", [(a & ~m | b & m) & M32 for a, b, m in zip(A, B, MASK)])))
test("vsububm", [("vsububm v1,v2,v3", vx(1024, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)),
     outs=OUT(vreg("v1", words_from_bytes([(a - b) & 0xFF for a, b in zip(bytes_of(A), bytes_of(B))]))))
test("vsubuwm", [("vsubuwm v1,v2,v3", vx(1152, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", [(a - b) & M32 for a, b in zip(A, B)])))
test("vaddsws", [("vaddsws v1,v2,v3", vx(896, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)),
     outs=OUT(vreg("v1", [sat(s32(a) + s32(b), -2**31, 2**31 - 1) & M32 for a, b in zip(A, B)])))
test("vcmpgtuw", [("vcmpgtuw v1,v2,v3", vxr(646, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", [M32 if a > b else 0 for a, b in zip(A, B)])))
for name, va, vb in [("mixed", A, B), ("all", [5, 6, 7, 0x80000000], [1, 2, 3, 4]), ("none", [1, 1, 1, 1], [9, 9, 9, 9])]:
    gt = [a > b for a, b in zip(va, vb)]
    test(f"vcmpgtuw_record_{name}", [("vcmpgtuw. v1,v2,v3", vxr(646, 1, 2, 3, 1)), ("mfcr r10", mfcr(10)), ("blr", BLR)],
         ins=IN(vreg("v2", va), vreg("v3", vb)),
         outs=OUT(vreg("v1", [M32 if g else 0 for g in gt]), reg("r10", cr_bits(6, lt=all(gt), eq=not any(gt)))))
COUNTS = [0x00000004, 0x0000001F, 0x00000021, 0x00000000]  # only the low 5 bits count
test("vrlw128", [("vrlw128 v1,v2,v3", vx128(6, 80, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", COUNTS)),
     outs=OUT(vreg("v1", [((a << (c & 31)) | (a >> (32 - (c & 31)))) & M32 for a, c in zip(A, COUNTS)])))
for shift in (0, 1, 3, 7):
    x = int.from_bytes(bytes(bytes_of(A)), "big")
    shifted = (x << shift) & M128
    test(f"vsl_{shift}", [("vsl v1,v2,v3", vx(452, 1, 2, 3)), ("blr", BLR)],
         ins=IN(vreg("v2", A), vreg("v3", [shift * 0x01010101] * 4)),
         outs=OUT(vreg("v1", words_from_bytes(be_bytes(shifted, 16)))))
for imm in (-3, 5, -16, 15):
    test(f"vspltish_{imm & 0x1F}", [("vspltish v1," + str(imm), vx(844, 1, imm & 0x1F, 0)), ("blr", BLR)],
         outs=OUT(vreg("v1", words_from_halves([imm & M16] * 8))))

FLOATS = [0x7FC00000, f32_bits(-1.0), f32_bits(3.7), f32_bits(3e9)]  # NaN, negative, normal, >= 2^31
for scale, floats in [(0, FLOATS), (1, [f32_bits(1.5), f32_bits(2.0**31), f32_bits(-0.25), f32_bits(5e9)])]:
    test(f"vcfpuxws128_scale{scale}", [(f"vcfpuxws128 v1,v2,{scale}", 6 << 26 | 0x270 | 1 << 21 | scale << 16 | 2 << 11),
                                       ("blr", BLR)],
         ins=IN(vreg("v2", floats)), outs=OUT(vreg("v1", [vctuxs(f, scale) for f in floats])))

test("vpkswss128", [("vpkswss128 v1,v2,v3", vx128(5, 640, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)),
     outs=OUT(vreg("v1", words_from_halves([sat(s32(w), -32768, 32767) & M16 for w in A + B]))))
test("vpkuwum128", [("vpkuwum128 v1,v2,v3", vx128(5, 896, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", words_from_halves([w & M16 for w in A + B]))))
test("vpkuwus128", [("vpkuwus128 v1,v2,v3", vx128(5, 960, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", words_from_halves([min(w, M16) for w in A + B]))))
test("vpkuhum128", [("vpkuhum128 v1,v2,v3", vx128(5, 768, 1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v1", words_from_bytes([h & 0xFF for h in halves(A) + halves(B)]))))
# Aliasing: the destination is also a source.
test("vpkuwum128_alias", [("vpkuwum128 v2,v2,v3", vx128(5, 896, 2, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", A), vreg("v3", B)), outs=OUT(vreg("v2", words_from_halves([w & M16 for w in A + B]))))

BOUND_A = [f32_bits(0.5), f32_bits(2.0), f32_bits(-3.0), 0x7FC00000]
BOUND_B = [f32_bits(1.0)] * 4
for name, enc in [("vcmpbfp", vxr(966, 1, 2, 3)), ("vcmpbfp128", vx128(6, 384, 1, 2, 3))]:
    test(name, [(name + " v1,v2,v3", enc), ("blr", BLR)],
         ins=IN(vreg("v2", BOUND_A), vreg("v3", BOUND_B)),
         outs=OUT(vreg("v1", [vcmpbfp(a, b) for a, b in zip(BOUND_A, BOUND_B)])))
for name, va in [("out", BOUND_A), ("in", [f32_bits(x) for x in (0.5, -1.0, 1.0, 0.0)])]:
    result = [vcmpbfp(a, b) for a, b in zip(va, BOUND_B)]
    test(f"vcmpbfp128_record_{name}", [("vcmpbfp128. v1,v2,v3", vx128(6, 448, 1, 2, 3)), ("mfcr r10", mfcr(10)), ("blr", BLR)],
         ins=IN(vreg("v2", va), vreg("v3", BOUND_B)),
         outs=OUT(vreg("v1", result), reg("r10", cr_bits(6, eq=not any(result)))))

# -- D3D half-float packs. The destination starts with marker words so untouched words show.
PREV = [0x11111111, 0x22222222, 0x33333333, 0x44444444]
HALF_SOURCES = [
    [f32_bits(1.5), f32_bits(-2.25), f32_bits(1e-5), f32_bits(70000.0)],      # normal, half denormal, saturating
    [0x7FC00000, f32_bits(-0.0), f32_bits(65504.0), f32_bits(3e-8)],          # NaN, -0, largest, underflow
]


def vpkd3d128(vd, vb, kind, pack, shift):
    return 6 << 26 | 0x610 | vd << 21 | (kind << 2 | pack) << 16 | vb << 11 | shift << 6


for kind in (3, 5):
    for pack in (1, 2, 3):
        for shift in range(4):
            src = HALF_SOURCES[(pack + shift) % 2]
            test(f"vpkd3d128_t{kind}_p{pack}_s{shift}",
                 [(f"vpkd3d128 v1,v2,{kind},{pack},{shift}", vpkd3d128(1, 2, kind, pack, shift)), ("blr", BLR)],
                 ins=IN(vreg("v1", PREV), vreg("v2", src)),
                 outs=OUT(vreg("v1", vpkd3d(kind, pack, shift, PREV, src))))
# The source register is also the destination.
test("vpkd3d128_alias", [("vpkd3d128 v2,v2,5,3,2", vpkd3d128(2, 2, 5, 3, 2)), ("blr", BLR)],
     ins=IN(vreg("v2", HALF_SOURCES[0])), outs=OUT(vreg("v2", vpkd3d(5, 3, 2, HALF_SOURCES[0], HALF_SOURCES[0]))))


def vupkd3d128(vd, vb, kind):
    return 6 << 26 | 0x7F0 | vd << 21 | (kind << 2) << 16 | vb << 11


for name, hs in [("a", [0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0x3C00, 0x8001, 0x7C00, 0xFBFF]),   # 1, -2^-24, 65536, -65504
                 ("b", [0x1111, 0x2222, 0x3333, 0x4444, 0x0000, 0x8000, 0x7FFF, 0x3555])]:  # 0, -0, 131008, ~0.333
    test(f"vupkd3d128_t5_{name}", [("vupkd3d128 v1,v2,20", vupkd3d128(1, 2, 5)), ("blr", BLR)],
         ins=IN(vreg("v2", words_from_halves(hs))), outs=OUT(vreg("v1", [unpack_half(h) for h in hs[4:]])))
HS2 = [0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x4248, 0xC000]  # x = 3.140625, y = -2
test("vupkd3d128_t3", [("vupkd3d128 v1,v2,12", vupkd3d128(1, 2, 3)), ("blr", BLR)],
     ins=IN(vreg("v2", words_from_halves(HS2))),
     outs=OUT(vreg("v1", [unpack_half(HS2[6]), unpack_half(HS2[7]), f32_bits(0.0), f32_bits(1.0)])))
test("vupkd3d128_alias", [("vupkd3d128 v2,v2,20", vupkd3d128(2, 2, 5)), ("blr", BLR)],
     ins=IN(vreg("v2", words_from_halves(HS2))), outs=OUT(vreg("v2", [unpack_half(h) for h in HS2[4:]])))


# ---- output ---------------------------------------------------------------------


def assemble(base):
    """Lays out every test; returns (code bytes, [(name, address)])."""
    words, starts = [], []
    pc = base
    for name, code, _, _ in TESTS:
        starts.append((name, pc))
        labels, layout = {}, []
        for item in code:  # pass 1: label addresses
            if isinstance(item, str):
                labels[item] = pc
            else:
                layout.append((pc, item))
                pc += 4
        for at, (_, enc) in layout:  # pass 2: encode
            words.append(enc[0](at, labels, enc[1]) if isinstance(enc, tuple) else enc)
    return b"".join(struct.pack(">I", w) for w in words), starts


def elf(text, addr):
    """Minimal big-endian ELF32 PowerPC object holding one executable .text section."""
    shstrtab = b"\0.text\0.shstrtab\0"
    text_off = 52
    shstr_off = text_off + len(text)
    sh_off = (shstr_off + len(shstrtab) + 3) & ~3
    header = struct.pack(">16sHHIIIIIHHHHHH", b"\x7fELF\x01\x02\x01" + bytes(9), 1, 20, 1, 0, 0, sh_off, 0,
                         52, 0, 0, 40, 3, 2)
    sections = [
        bytes(40),
        struct.pack(">IIIIIIIIII", 1, 1, 6, addr, text_off, len(text), 0, 0, 4, 0),       # .text: PROGBITS, AX
        struct.pack(">IIIIIIIIII", 7, 3, 0, 0, shstr_off, len(shstrtab), 0, 0, 1, 0),     # .shstrtab
    ]
    body = header + text + shstrtab
    return body + bytes(sh_off - len(body)) + b"".join(sections)


def main(out):
    os.makedirs(os.path.join(out, "bin"), exist_ok=True)
    base = 0x1000
    text, starts = assemble(base)
    with open(os.path.join(out, "bin", "s3n_isa.o"), "wb") as f:
        f.write(elf(text, base))
    with open(os.path.join(out, "bin", "s3n_isa.dis"), "w", newline="\n") as f:
        for name, address in starts:
            f.write(f"{address:08x} <test_{name}>:\n")
    checks = 0
    with open(os.path.join(out, "s3n_isa.s"), "w", newline="\n") as f:
        for name, code, ins, outs in TESTS:
            f.write(f"test_{name}:\n")
            for kind, value in ins:
                f.write(f"  #_ {kind} {value}\n")
            for item in code:
                if not isinstance(item, str):
                    f.write(f"  {item[0]}\n")
            for kind, value in outs:
                f.write(f"  #_ {kind} {value}\n")
                checks += 4 if "[" in value else len(value.split()) - 1 if kind == "MEMORY_OUT" else 1
            f.write("\n")
    print(f"{len(TESTS)} tests, {checks} checks, {len(text) // 4} instructions -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
