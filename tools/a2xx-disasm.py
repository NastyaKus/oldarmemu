#!/usr/bin/env python3
"""Disassemble an a2xx (Adreno 200) shader given as hex dwords.

  a2xx-disasm.py dw0 dw1 ...      (as printed by the adreno trace)
Field layouts follow freedreno's instr-a2xx.h / Xenia's ucode.h.
"""
import sys, struct

VOP = "ADD MUL MAX MIN SETE SETGT SETGTE SETNE FRAC TRUNC FLOOR MULADD CNDE CNDGTE CNDGT DOT4 DOT3 DOT2ADD CUBE MAX4 PRED_SETE_PUSH PRED_SETNE_PUSH PRED_SETGT_PUSH PRED_SETGTE_PUSH KILLE KILLGT KILLGTE KILLNE DST MOVA".split()
SOP = ("ADDs ADD_PREVs MULs MUL_PREVs MUL_PREV2s MAXs MINs SETEs SETGTs SETGTEs SETNEs FRACs TRUNCs FLOORs EXP_IEEE LOG_CLAMP LOG_IEEE RECIP_CLAMP RECIP_FF RECIP_IEEE RECIPSQ_CLAMP RECIPSQ_FF RECIPSQ_IEEE MOVAs MOVA_FLOORs SUBs SUB_PREVs PRED_SETEs PRED_SETNEs PRED_SETGTs PRED_SETGTEs PRED_SET_INVs PRED_SET_POPs PRED_SET_CLRs PRED_SET_RESTOREs KILLEs KILLGTs KILLGTEs KILLNEs KILLONEs SQRT_IEEE ? MUL_CONST_0 MUL_CONST_1 ADD_CONST_0 ADD_CONST_1 SUB_CONST_0 SUB_CONST_1 SIN COS RETAIN_PREV").split()
CF = "NOP EXEC EXEC_END COND_EXEC COND_EXEC_END COND_PRED_EXEC COND_PRED_EXEC_END LOOP_START LOOP_END COND_CALL RETURN COND_JMP ALLOC COND_EXEC_PRED_CLEAN COND_EXEC_PRED_CLEAN_END MARK_VS_FETCH_DONE".split()

def swz(s):
    return ''.join("xyzw"[((s >> (2*i)) + i) & 3] for i in range(4))

def src(sel, reg, s, neg):
    t = ('R%d' % (reg & 63) + ('[aL]' if reg & 64 else '')) if sel else 'C%d' % reg
    if sel and reg & 128: t = '|%s|' % t
    return ('-' if neg else '') + t + '.' + swz(s)

def alu(w):
    d0, d1, d2 = w
    vd, sd, exp = d0 & 63, (d0 >> 8) & 63, (d0 >> 15) & 1
    vm, sm, sop = (d0 >> 16) & 15, (d0 >> 20) & 15, (d0 >> 26) & 63
    sw = [(d1 >> 16) & 255, (d1 >> 8) & 255, d1 & 255]
    ng = [(d1 >> 26) & 1, (d1 >> 25) & 1, (d1 >> 24) & 1]
    rg = [(d2 >> 16) & 255, (d2 >> 8) & 255, d2 & 255]
    vop = (d2 >> 24) & 31
    sel = [(d2 >> 31) & 1, (d2 >> 30) & 1, (d2 >> 29) & 1]
    pred = (' (pred %s)' % ('T' if (d1 >> 27) & 1 else 'F')) if (d1 >> 28) & 1 else ''
    dst = lambda r, m: ('export%d' % r if exp else 'R%d' % r) + '.' + ''.join("xyzw"[i] if m >> i & 1 else '_' for i in range(4))
    s = "ALU  %-8s %s = %s, %s, %s%s" % (VOP[vop] if vop < len(VOP) else vop, dst(vd, vm),
        src(sel[0], rg[0], sw[0], ng[0]), src(sel[1], rg[1], sw[1], ng[1]), src(sel[2], rg[2], sw[2], ng[2]), pred)
    if sm or not vm:
        s += "\n          %-8s %s = src3.wx" % (SOP[sop] if sop < len(SOP) else sop, dst(sd, sm))
    if (d0 >> 24) & 1: s += ' CLAMP'
    return s

def fetch(w):
    d0, d1, d2 = w
    op = d0 & 31
    dst = (d0 >> 12) & 63
    dsw = ''.join("xyzw01?_"[(d1 >> (3*i)) & 7] for i in range(4))
    if op == 0:
        ci = ((d0 >> 20) & 31) * 3 + ((d0 >> 25) & 3)
        return "VFETCH R%d.%s = R%d.%s const %d fmt %d stride %d off %d%s" % (dst, dsw, (d0 >> 5) & 63,
            "xyzw"[(d0 >> 30) & 3], ci, (d1 >> 16) & 63, d2 & 255, (d2 >> 8) & 0x3fffff, ' mini' if (d1 >> 30) & 1 else '')
    ssw = ''.join("xyzw"[(d0 >> (26 + 2*i)) & 3] for i in range(3))
    return "TFETCH%d R%d.%s = R%d.%s tex %d mag %d min %d%s" % (op, dst, dsw, (d0 >> 5) & 63, ssw,
        (d0 >> 20) & 31, (d1 >> 12) & 3, (d1 >> 14) & 3, ' denorm' if (d0 >> 25) & 1 else '')

def disasm(dw):
    def cf(i):
        p = dw[3*(i >> 1):3*(i >> 1)+3]
        if i & 1:
            lo, hi = ((p[1] >> 16) | (p[2] << 16)) & 0xffffffff, p[2] >> 16
        else:
            lo, hi = p[0], p[1] & 0xffff
        return lo | (hi << 32)
    i, ends = 0, False
    while not ends and 3*(i >> 1) + 2 < len(dw):
        c = cf(i); op = (c >> 44) & 15
        if op in (1, 2, 3, 4, 5, 6, 13, 14):
            addr, cnt, ser = c & 0x1ff, (c >> 12) & 7, (c >> 16) & 0xfff
            print("%2d CF %s addr %d cnt %d%s" % (i, CF[op], addr, cnt,
                  (' bool %d cond %d' % ((c >> 34) & 255, (c >> 42) & 1)) if op in (3, 4, 13, 14, 5, 6) else ''))
            for k in range(cnt):
                w = dw[3*(addr+k):3*(addr+k)+3]
                if len(w) < 3: break
                print("     %2d: %s" % (addr + k, fetch(w) if (ser >> (2*k)) & 1 else alu(w)))
            ends = op in (2, 4, 6, 14)
        else:
            print("%2d CF %s %x" % (i, CF[op], c))
        i += 1

if __name__ == "__main__":
    disasm([int(x, 16) for x in sys.argv[1:]])
