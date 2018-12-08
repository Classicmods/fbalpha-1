#include "driver.h"
#include "burnint.h"
#include "midtunit.h"

static struct BurnInputInfo MkInputList[] = {
	{ "Coin 1",			BIT_DIGITAL,	nTUnitJoy2 + 0,	 "p1 coin"   },
	{ "Coin 2",			BIT_DIGITAL,	nTUnitJoy2 + 1,	 "p2 coin"   },
	{ "Coin 3",			BIT_DIGITAL,	nTUnitJoy2 + 7,	 "p1 coin"   },
	{ "Coin 4",			BIT_DIGITAL,	nTUnitJoy2 + 8,	 "p2 coin"   },
	
	{ "P1 Start",		BIT_DIGITAL,	nTUnitJoy2 + 2,	 "p1 start"  },
	{ "P1 Up",			BIT_DIGITAL,	nTUnitJoy1 + 0,	 "p1 up"     },
	{ "P1 Down",		BIT_DIGITAL,	nTUnitJoy1 + 1,	 "p1 down"   },
	{ "P1 Left",		BIT_DIGITAL,	nTUnitJoy1 + 2,	 "p1 left"   },
	{ "P1 Right",		BIT_DIGITAL,	nTUnitJoy1 + 3,	 "p1 right"  },
	{ "P1 Button 1",	BIT_DIGITAL,	nTUnitJoy1 + 4,	 "p1 fire 1" },
	{ "P1 Button 2",	BIT_DIGITAL,	nTUnitJoy1 + 5,	 "p1 fire 2" },
	{ "P1 Button 3",	BIT_DIGITAL,	nTUnitJoy1 + 6,	 "p1 fire 3" },
	{ "P1 Button 4",	BIT_DIGITAL,	nTUnitJoy2 + 12, "p1 fire 4" },
	{ "P1 Button 5",	BIT_DIGITAL,	nTUnitJoy2 + 13, "p1 fire 5" },
	{ "P1 Button 6",	BIT_DIGITAL,	nTUnitJoy2 + 15, "p1 fire 6" },
	
	{ "P2 Start",		BIT_DIGITAL,	nTUnitJoy2 + 5,	 "p2 start"  },
	{ "P2 Up",			BIT_DIGITAL,	nTUnitJoy1 + 8,	 "p2 up"     },
	{ "P2 Down",		BIT_DIGITAL,	nTUnitJoy1 + 9,	 "p2 down"   },
	{ "P2 Left",		BIT_DIGITAL,	nTUnitJoy1 + 10, "p2 left"   },
	{ "P2 Right",		BIT_DIGITAL,	nTUnitJoy1 + 11, "p2 right"  },
	{ "P2 Button 1",	BIT_DIGITAL,	nTUnitJoy1 + 12, "p2 fire 1" },
	{ "P2 Button 2",	BIT_DIGITAL,	nTUnitJoy1 + 13, "p2 fire 2" },
	{ "P2 Button 3",	BIT_DIGITAL,	nTUnitJoy1 + 14, "p2 fire 3" },
	{ "P2 Button 4",	BIT_DIGITAL,	nTUnitJoy2 + 9,  "p2 fire 4" },
	{ "P2 Button 5",	BIT_DIGITAL,	nTUnitJoy2 + 10, "p2 fire 5" },
	{ "P2 Button 6",	BIT_DIGITAL,	nTUnitJoy2 + 11, "p2 fire 6" },

	{ "Reset",			BIT_DIGITAL,	&nTUnitReset,    "reset"     },
	{ "Service",		BIT_DIGITAL,	nTUnitJoy2 + 6,  "service"   },
	{ "Service Mode",	BIT_DIGITAL,	nTUnitJoy2 + 4,  "diag"      },
	{ "Tilt",			BIT_DIGITAL,	nTUnitJoy2 + 3,  "tilt"      },
	{ "Dip A",			BIT_DIPSWITCH,	nTUnitDSW + 0,   "dip"       },
	{ "Dip B",			BIT_DIPSWITCH,	nTUnitDSW + 1,   "dip"       },
};

STDINPUTINFO(Mk)

static struct BurnDIPInfo MkDIPList[]=
{
	{0x1e, 0xff, 0xff, 0x7d, NULL                },
	{0x1f, 0xff, 0xff, 0xf0, NULL                },
	
	{0   , 0xfe, 0   ,    2, "Test Switch"       },
	{0x1e, 0x01, 0x01, 0x01, "Off"               },
	{0x1e, 0x01, 0x01, 0x00, "On"                },

	{0   , 0xfe, 0   ,    2, "Counters"          },
	{0x1e, 0x01, 0x02, 0x02, "One"               },
	{0x1e, 0x01, 0x02, 0x00, "Two"               },

	{0   , 0xfe, 0   ,   19, "Coinage"           },
	{0x1e, 0x01, 0x7c, 0x7c, "USA-1"             },
	{0x1e, 0x01, 0x7c, 0x3c, "USA-2"             },
	{0x1e, 0x01, 0x7c, 0x5c, "USA-3"             },
	{0x1e, 0x01, 0x7c, 0x1c, "USA-4"             },
	{0x1e, 0x01, 0x7c, 0x6c, "USA-ECA"           },
	{0x1e, 0x01, 0x7c, 0x0c, "USA-Free Play"     },
	{0x1e, 0x01, 0x7c, 0x74, "German-1"          },
	{0x1e, 0x01, 0x7c, 0x34, "German-2"          },
	{0x1e, 0x01, 0x7c, 0x54, "German-3"          },
	{0x1e, 0x01, 0x7c, 0x14, "German-4"          },
	{0x1e, 0x01, 0x7c, 0x64, "German-5"          },
	{0x1e, 0x01, 0x7c, 0x24, "German-ECA"        },
	{0x1e, 0x01, 0x7c, 0x04, "German-Free Play"  },
	{0x1e, 0x01, 0x7c, 0x78, "French-1"          },
	{0x1e, 0x01, 0x7c, 0x38, "French-2"          },
	{0x1e, 0x01, 0x7c, 0x58, "French-3"          },
	{0x1e, 0x01, 0x7c, 0x18, "French-4"          },
	{0x1e, 0x01, 0x7c, 0x68, "French-ECA"        },
	{0x1e, 0x01, 0x7c, 0x08, "French-Free Play"  },

	{0   , 0xfe, 0   ,    2, "Coinage Source"    },
	{0x1e, 0x01, 0x80, 0x80, "Dipswitch"         },
	{0x1e, 0x01, 0x80, 0x00, "CMOS"              },

	{0   , 0xfe, 0   ,    2, "Skip Powerup Test" },
	{0x1f, 0x01, 0x01, 0x01, "Off"               },
	{0x1f, 0x01, 0x01, 0x00, "On"                },
	
	{0   , 0xfe, 0   ,    2, "Comic Book Offer"  },
	{0x1f, 0x01, 0x08, 0x00, "Off"               },
	{0x1f, 0x01, 0x08, 0x08, "On"                },
	
	{0   , 0xfe, 0   ,    2, "Attract Sound"     },
	{0x1f, 0x01, 0x10, 0x00, "Off"               },
	{0x1f, 0x01, 0x10, 0x10, "On"                },
	
	{0   , 0xfe, 0   ,    2, "Low Blows"         },
	{0x1f, 0x01, 0x20, 0x00, "Off"               },
	{0x1f, 0x01, 0x20, 0x20, "On"                },

	{0   , 0xfe, 0   ,    2, "Blood"             },
	{0x1f, 0x01, 0x40, 0x00, "Off"               },
	{0x1f, 0x01, 0x40, 0x40, "On"                },

	{0   , 0xfe, 0   ,    2, "Violence"          },
	{0x1f, 0x01, 0x80, 0x00, "Off"               },
	{0x1f, 0x01, 0x80, 0x80, "On"                },
};

STDDIPINFO(Mk)

static struct BurnRomInfo mkRomDesc[] = {
	{ "mkt-uj12.bin",	0x080000, 0xf4990bf2, 1 | BRF_PRG | BRF_ESS }, //  0 TMS34010
	{ "mkt-ug12.bin",	0x080000, 0xb06aeac1, 1 | BRF_PRG | BRF_ESS }, //  1
	
	{ "sl1_mortal_kombat_u12_sound_rom.u12",	0x040000, 0x258bd7f9, 2 | BRF_PRG | BRF_ESS }, //  2 ADPCM sound banks
	{ "sl1_mortal_kombat_u13_sound_rom.u13",	0x040000, 0x7b7ec3b6, 2 | BRF_PRG | BRF_ESS }, //  3

	{ "mkt-ug14.bin",	0x080000, 0x9e00834e, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x00, 0) }, //  4 GFX
	{ "mkt-uj14.bin",	0x080000, 0xf4b0aaa7, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x00, 1) }, //  5
	{ "mkt-ug19.bin",	0x080000, 0x2d8c7ba1, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x00, 2) }, //  6
	{ "mkt-uj19.bin",	0x080000, 0x33b9b7a4, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x00, 3) }, //  7

	{ "mkt-ug16.bin",	0x080000, 0x52c9d1e5, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x02, 0) }, //  8
	{ "mkt-uj16.bin",	0x080000, 0xc94c58cf, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x02, 1) }, //  9
	{ "mkt-ug20.bin",	0x080000, 0x2f7e55d3, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x02, 2) }, // 10
	{ "mkt-uj20.bin",	0x080000, 0xeae96df0, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x02, 3) }, // 11

	{ "mkt-ug17.bin",	0x080000, 0xe34fe253, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x04, 0) }, // 12
	{ "mkt-uj17.bin",	0x080000, 0xa56e12f5, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x04, 1) }, // 13
	{ "mkt-ug22.bin",	0x080000, 0xb537bb4e, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x04, 2) }, // 14
	{ "mkt-uj22.bin",	0x080000, 0x5e12523b, 3 | BRF_GRA | BRF_ESS | TUNIT_GFX(0x04, 3) }, // 15
	
	{ "sl1_mortal_kombat_u3_sound_rom.u3",	0x040000, 0xc615844c, 4 | BRF_PRG | BRF_ESS  }, // 16 Sound CPU
};

STD_ROM_PICK(mk)
STD_ROM_FN(mk)

struct BurnDriverD BurnDrvMk = {
	"mk", NULL, NULL, NULL, "1992",
	"Mortal Kombat (rev 5.0 T-Unit 03/19/93)\0", NULL, "Midway", "MIDWAY T-Unit",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_16BIT_ONLY, 2, HARDWARE_MIDWAY_TUNIT, GBF_MISC, 0,
	NULL, mkRomInfo, mkRomName, NULL, NULL, MkInputInfo, MkDIPInfo,
    TUnitInit, TUnitExit, TUnitFrame, TUnitDraw, TUnitScan, &nTUnitRecalc, 0x8000,
    400, 254, 4, 3
};
