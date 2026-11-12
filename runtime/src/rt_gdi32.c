/*
 * GDI32: stock objects. The game uses GDI only for the window class
 * background brush (GetStockObject). Drawing is done with Metal, so the
 * stock object handles are identifiers only.
 */
#include "rt.h"

/* Stock object handles: one value for each index, as on Windows (the
   values are shared by all callers). */
#define STOCK_BASE 0x01900010u

/* HGDIOBJ GetStockObject(int index)
   Valid indices: 0-8 (brushes and pens), 10-17 (fonts and the default
   palette), 18 (DC_BRUSH), 19 (DC_PEN). Other values give NULL. */
void rt_GetStockObject(_cpu *cpu)
{
    uint32_t i = ARG(0);
    if (i > 19 || i == 9)
    {
        RET(0);
        return;
    }
    RET(STOCK_BASE + 4u * i);
}
