#include "tiles_generic.h"
#include "midtunit.h"
#include "tms34010_intf.h"
#include "m6809_intf.h"
#include "burn_ym2151.h"

static UINT8 *AllMem;
static UINT8 *RamEnd;
static UINT8 *AllRam;
static UINT8 *MemEnd;
static UINT8 *DrvBootROM;
static UINT8 *DrvSoundROM;
static UINT8 *DrvGfxROM;
static UINT8 *DrvRAM;
static UINT8 *DrvNVRAM;
static UINT8 *DrvPalette;
static UINT32 *DrvPaletteB;
static UINT8 *DrvVRAM;
static UINT16 *DrvVRAM16;
static UINT8 *DrvSoundProgROM;
static UINT8 *DrvSoundProgRAM;

UINT8 nTUnitRecalc;
UINT8 nTUnitJoy1[32];
UINT8 nTUnitJoy2[32];
UINT8 nTUnitJoy3[32];
UINT8 nTUnitDSW[8];
UINT8 nTUnitReset = 0;
static UINT32 DrvInputs[4];

static bool bGfxRomLarge = false;
static UINT32 nGfxBankOffset[2] = { 0x000000, 0x400000 };

static bool bCMOSWriteEnable = false;
static UINT32 nVideoBank = 1;
static UINT16 nDMA[32];
static UINT16 nTUnitCtrl = 0;
static UINT8 DrvFakeSound = 0;

static UINT8 MKProtIndex = 0;

#define RGB888(r,g,b)   ((r) | ((g) << 8) | ((b) << 16))
#define RGB888_r(x) ((x) & 0xFF)
#define RGB888_g(x) (((x) >>  8) & 0xFF)
#define RGB888_b(x) (((x) >> 16) & 0xFF)

#define RGB555_2_888(x)     \
    RGB888((x >> 7) & 0xF8, \
           (x >> 2) & 0xF8, \
           (x << 3) & 0xF8)

#define RGB888_2_565(x)  (          \
    ((RGB888_r(x) << 8) & 0xF800) | \
    ((RGB888_g(x) << 3) & 0x07E0) | \
    ((RGB888_b(x) >> 3)))

#include "midtunit_dma.h"

static INT32 MemIndex()
{
    UINT8 *Next; Next = AllMem;

    DrvBootROM 	= Next;             Next += 0x800000 * sizeof(UINT8);
	DrvSoundROM	= Next;				Next += 0x1000000 * sizeof(UINT8);
	DrvGfxROM 	= Next;				Next += 0x2000000 * sizeof(UINT8);
	DrvSoundProgROM = Next;			Next +- 0x0040000 * sizeof(UINT8);

	AllRam		= Next;
	DrvSoundProgRAM = Next;			Next += 0x002000 * sizeof(UINT8);
	DrvRAM		= Next;				Next += 0x400000 * sizeof(UINT16);
	DrvNVRAM	= Next;				Next += 0x60000 * sizeof(UINT16);
	DrvPalette	= Next;				Next += 0x20000 * sizeof(UINT8);
	DrvPaletteB	= (UINT32*)Next;	Next += 0x8000 * sizeof(UINT32);
	DrvVRAM		= Next;				Next += 0x400000 * sizeof(UINT16);
	DrvVRAM16	= (UINT16*)DrvVRAM;
	RamEnd		= Next;

	MemEnd		= Next;
    return 0;
}

static INT32 LoadGfxBanks()
{
    char *pRomName;
    struct BurnRomInfo pri;

    for (INT32 i = 0; !BurnDrvGetRomName(&pRomName, i, 0); i++) {
        BurnDrvGetRomInfo(&pri, i);
        if ((pri.nType & 7) == 3) {

            UINT32 addr = TUNIT_GFX_ADR(pri.nType) << 20;
            UINT32 offs = TUNIT_GFX_OFF(pri.nType);

            if (BurnLoadRom(DrvGfxROM + addr + offs, i, 4) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static INT32 LoadSoundProgRom()
{
    char *pRomName;
    struct BurnRomInfo pri;

    for (INT32 i = 0; !BurnDrvGetRomName(&pRomName, i, 0); i++) {
        BurnDrvGetRomInfo(&pri, i);
        if ((pri.nType & 7) == 4) {
            if (BurnLoadRom(DrvSoundProgROM, i, 1) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static void TUnitToShift(UINT32 address, void *dst)
{
    memcpy(dst, &DrvVRAM16[(address >> 3)], 4096/2);
}

static void TUnitFromShift(UINT32 address, void *src)
{
    memcpy(&DrvVRAM16[(address >> 3)], src, 4096/2);
}

static void TUnitDoReset()
{
	TMS34010Reset();
	
	M6809Open(0);
	M6809Reset();
	M6809Close();
	
	BurnYM2151Reset();
	
	MKProtIndex = 0;
	DrvFakeSound = 0;
}

static UINT16 TUnitVramRead(UINT32 address)
{
    UINT32 offset = TOBYTE(address & 0x3fffff);
    if (nVideoBank)
        return (DrvVRAM16[offset] & 0x00ff) | (DrvVRAM16[offset + 1] << 8);
    else
        return (DrvVRAM16[offset] >> 8) | (DrvVRAM16[offset + 1] & 0xff00);
}

static void TUnitVramWrite(UINT32 address, UINT16 data)
{
    UINT32 offset = TOBYTE(address & 0x3fffff);
    if (nVideoBank)
    {
        DrvVRAM16[offset] = (data & 0xff) | ((nDMA[DMA_PALETTE] & 0xff) << 8);
        DrvVRAM16[offset + 1] = ((data >> 8) & 0xff) | (nDMA[DMA_PALETTE] & 0xff00);
    }
    else
    {
        DrvVRAM16[offset] = (DrvVRAM16[offset] & 0xff) | ((data & 0xff) << 8);
        DrvVRAM16[offset + 1] = (DrvVRAM16[offset + 1] & 0xff) | (data & 0xff00);
    }
}

static UINT16 TUnitPalRead(UINT32 address)
{
    address &= 0x7FFFF;
    return *(UINT16*)(&DrvPalette[TOBYTE(address)]);
}

static void TUnitPalWrite(UINT32 address, UINT16 value)
{
    address &= 0x7FFFF;
    *(UINT16*)(&DrvPalette[TOBYTE(address)]) = value;

    UINT32 col = RGB555_2_888(BURN_ENDIAN_SWAP_INT16(value));
    DrvPaletteB[address>>4] = BurnHighCol(RGB888_r(col),RGB888_g(col),RGB888_b(col),0);
}

static void TUnitPalRecalc()
{
	for (INT32 i = 0; i < 0x10000; i += 2) {
		UINT16 value = *(UINT16*)(&DrvPalette[i]);

		UINT32 col = RGB555_2_888(BURN_ENDIAN_SWAP_INT16(value));
		DrvPaletteB[i>>1] = BurnHighCol(RGB888_r(col),RGB888_g(col),RGB888_b(col),0);
	}
}

static INT32 ScanlineRender(INT32 line, TMS34010Display *info)
{
    if (!pBurnDraw) return 0;

	UINT16 *src = &DrvVRAM16[(info->rowaddr << 9) & 0x3FE00];

    if (info->rowaddr >= nScreenHeight) return 0;

    INT32 col = info->coladdr << 1;
    UINT16 *dest = (UINT16*) pTransDraw + (info->rowaddr * nScreenWidth);

    const INT32 heblnk = info->heblnk;
    const INT32 hsblnk = info->hsblnk * 2; // T-Unit is 2 pixels per clock
    for (INT32 x = heblnk; x < hsblnk; x++) {
        dest[x - heblnk] = src[col++ & 0x1FF] & 0x7FFF;
    }

    return 0;
}

static UINT16 TUnitRead(UINT32 address)
{
	if (address == 0x01600040) return 0xff; // ???
	if (address == 0x01d81070) return 0xff; // watchdog
	
	bprintf(PRINT_NORMAL, _T("Read %x\n"), address);

	return ~0;
}

static void TUnitWrite(UINT32 address, UINT16 value)
{
	if (address == 0x01d81070) return; // watchdog
	
	bprintf(PRINT_NORMAL, _T("Write %x, %x\n"), address, value);
}

static UINT16 TUnitInputRead(UINT32 address)
{
	INT32 offset = (address & 0xff) >> 4;
	
	switch (offset) {
		case 0x00: return ~DrvInputs[0];
		case 0x01: return ~DrvInputs[1];
		case 0x02: return ~DrvInputs[2];
		case 0x03: return nTUnitDSW[0] | (nTUnitDSW[1] << 8);
	}

	return ~0;
}

static UINT16 TUnitGfxRead(UINT32 address)
{
	//uint8_t *base = m_gfxrom->base() + gfxbank_offset[(offset >> 21) & 1];
	UINT8 *base = DrvGfxROM + nGfxBankOffset[0];
    UINT32 offset = TOBYTE(address - 0x02000000);
    return base[offset] | (base[offset + 1] << 8);
}

static UINT16 TUnitSoundStateRead(UINT32 address)
{
	//bprintf(PRINT_NORMAL, _T("Sound State Read %x\n"), address);
	
	if (DrvFakeSound) {
		DrvFakeSound--;
		return 0;
	}

	return ~0;
}

static UINT16 TUnitSoundRead(UINT32 address)
{
	bprintf(PRINT_NORMAL, _T("Sound Read %x\n"), address);

	return ~0;
}

static void TUnitSoundWrite(UINT32 address, UINT16 value)
{
	bprintf(PRINT_NORMAL, _T("Sound Write %x, %x\n"), address, value);
}

static void TUnitCtrlWrite(UINT32 address, UINT16 value)
{
	nTUnitCtrl = value;
    
	if (!(nTUnitCtrl & 0x0080) || !bGfxRomLarge) {
		nGfxBankOffset[0] = 0x000000;
	} else {
		nGfxBankOffset[0] = 0x800000;
	}
	
    nVideoBank = (nTUnitCtrl >> 5) & 1;
}

static void TUnitCMOSWriteEnable(UINT32 address, UINT16 value)
{
    bCMOSWriteEnable = true;
}

static UINT16 TUnitCMOSRead(UINT32 address)
{
    UINT16 *wn = (UINT16*)DrvNVRAM;
    UINT32 offset = (address & 0x01ffff) >> 1;
    return wn[offset];
}


static void TUnitCMOSWrite(UINT32 address, UINT16 value)
{
	UINT16 *wn = (UINT16*)DrvNVRAM;
	UINT32 offset = (address & 0x01ffff) >> 1;
	wn[offset] = value;
}

static const uint8_t mk_prot_values[] =
{
	0x13, 0x27, 0x0f, 0x1f, 0x3e, 0x3d, 0x3b, 0x37,
	0x2e, 0x1c, 0x38, 0x31, 0x22, 0x05, 0x0a, 0x15,
	0x2b, 0x16, 0x2d, 0x1a, 0x34, 0x28, 0x10, 0x21,
	0x03, 0x06, 0x0c, 0x19, 0x32, 0x24, 0x09, 0x13,
	0x27, 0x0f, 0x1f, 0x3e, 0x3d, 0x3b, 0x37, 0x2e,
	0x1c, 0x38, 0x31, 0x22, 0x05, 0x0a, 0x15, 0x2b,
	0x16, 0x2d, 0x1a, 0x34, 0x28, 0x10, 0x21, 0x03,
	0xff
};

static UINT16 MKProtRead(UINT32 /*address*/)
{
	if (MKProtIndex >= sizeof(mk_prot_values)) {
		MKProtIndex = 0;
	}

	return mk_prot_values[MKProtIndex++] << 9;
}


static void MKProtWrite(UINT32 /*address*/, UINT16 value)
{
	INT32 first_val = (value >> 9) & 0x3f;
	INT32 i;

	for (i = 0; i < sizeof(mk_prot_values); i++) {
		if (mk_prot_values[i] == first_val) {
			MKProtIndex = i;
			break;
		}
	}

	if (i == sizeof(mk_prot_values)) {
		MKProtIndex = 0;
	}
}

static void MKYM2151IrqHandler(INT32 Irq)
{
	M6809SetIRQLine(M6809_FIRQ_LINE, (Irq) ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
}

static UINT8 MKSoundRead(UINT16 address)
{
	switch (address) {
		
	}

	bprintf(PRINT_NORMAL, _T("M6809 Read Byte -> %04X\n"), address);

	return 0;
}


static void MKSoundWrite(UINT16 address, UINT8 value)
{
	if (address >= 0x4000) return; // ROM
	
	switch (address) {
		case 0x2400: {
			BurnYM2151SelectRegister(value);
			return;
		}
		
		case 0x2401: {
			BurnYM2151WriteRegister(value);
			return;
		}
	}

	bprintf(PRINT_NORMAL, _T("M6809 Write Byte -> %04X, %02X\n"), address, value);
}

INT32 TUnitInit()
{
    MemIndex();
    INT32 nLen = MemEnd - (UINT8 *)0;

    if ((AllMem = (UINT8 *)BurnMalloc(nLen)) == NULL)
        return 1;

    MemIndex();

    UINT32 nRet;
    nRet = BurnLoadRom(DrvBootROM + 0, 0, 2);
    if (nRet != 0) return 1;

    nRet = BurnLoadRom(DrvBootROM + 1, 1, 2);
    if (nRet != 0) return 1;
	
	nRet = LoadSoundProgRom();
	if (nRet != 0) return 1;

/*    nRet = LoadSoundBanks();
    if (nRet != 0)
        return 1;*/

    nRet = LoadGfxBanks();
    if (nRet != 0) return 1;
	
	BurnSetRefreshRate(54.71);

    TMS34010MapReset();
    TMS34010Init();

    TMS34010SetScanlineRender(ScanlineRender);
    TMS34010SetToShift(TUnitToShift);
    TMS34010SetFromShift(TUnitFromShift);
	
#if 0
	// barry - for reference - unmapped memory
	map(0x01b00000, 0x01b0001f).w(FUNC(midtunit_state::midtunit_control_w));
	m_maincpu->space(AS_PROGRAM).install_readwrite_handler(0x1b00000, 0x1b6ffff, read16_delegate(FUNC(midtunit_state::mk_prot_r),this), write16_delegate(FUNC(midtunit_state::mk_prot_w),this));
	
	map(0x02000000, 0x07ffffff).r(FUNC(midtunit_state::midtunit_gfxrom_r)).share("gfxrom");
#endif
	
	// this will be removed - but putting all unmapped memory through generic handlers to enable logging unmapped reads/writes
	TMS34010SetHandlers(1, TUnitRead, TUnitWrite);
	TMS34010MapHandler(1, 0x00000000, 0x1FFFFFFF, MAP_READ | MAP_WRITE);
	
	TMS34010MapMemory(DrvBootROM, 0xFF800000, 0xFFFFFFFF, MAP_READ);
	TMS34010MapMemory(DrvBootROM, 0x1F800000, 0x1FFFFFFF, MAP_READ); // mirror
	TMS34010MapMemory(DrvRAM,     0x01000000, 0x013FFFFF, MAP_READ | MAP_WRITE);
	
	TMS34010SetHandlers(2, TUnitVramRead, TUnitVramWrite);
    TMS34010MapHandler(2, 0x00000000, 0x003fffff, MAP_READ | MAP_WRITE);
	
	TMS34010SetHandlers(3, TUnitCMOSRead, TUnitCMOSWrite);
    TMS34010MapHandler(3, 0x01400000, 0x0141ffff, MAP_READ | MAP_WRITE);
	
	TMS34010SetWriteHandler(4, TUnitCMOSWriteEnable);
    TMS34010MapHandler(4, 0x01480000, 0x014fffff, MAP_READ | MAP_WRITE);
	
	TMS34010SetReadHandler(5, TUnitInputRead);
    TMS34010MapHandler(5, 0x01600000, 0x0160003f, MAP_READ);
	
	TMS34010SetHandlers(6, TUnitPalRead, TUnitPalWrite);
    TMS34010MapHandler(6, 0x01800000, 0x0187ffff, MAP_READ | MAP_WRITE);
	
	TMS34010SetHandlers(7, TUnitDmaRead, TUnitDmaWrite);
    TMS34010MapHandler(7, 0x01a80000, 0x01a800ff, MAP_READ | MAP_WRITE);
	
	TMS34010SetHandlers(8, MKProtRead, MKProtWrite);
    TMS34010MapHandler(8, 0x01b00000, 0x1b6ffff, MAP_READ | MAP_WRITE);
	
	TMS34010SetReadHandler(9, TUnitSoundStateRead);
    TMS34010MapHandler(9, 0x01d00000, 0x01d0001f, MAP_READ);
	
	TMS34010SetHandlers(10, TUnitSoundRead, TUnitSoundWrite);
    TMS34010MapHandler(10, 0x01d01020, 0x01d0103f, MAP_READ | MAP_WRITE);
	
	TMS34010SetWriteHandler(11, TUnitCtrlWrite);
    TMS34010MapHandler(11, 0x01f00000, 0x01f0001f, MAP_WRITE);
	
	TMS34010SetReadHandler(12, TUnitGfxRead);
    TMS34010MapHandler(12, 0x02000000, 0x07ffffff, MAP_READ);

    memset(DrvVRAM, 0, 0x400000);
	
	M6809Init(0);
	M6809Open(0);
	M6809MapMemory(DrvSoundProgRAM         , 0x0000, 0x1fff, MAP_RAM);
	M6809MapMemory(DrvSoundProgROM         , 0x4000, 0xbfff, MAP_ROM);
	M6809MapMemory(DrvSoundProgROM + 0x8000, 0xc000, 0xffff, MAP_ROM);
	M6809SetReadHandler(MKSoundRead);
	M6809SetWriteHandler(MKSoundWrite);
	M6809Close();
	
	BurnYM2151Init(3579545);
	BurnYM2151SetIrqHandler(&MKYM2151IrqHandler);
	BurnYM2151SetRoute(BURN_SND_YM2151_YM2151_ROUTE_1, 1.00, BURN_SND_ROUTE_LEFT);
	BurnYM2151SetRoute(BURN_SND_YM2151_YM2151_ROUTE_2, 1.00, BURN_SND_ROUTE_RIGHT);
	
	GenericTilesInit();
	
	TUnitDoReset();
	
    return 0;
}

static void MakeInputs()
{
    DrvInputs[0] = 0;
    DrvInputs[1] = 0;
    DrvInputs[2] = 0;
    DrvInputs[3] = 0;

    for (INT32 i = 0; i < 16; i++) {
        if (nTUnitJoy1[i] & 1) DrvInputs[0] |= (1 << i);
        if (nTUnitJoy2[i] & 1) DrvInputs[1] |= (1 << i);
        if (nTUnitJoy3[i] & 1) DrvInputs[2] |= (1 << i);
    }
}

INT32 TUnitExit()
{
	BurnFree(AllMem);
	
	M6809Exit();
	BurnYM2151Exit();
	
	GenericTilesExit();

    return 0;
}

INT32 TUnitDraw()
{
	if (nTUnitRecalc) {
		bprintf(0, _T("\nRecalculating the palette!!!!\n"));
		TUnitPalRecalc();
		nTUnitRecalc = 0;
	}

	// TMS34010 renders scanlines direct to pTransDraw
	BurnTransferCopy(DrvPaletteB);

	return 0;
}

INT32 TUnitFrame()
{
	if (nTUnitReset) TUnitDoReset();
	
	MakeInputs();

	TMS34010NewFrame();
	M6809NewFrame();

	INT32 nInterleave = 288;
	INT32 nCyclesTotal[2] = { (INT32)(50000000/8/54.71), (INT32)(2000000 / 54.71) };
	INT32 nCyclesDone[2] = { 0, 0 };
	INT32 nSoundBufferPos = 0;
	
	M6809Open(0);

	for (INT32 i = 0; i < nInterleave; i++) {
		nCyclesDone[0] += TMS34010Run((nCyclesTotal[0] * (i + 1) / nInterleave) - nCyclesDone[0]);
		
		nCyclesDone[1] += M6809Run((nCyclesTotal[1] * (i + 1) / nInterleave) - nCyclesDone[1]);

		TMS34010GenerateScanline(i);
		
		if (pBurnSoundOut) {
			INT32 nSegmentLength = nBurnSoundLen / nInterleave;
			INT16* pSoundBuf = pBurnSoundOut + (nSoundBufferPos << 1);
			BurnYM2151Render(pSoundBuf, nSegmentLength);
			nSoundBufferPos += nSegmentLength;
		}
    }
	
	// Make sure the buffer is entirely filled.
	if (pBurnSoundOut) {
		INT32 nSegmentLength = nBurnSoundLen - nSoundBufferPos;
		INT16* pSoundBuf = pBurnSoundOut + (nSoundBufferPos << 1);

		if (nSegmentLength) {
			BurnYM2151Render(pSoundBuf, nSegmentLength);
		}
	}
	
	M6809Close();

	if (pBurnDraw) {
		TUnitDraw();
	}

    return 0;
}

INT32 TUnitScan(INT32 nAction, INT32 *pnMin)
{
	struct BurnArea ba;

	if (pnMin) {
		*pnMin = 0x029704;
	}

	if (nAction & ACB_VOLATILE) {
		memset(&ba, 0, sizeof(ba));
		ba.Data	  = AllRam;
		ba.nLen	  = RamEnd - AllRam;
		ba.szName = "All RAM";
		BurnAcb(&ba);
	}

	if (nAction & ACB_DRIVER_DATA) {
		TMS34010Scan(nAction);

		SCAN_VAR(nVideoBank);
		SCAN_VAR(nDMA);
		SCAN_VAR(nTUnitCtrl);
		SCAN_VAR(bCMOSWriteEnable);
		// Might need to scan the dma_state struct in midtunit_dma.h
	}

	if (nAction & ACB_WRITE) {
		//
	}

	return 0;
}
