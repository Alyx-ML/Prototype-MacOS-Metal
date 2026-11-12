/*
 * Host hooks of the SR llasm support code (third_party/SR/SR/llasm-support).
 *
 * The translated code calls the support helpers for instructions that have
 * no direct LLVM form. Some helpers call back into the host:
 *   X86_ReadMemProcedure, X86_WriteMemProcedure   memory accesses at an
 *       address that SRW did not map to the image (llasm_movmem.c). The
 *       engine has one such access: a read of guest address 4, a deliberate
 *       fault.
 *   X86_InterruptProcedure   the "int n" instruction (llasm_int.c). The
 *       launcher has int3 bytes between functions.
 *   X86_InterruptFlag        the IF bit that popfd writes (llasm_pushx.c).
 *
 * Processor faults are not SEH exceptions in this runtime (see rt_seh.c):
 * the translated code does not keep an exact guest register state at each
 * instruction. So an access to guest memory that is not committed, and an
 * interrupt instruction, stop the process with the guest address. An access
 * to committed guest memory works, as on Windows.
 */
#include "rt.h"

/* The support code declares the hooks with the cdecl convention; on ARM64
   the convention attribute has no effect, so plain C functions agree. */
uint32_t X86_ReadMemProcedure(const uint32_t Address, const uint32_t MemSize);
void X86_WriteMemProcedure(const uint32_t Address, const uint32_t MemSize, const uint32_t _eax);
void X86_InterruptProcedure(const uint8_t IntNum, void *regs);

/* User-mode popfd does not change IF on Windows (IOPL 0); the support code
   stores the bit here and nothing reads it. */
uint32_t X86_InterruptFlag = 1;

static void check_access(uint32_t addr, uint32_t size, const char *kind)
{
    if (size != 1 && size != 2 && size != 4) rt_fatal("guest %s of %u bytes at 0x%08x: bad access size", kind, size, addr);
    if (!rt_va_is_committed(addr, size))
        rt_fatal("access violation: guest %s of %u bytes at 0x%08x (memory that is not committed); processor faults "
                 "are not converted to SEH exceptions", kind, size, addr);
}

uint32_t X86_ReadMemProcedure(const uint32_t Address, const uint32_t MemSize)
{
    check_access(Address, MemSize, "read");
    if (MemSize == 1) return rd8(Address);
    if (MemSize == 2) return rd16(Address);
    return rd32(Address);
}

void X86_WriteMemProcedure(const uint32_t Address, const uint32_t MemSize, const uint32_t _eax)
{
    check_access(Address, MemSize, "write");
    if (MemSize == 1)
        wr8(Address, (uint8_t)_eax);
    else if (MemSize == 2)
        wr16(Address, (uint16_t)_eax);
    else
        wr32(Address, _eax);
}

void X86_InterruptProcedure(const uint8_t IntNum, void *regs)
{
    (void)regs;
    if (IntNum == 3)
        rt_fatal("breakpoint: the guest ran an int3 instruction; processor faults are not converted to SEH exceptions");
    rt_fatal("the guest ran the instruction int 0x%02x; processor faults are not converted to SEH exceptions", IntNum);
}
