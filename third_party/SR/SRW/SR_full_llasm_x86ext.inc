/**
 *  SRW llasm lowering for SSE/SSE2, locked integer instructions, cmovcc,
 *  missing setcc conditions, and missing x87 instructions.
 *  Written for the macOS ARM64 port of Prototype.
 *
 *  This file is included from SR_full_llasm_instr.c. Each handled
 *  instruction becomes one call to a helper in
 *  SR/llasm-support/llasm_x86ext.c.
 *
 *  Return value of SR_llasm_x86ext():
 *     1 - instruction handled, output appended to cOutput
 *     0 - instruction not handled here
 *    -1 - instruction belongs here, but this form is not supported
 *
 *  This file is released under the same MIT license as the SR project.
 */

#define XO(x) strcat(cOutput, (x))
#define XP(...) { sprintf(cOutPart, __VA_ARGS__); strcat(cOutput, cOutPart); }

struct x86ext_map {
    enum ud_mnemonic_code mnemonic;
    const char *helper;
};

/* dst xmm, src xmm or memory */
static const struct x86ext_map x86ext_xxm[] = {
    { UD_Iaddss, "addss" }, { UD_Iaddps, "addps" }, { UD_Iaddsd, "addsd" }, { UD_Iaddpd, "addpd" },
    { UD_Isubss, "subss" }, { UD_Isubps, "subps" }, { UD_Isubsd, "subsd" }, { UD_Isubpd, "subpd" },
    { UD_Imulss, "mulss" }, { UD_Imulps, "mulps" }, { UD_Imulsd, "mulsd" }, { UD_Imulpd, "mulpd" },
    { UD_Idivss, "divss" }, { UD_Idivps, "divps" }, { UD_Idivsd, "divsd" }, { UD_Idivpd, "divpd" },
    { UD_Iminss, "minss" }, { UD_Iminps, "minps" }, { UD_Iminsd, "minsd" }, { UD_Iminpd, "minpd" },
    { UD_Imaxss, "maxss" }, { UD_Imaxps, "maxps" }, { UD_Imaxsd, "maxsd" }, { UD_Imaxpd, "maxpd" },
    { UD_Isqrtss, "sqrtss" }, { UD_Isqrtps, "sqrtps" }, { UD_Isqrtsd, "sqrtsd" }, { UD_Isqrtpd, "sqrtpd" },
    { UD_Ircpss, "rcpss" }, { UD_Ircpps, "rcpps" },
    { UD_Irsqrtss, "rsqrtss" }, { UD_Irsqrtps, "rsqrtps" },
    { UD_Iandps, "and128" }, { UD_Iandpd, "and128" }, { UD_Ipand, "and128" },
    { UD_Iandnps, "andn128" }, { UD_Iandnpd, "andn128" }, { UD_Ipandn, "andn128" },
    { UD_Iorps, "or128" }, { UD_Iorpd, "or128" }, { UD_Ipor, "or128" },
    { UD_Ixorps, "xor128" }, { UD_Ixorpd, "xor128" }, { UD_Ipxor, "xor128" },
    { UD_Iunpcklps, "unpcklps" }, { UD_Iunpckhps, "unpckhps" }, { UD_Iunpcklpd, "unpcklpd" }, { UD_Iunpckhpd, "unpckhpd" },
    { UD_Ipunpcklbw, "punpcklbw" }, { UD_Ipunpckhbw, "punpckhbw" }, { UD_Ipunpcklwd, "punpcklwd" }, { UD_Ipunpckhwd, "punpckhwd" },
    { UD_Ipunpckldq, "punpckldq" }, { UD_Ipunpckhdq, "punpckhdq" }, { UD_Ipunpcklqdq, "punpcklqdq" }, { UD_Ipunpckhqdq, "punpckhqdq" },
    { UD_Ipaddb, "paddb" }, { UD_Ipaddw, "paddw" }, { UD_Ipaddd, "paddd" }, { UD_Ipaddq, "paddq" },
    { UD_Ipsubb, "psubb" }, { UD_Ipsubw, "psubw" }, { UD_Ipsubd, "psubd" }, { UD_Ipsubq, "psubq" },
    { UD_Ipmullw, "pmullw" }, { UD_Ipmulhw, "pmulhw" }, { UD_Ipmulhuw, "pmulhuw" },
    { UD_Ipcmpeqb, "pcmpeqb" }, { UD_Ipcmpeqw, "pcmpeqw" }, { UD_Ipcmpeqd, "pcmpeqd" },
    { UD_Ipcmpgtb, "pcmpgtb" }, { UD_Ipcmpgtw, "pcmpgtw" }, { UD_Ipcmpgtd, "pcmpgtd" },
    { UD_Icomiss, "comiss" }, { UD_Iucomiss, "comiss" }, { UD_Icomisd, "comisd" }, { UD_Iucomisd, "comisd" },
    { UD_Icvtss2sd, "cvtss2sd" }, { UD_Icvtsd2ss, "cvtsd2ss" },
    { UD_Icvtdq2ps, "cvtdq2ps" }, { UD_Icvttps2dq, "cvttps2dq" }, { UD_Icvtps2dq, "cvtps2dq" },
    { UD_Icvtps2pd, "cvtps2pd" }, { UD_Icvtpd2ps, "cvtpd2ps" },
    { UD_Imovaps, "mov128" }, { UD_Imovups, "mov128" }, { UD_Imovapd, "mov128" }, { UD_Imovupd, "mov128" },
    { UD_Imovdqa, "mov128" }, { UD_Imovdqu, "mov128" },
    { UD_Imovss, "movss" },
    { UD_Imovhlps, "movhlps" }, { UD_Imovlhps, "movlhps" },
    { UD_Imovhps, "movhps" }, { UD_Imovlps, "movlps" },
    { UD_Inone, NULL }
};

/* dst xmm, src xmm or memory, imm8 */
static const struct x86ext_map x86ext_xxmi[] = {
    { UD_Ishufps, "shufps" }, { UD_Ishufpd, "shufpd" }, { UD_Ipshufd, "pshufd" },
    { UD_Icmpps, "cmpps" }, { UD_Icmpss, "cmpss" }, { UD_Icmppd, "cmppd" },
    { UD_Inone, NULL }
};

/* packed shifts: dst xmm, count imm8 or xmm or memory */
static const struct x86ext_map x86ext_shift[] = {
    { UD_Ipsllw, "psllw" }, { UD_Ipslld, "pslld" }, { UD_Ipsllq, "psllq" },
    { UD_Ipsrlw, "psrlw" }, { UD_Ipsrld, "psrld" }, { UD_Ipsrlq, "psrlq" },
    { UD_Ipsraw, "psraw" }, { UD_Ipsrad, "psrad" },
    { UD_Ipslldq, "pslldq" }, { UD_Ipsrldq, "psrldq" },
    { UD_Inone, NULL }
};

/* dst r32, src xmm or memory */
static const struct x86ext_map x86ext_rxm[] = {
    { UD_Icvttss2si, "cvttss2si" }, { UD_Icvtss2si, "cvtss2si" },
    { UD_Icvttsd2si, "cvttsd2si" }, { UD_Icvtsd2si, "cvtsd2si" },
    { UD_Imovmskps, "movmskps" }, { UD_Imovmskpd, "movmskpd" },
    { UD_Inone, NULL }
};

/* x87 instructions without operands */
static const struct x86ext_map x86ext_x87void[] = {
    { UD_Ifpatan, "x87_fpatan_void" }, { UD_Ifsincos, "x87_fsincos_void" },
    { UD_Ifscale, "x87_fscale_void" }, { UD_Ifrndint, "x87_frndint_void" },
    { UD_If2xm1, "x87_f2xm1_void" }, { UD_Ifldl2e, "x87_fldl2e_void" },
    { UD_Ifldl2t, "x87_fldl2t_void" }, { UD_Ifldpi, "x87_fldpi_void" },
    { UD_Inone, NULL }
};

/* condition code (x86 encoding) of cmovcc / setcc / fcmovcc, or -1 */
static int x86ext_cc(enum ud_mnemonic_code m)
{
    switch (m)
    {
        case UD_Icmovo:  return 0;  case UD_Icmovno: return 1;
        case UD_Icmovb:  return 2;  case UD_Icmovae: return 3;
        case UD_Icmovz:  return 4;  case UD_Icmovnz: return 5;
        case UD_Icmovbe: return 6;  case UD_Icmova:  return 7;
        case UD_Icmovs:  return 8;  case UD_Icmovns: return 9;
        case UD_Icmovp:  return 10; case UD_Icmovnp: return 11;
        case UD_Icmovl:  return 12; case UD_Icmovge: return 13;
        case UD_Icmovle: return 14; case UD_Icmovg:  return 15;
        default: return -1;
    }
}

static int x86ext_setcc_cc(enum ud_mnemonic_code m)
{
    /* only the conditions that upstream SRW does not lower */
    switch (m)
    {
        case UD_Iseto:  return 0;
        case UD_Isetno: return 1;
        case UD_Isetae: return 3;
        case UD_Isetp:  return 10;
        case UD_Isetnp: return 11;
        default: return -1;
    }
}

static int x86ext_fcmov_cc(enum ud_mnemonic_code m)
{
    switch (m)
    {
        case UD_Ifcmovb:   return 2;
        case UD_Ifcmovnb:  return 3;
        case UD_Ifcmove:   return 4;
        case UD_Ifcmovne:  return 5;
        case UD_Ifcmovbe:  return 6;
        case UD_Ifcmovnbe: return 7;
        case UD_Ifcmovu:   return 10;
        case UD_Ifcmovnu:  return 11;
        default: return -1;
    }
}

static const char *x86ext_find(const struct x86ext_map *map, enum ud_mnemonic_code m)
{
    for (; map->helper != NULL; map++)
    {
        if (map->mnemonic == m) return map->helper;
    }
    return NULL;
}

static int x86ext_xmm(const ud_operand_t *op)
{
    if ((op->type == UD_OP_REG) && (op->base >= UD_R_XMM0) && (op->base <= UD_R_XMM7)) return (int)(op->base - UD_R_XMM0);
    return -1;
}

/* x86 register encoding and size of a general-purpose register operand */
static int x86ext_gpr(const ud_operand_t *op, unsigned int *code, unsigned int *size)
{
    if (op->type != UD_OP_REG) return 0;
    if ((op->base >= UD_R_EAX) && (op->base <= UD_R_EDI)) { *code = op->base - UD_R_EAX; *size = 32; return 1; }
    if ((op->base >= UD_R_AX) && (op->base <= UD_R_DI)) { *code = op->base - UD_R_AX; *size = 16; return 1; }
    if ((op->base >= UD_R_AL) && (op->base <= UD_R_BH)) { *code = op->base - UD_R_AL; *size = 8; return 1; }
    return 0;
}

/* Put the value of a general-purpose register operand in a form usable as
   a helper argument. Only the low "size" bits are significant. */
static const char *x86ext_gpr_value(char *cOutput, char *cOutPart, const ud_operand_t *op)
{
    if ((op->base >= UD_R_AH) && (op->base <= UD_R_BH))
    {
        XP("lshr tmp1, %s, 8\n", X862LLSTR(op->base))
        return "tmp1";
    }
    return X862LLSTR(op->base);
}

static int x86ext_lock_opcode(enum ud_mnemonic_code m)
{
    switch (m)
    {
        case UD_Iadd: return 0;  case UD_Ior:  return 1;
        case UD_Iadc: return 2;  case UD_Isbb: return 3;
        case UD_Iand: return 4;  case UD_Isub: return 5;
        case UD_Ixor: return 6;
        case UD_Iinc: return 8;  case UD_Idec: return 9;
        case UD_Inot: return 10; case UD_Ineg: return 11;
        case UD_Ibts: return 14; case UD_Ibtr: return 15;
        case UD_Ibtc: return 16;
        default: return -1;
    }
}

static int SR_llasm_x86ext(unsigned int Entry, uint_fast32_t cur_ofs, output_data *output, char *cOutput, char *cOutPart, fixup_data **fixup, extrn_data **extrn, struct madr_result *memadr)
{
    const enum ud_mnemonic_code m = ud_obj.mnemonic;
    const ud_operand_t *op0 = &(ud_obj.operand[0]);
    const ud_operand_t *op1 = &(ud_obj.operand[1]);
    const ud_operand_t *op2 = &(ud_obj.operand[2]);
    const char *h;
    char cAddress[128];
    unsigned int code, size;
    int d, s, cc, lockop;

#define X86EXT_FAIL { fprintf(stderr, "Error: unsupported operand form - %i - %i (0x%x) - %s\n", Entry, (unsigned int)cur_ofs, (unsigned int)(section[Entry].start + cur_ofs), output->str); return -1; }

    /* ---------------- locked and atomic integer instructions ---------------- */

    if ((m == UD_Ixchg) && ((op0->type == UD_OP_MEM) || (op1->type == UD_OP_MEM)))
    {
        const ud_operand_t *mop = (op0->type == UD_OP_MEM) ? op0 : op1;
        const ud_operand_t *rop = (op0->type == UD_OP_MEM) ? op1 : op0;
        int mi = (op0->type == UD_OP_MEM) ? 0 : 1;

        if (!x86ext_gpr(rop, &code, &size)) X86EXT_FAIL
        SR_disassemble_get_madr(cOutput, mop, fixup[mi], extrn[mi], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        XP("call x86_lock_xchg $, %u, %s, %u\n", size, memadr->madr, code)
        return 1;
    }

    if ((m == UD_Ixadd) || (m == UD_Icmpxchg))
    {
        if ((op0->type != UD_OP_MEM) || !x86ext_gpr(op1, &code, &size)) X86EXT_FAIL
        SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        XP("call %s $, %u, %s, %u\n", (m == UD_Ixadd) ? "x86_lock_xadd" : "x86_lock_cmpxchg", size, memadr->madr, code)
        return 1;
    }

    if (m == UD_Icmpxchg8b)
    {
        if (op0->type != UD_OP_MEM) X86EXT_FAIL
        SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        XP("call x86_lock_cmpxchg8b $, %s\n", memadr->madr)
        return 1;
    }

    if (ud_obj.pfx_lock)
    {
        const char *src;

        lockop = x86ext_lock_opcode(m);
        if ((lockop < 0) || (op0->type != UD_OP_MEM)) X86EXT_FAIL
        size = op0->size;
        if ((size != 8) && (size != 16) && (size != 32)) X86EXT_FAIL

        SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);

        if (op1->type == UD_NONE)
        {
            src = "0";
        }
        else if (op1->type == UD_OP_REG)
        {
            unsigned int src_size;

            if (!x86ext_gpr(op1, &code, &src_size) || (src_size != size)) X86EXT_FAIL
            src = x86ext_gpr_value(cOutput, cOutPart, op1);
            if (lockop >= 14)
            {
                XP("call x86_lock_bt_reg $, %i, %u, %s, %s\n", lockop, size, memadr->madr, src)
                return 1;
            }
        }
        else if (op1->type == UD_OP_IMM)
        {
            if (fixup[1] != NULL)
            {
                SR_get_fixup_label(cAddress, fixup[1], extrn[1]);
                XP("mov tmp1, %s\n", cAddress)
                src = "tmp1";
            }
            else
            {
                uint32_t v = SR_disassemble_get_value(op1, (lockop >= 14) ? ZERO_EXTEND : SIGN_EXTEND);
                sprintf(cAddress, "0x%x", (unsigned int)v);
                src = cAddress;
            }
        }
        else X86EXT_FAIL

        XP("call x86_lock_op $, %i, %u, %s, %s\n", lockop, size, memadr->madr, src)
        return 1;
    }

    /* ---------------- cmovcc / setcc ---------------- */

    cc = x86ext_cc(m);
    if (cc >= 0)
    {
        const char *val;

        if (!x86ext_gpr(op0, &code, &size) || (size == 8)) X86EXT_FAIL
        if (op1->type == UD_OP_REG)
        {
            val = x86ext_gpr_value(cOutput, cOutPart, op1);
        }
        else if (op1->type == UD_OP_MEM)
        {
            /* x86 reads the source operand even when the condition is false */
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            if (size == 32) SR_disassemble_read_mem_word(cOutput, memadr, LR_TMP1);
            else SR_disassemble_read_mem_halfword(cOutput, memadr, LR_TMP1, READ16TO32ZERO);
            val = "tmp1";
        }
        else X86EXT_FAIL
        XP("call x86_cmov $, %i, %u, %u, %s\n", cc, size, code, val)
        return 1;
    }

    cc = x86ext_setcc_cc(m);
    if (cc >= 0)
    {
        if (op0->type == UD_OP_REG)
        {
            if (!x86ext_gpr(op0, &code, &size) || (size != 8)) X86EXT_FAIL
            XP("call x86_setcc_r $, %i, %u\n", cc, code)
        }
        else if (op0->type == UD_OP_MEM)
        {
            SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call x86_setcc_m $, %i, %s\n", cc, memadr->madr)
        }
        else X86EXT_FAIL
        return 1;
    }

    /* ---------------- x87 additions ---------------- */

    if ((m == UD_Ifcomi) || (m == UD_Ifucomi) || (m == UD_Ifcomip) || (m == UD_Ifucomip))
    {
        const ud_operand_t *sop = (op1->type == UD_OP_REG) ? op1 : op0;

        if ((sop->type != UD_OP_REG) || (sop->base < UD_R_ST0) || (sop->base > UD_R_ST7)) X86EXT_FAIL
        XP("call x87_fcomi_st $, %i, %i\n", (int)(sop->base - UD_R_ST0), ((m == UD_Ifcomip) || (m == UD_Ifucomip)) ? 1 : 0)
        return 1;
    }

    cc = x86ext_fcmov_cc(m);
    if (cc >= 0)
    {
        const ud_operand_t *sop = (op1->type == UD_OP_REG) ? op1 : op0;

        if ((sop->type != UD_OP_REG) || (sop->base < UD_R_ST0) || (sop->base > UD_R_ST7)) X86EXT_FAIL
        XP("call x87_fcmov_st $, %i, %i\n", (int)(sop->base - UD_R_ST0), cc)
        return 1;
    }

    h = x86ext_find(x86ext_x87void, m);
    if (h != NULL)
    {
        XP("call %s $\n", h)
        return 1;
    }

    if (m == UD_Iffree)
    {
        /* llasm has no x87 tag word, so ffree has no visible effect */
        XO("NOP\n");
        return 1;
    }

    if (m == UD_Iffreep)
    {
        if ((op0->type != UD_OP_REG) || (op0->base < UD_R_ST0) || (op0->base > UD_R_ST7)) X86EXT_FAIL
        XP("call x87_ffreep_st $, %i\n", (int)(op0->base - UD_R_ST0))
        return 1;
    }

    if (m == UD_Ifisttp)
    {
        if ((op0->type != UD_OP_MEM) || (op0->size != 32)) X86EXT_FAIL
        XO("call x87_fisttp_int32 $\n");
        SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        SR_disassemble_write_mem_word(cOutput, memadr, LR_TMP0);
        return 1;
    }

    /* ---------------- misc ---------------- */

    switch (m)
    {
        case UD_Iprefetch:
        case UD_Iprefetchnta:
        case UD_Iprefetcht0:
        case UD_Iprefetcht1:
        case UD_Iprefetcht2:
            /* cache hint only: no architectural effect */
            XO("NOP\n");
            return 1;
        case UD_Iemms:
            /* MMX registers are not modelled; MMX instructions fail translation explicitly */
            XO("NOP\n");
            return 1;
        case UD_Isfence:
        case UD_Ilfence:
        case UD_Imfence:
            XO("call x86_mfence $\n");
            return 1;
        case UD_Ipause:
            XO("call x86_pause $\n");
            return 1;
        case UD_Icpuid:
            XO("call x86_cpuid $\n");
            return 1;
        case UD_Ildmxcsr:
        case UD_Istmxcsr:
            if (op0->type != UD_OP_MEM) X86EXT_FAIL
            SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, (m == UD_Ildmxcsr) ? MADR_REG : MADR_REG, ZERO_EXTEND, memadr);
            XP("call %s $, %s\n", (m == UD_Ildmxcsr) ? "sse_ldmxcsr" : "sse_stmxcsr", memadr->madr)
            return 1;
        default:
            break;
    }

    /* ---------------- SSE / SSE2 ---------------- */

    /* string cmpsw/cmpsd/scasw/scasd (cmpsb and scasb are lowered by upstream SRW) */
    if (((m == UD_Icmpsw) || (m == UD_Icmpsd) || (m == UD_Iscasw) || (m == UD_Iscasd)) &&
        (x86ext_xmm(op0) < 0) && (x86ext_xmm(op1) < 0))
    {
        int rep = ud_obj.pfx_repne ? 2 : ((ud_obj.pfx_rep || ud_obj.pfx_repe) ? 1 : 0);
        unsigned int esize = ((m == UD_Icmpsw) || (m == UD_Iscasw)) ? 16 : 32;

        XP("call %s $, %u, %i\n", ((m == UD_Icmpsw) || (m == UD_Icmpsd)) ? "x86_cmps" : "x86_scas", esize, rep)
        return 1;
    }

    /* movsd is also a string instruction; take only the XMM form here */
    if ((m == UD_Imovsd) && (x86ext_xmm(op0) < 0) && (x86ext_xmm(op1) < 0)) return 0;

    if ((m == UD_Imovsd) || (m == UD_Imovq))
    {
        h = (m == UD_Imovsd) ? "movsd" : "movq";
        d = x86ext_xmm(op0);
        s = x86ext_xmm(op1);
        if ((d >= 0) && (s >= 0)) { XP("call sse_%s_xx $, %i, %i\n", h, d, s) return 1; }
        if ((d >= 0) && (op1->type == UD_OP_MEM))
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_xm $, %i, %s\n", h, d, memadr->madr)
            return 1;
        }
        if ((s >= 0) && (op0->type == UD_OP_MEM))
        {
            SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_mx $, %s, %i\n", h, memadr->madr, s)
            return 1;
        }
        X86EXT_FAIL
    }

    if (m == UD_Icmpsd)
    {
        /* SSE2 cmpsd shares its mnemonic with string cmpsd, whose flag-table
           entry says that it writes flags. SSE2 cmpsd writes no flags, so a
           translation here could hide a live flag value. */
        X86EXT_FAIL
    }

    /* imul r/m8 (one operand): AX = AL * r/m8 */
    if ((m == UD_Iimul) && (op1->type == UD_NONE) && (op0->type == UD_OP_MEM) && (op0->size == 8))
    {
        SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        XP("call x86_imul8_m $, %s\n", memadr->madr)
        return 1;
    }

    /* bsf r32, r32 (upstream SRW has no bsf) */
    if ((m == UD_Ibsf) && (op1->type == UD_OP_REG))
    {
        unsigned int src_size;

        if (!x86ext_gpr(op0, &code, &size) || (size != 32) || !x86ext_gpr(op1, &code, &src_size) || (src_size != 32)) X86EXT_FAIL
        x86ext_gpr(op0, &code, &size);
        XP("call x86_bsf_v $, %u, %s\n", code, X862LLSTR(op1->base))
        return 1;
    }

    /* bsr/bsf r32, m32 (upstream SRW lowers only the register form of bsr) */
    if (((m == UD_Ibsr) || (m == UD_Ibsf)) && (op1->type == UD_OP_MEM))
    {
        if (!x86ext_gpr(op0, &code, &size) || (size != 32) || (op1->size != 32)) X86EXT_FAIL
        SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
        XP("call %s $, %u, %s\n", (m == UD_Ibsr) ? "x86_bsr_m" : "x86_bsf_m", code, memadr->madr)
        return 1;
    }

    if (m == UD_Imovd)
    {
        d = x86ext_xmm(op0);
        s = x86ext_xmm(op1);
        if ((d >= 0) && x86ext_gpr(op1, &code, &size) && (size == 32))
        {
            XP("call sse_movd_xv $, %i, %s\n", d, X862LLSTR(op1->base))
            return 1;
        }
        if ((d >= 0) && (op1->type == UD_OP_MEM))
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_movd_xm $, %i, %s\n", d, memadr->madr)
            return 1;
        }
        if ((s >= 0) && x86ext_gpr(op0, &code, &size) && (size == 32))
        {
            XP("call sse_movd_rx $, %u, %i\n", code, s)
            return 1;
        }
        if ((s >= 0) && (op0->type == UD_OP_MEM))
        {
            SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_movd_mx $, %s, %i\n", memadr->madr, s)
            return 1;
        }
        X86EXT_FAIL
    }

    if ((m == UD_Icvtsi2ss) || (m == UD_Icvtsi2sd))
    {
        const char *val;

        h = (m == UD_Icvtsi2ss) ? "cvtsi2ss" : "cvtsi2sd";
        d = x86ext_xmm(op0);
        if (d < 0) X86EXT_FAIL
        if (x86ext_gpr(op1, &code, &size) && (size == 32))
        {
            val = X862LLSTR(op1->base);
        }
        else if ((op1->type == UD_OP_MEM) && ((op1->size == 32) || (op1->size == 0)))
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            SR_disassemble_read_mem_word(cOutput, memadr, LR_TMP1);
            val = "tmp1";
        }
        else X86EXT_FAIL
        XP("call sse_%s_xv $, %i, %s\n", h, d, val)
        return 1;
    }

    h = x86ext_find(x86ext_rxm, m);
    if (h != NULL)
    {
        if (!x86ext_gpr(op0, &code, &size) || (size != 32)) X86EXT_FAIL
        s = x86ext_xmm(op1);
        if (s >= 0) { XP("call sse_%s_rx $, %u, %i\n", h, code, s) return 1; }
        if ((op1->type == UD_OP_MEM) && (m != UD_Imovmskps) && (m != UD_Imovmskpd))
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_rm $, %u, %s\n", h, code, memadr->madr)
            return 1;
        }
        X86EXT_FAIL
    }

    h = x86ext_find(x86ext_xxmi, m);
    if (h != NULL)
    {
        d = x86ext_xmm(op0);
        s = x86ext_xmm(op1);
        if ((d < 0) || (op2->type != UD_OP_IMM)) X86EXT_FAIL
        if (s >= 0) { XP("call sse_%s_xxi $, %i, %i, %u\n", h, d, s, (unsigned int)op2->lval.ubyte) return 1; }
        if (op1->type == UD_OP_MEM)
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_xmi $, %i, %s, %u\n", h, d, memadr->madr, (unsigned int)op2->lval.ubyte)
            return 1;
        }
        X86EXT_FAIL
    }

    h = x86ext_find(x86ext_shift, m);
    if (h != NULL)
    {
        d = x86ext_xmm(op0);
        s = x86ext_xmm(op1);
        if (d < 0) X86EXT_FAIL
        if (op1->type == UD_OP_IMM) { XP("call sse_%s_xi $, %i, %u\n", h, d, (unsigned int)op1->lval.ubyte) return 1; }
        if ((m == UD_Ipslldq) || (m == UD_Ipsrldq)) X86EXT_FAIL
        if (s >= 0) { XP("call sse_%s_xx $, %i, %i\n", h, d, s) return 1; }
        if (op1->type == UD_OP_MEM)
        {
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_xm $, %i, %s\n", h, d, memadr->madr)
            return 1;
        }
        X86EXT_FAIL
    }

    h = x86ext_find(x86ext_xxm, m);
    if (h != NULL)
    {
        d = x86ext_xmm(op0);
        s = x86ext_xmm(op1);
        if ((d >= 0) && (s >= 0))
        {
            if ((m == UD_Imovhps) || (m == UD_Imovlps)) X86EXT_FAIL
            XP("call sse_%s_xx $, %i, %i\n", h, d, s)
            return 1;
        }
        if ((d >= 0) && (op1->type == UD_OP_MEM))
        {
            if ((m == UD_Imovhlps) || (m == UD_Imovlhps)) X86EXT_FAIL
            SR_disassemble_get_madr(cOutput, op1, fixup[1], extrn[1], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_xm $, %i, %s\n", h, d, memadr->madr)
            return 1;
        }
        if ((s >= 0) && (op0->type == UD_OP_MEM) &&
            ((m == UD_Imovaps) || (m == UD_Imovups) || (m == UD_Imovapd) || (m == UD_Imovupd) ||
             (m == UD_Imovdqa) || (m == UD_Imovdqu) || (m == UD_Imovss) || (m == UD_Imovhps) || (m == UD_Imovlps)))
        {
            SR_disassemble_get_madr(cOutput, op0, fixup[0], extrn[0], UD_NONE, MADR_REG, ZERO_EXTEND, memadr);
            XP("call sse_%s_mx $, %s, %i\n", h, memadr->madr, s)
            return 1;
        }
        X86EXT_FAIL
    }

#undef X86EXT_FAIL

    return 0;
}

#undef XO
#undef XP
