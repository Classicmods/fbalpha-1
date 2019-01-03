#include <vector>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>

#include "libretro.h"
#include "burner.h"

#include "retro_common.h"
#include "retro_cdemu.h"
#include "retro_input.h"
#include "retro_memory.h"

#include <file/file_path.h>

#include <streams/file_stream.h>

#define FBA_VERSION "v0.2.97.44"

static void log_dummy(enum retro_log_level level, const char *fmt, ...) { }
static const char *print_label(unsigned i);

static void set_controller_infos();
static bool apply_dipswitch_from_variables();

static void init_audio_buffer(INT32 sample_rate, INT32 fps);

retro_log_printf_t log_cb = log_dummy;
retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_audio_sample_batch_t audio_batch_cb;

#define BPRINTF_BUFFER_SIZE 512
char bprintf_buf[BPRINTF_BUFFER_SIZE];
static INT32 __cdecl libretro_bprintf(INT32 nStatus, TCHAR* szFormat, ...)
{
	va_list vp;
	va_start(vp, szFormat);
	int rc = vsnprintf(bprintf_buf, BPRINTF_BUFFER_SIZE, szFormat, vp);
	va_end(vp);

	if (rc >= 0)
	{
		retro_log_level retro_log = RETRO_LOG_DEBUG;
		if (nStatus == PRINT_UI)
			retro_log = RETRO_LOG_INFO;
		else if (nStatus == PRINT_IMPORTANT)
			retro_log = RETRO_LOG_WARN;
		else if (nStatus == PRINT_ERROR)
			retro_log = RETRO_LOG_ERROR;

		log_cb(retro_log, bprintf_buf);
	}

	return rc;
}

INT32 (__cdecl *bprintf) (INT32 nStatus, TCHAR* szFormat, ...) = libretro_bprintf;

// FBARL ---

int kNetGame = 0;
INT32 nReplayStatus = 0;
static unsigned nGameType = 0;
#ifdef USE_CYCLONE
// 0 - c68k, 1 - m68k
// we don't use cyclone by default because it breaks savestates cross-platform compatibility (including netplay)
int nSekCpuCore = 1;
static bool cyclone_enabled = false;
#endif

static unsigned int BurnDrvGetIndexByName(const char* name);
char* DecorateGameName(UINT32 nBurnDrv);

extern INT32 EnableHiscores;

#define STAT_NOFIND  0
#define STAT_OK      1
#define STAT_CRC     2
#define STAT_SMALL   3
#define STAT_LARGE   4

struct ROMFIND
{
	unsigned int nState;
	int nArchive;
	int nPos;
	BurnRomInfo ri;
};

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

INT32 nAudSegLen = 0;

static uint32_t *g_fba_frame;
static int16_t *g_audio_buf;
static int16_t *neocd_ibuf;
static float *neocd_fbuf;

// Mapping of PC inputs to game inputs
struct GameInp* GameInp = NULL;
UINT32 nGameInpCount = 0;
UINT32 nMacroCount = 0;
UINT32 nMaxMacro = 0;
INT32 nAnalogSpeed = 0x0100;
INT32 nFireButtons = 0;
bool bStreetFighterLayout = false;
bool bVolumeIsFireButton = false;

// libretro globals
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

#define RETRO_GAME_TYPE_CV		1
#define RETRO_GAME_TYPE_GG		2
#define RETRO_GAME_TYPE_MD		3
#define RETRO_GAME_TYPE_MSX		4
#define RETRO_GAME_TYPE_PCE		5
#define RETRO_GAME_TYPE_SG1K	6
#define RETRO_GAME_TYPE_SGX		7
#define RETRO_GAME_TYPE_SMS		8
#define RETRO_GAME_TYPE_TG		9
#define RETRO_GAME_TYPE_SPEC	10
#define RETRO_GAME_TYPE_NEOCD	11

void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	// Subsystem (needs to be called now, or it won't work on command line)
	static const struct retro_subsystem_rom_info subsystem_rom[] = {
		{ "Rom", "zip|7z", true, true, true, NULL, 0 },
	};
	static const struct retro_subsystem_rom_info subsystem_iso[] = {
		{ "Iso", "iso|cue", true, true, true, NULL, 0 },
	};
	static const struct retro_subsystem_info subsystems[] = {
		{ "CBS ColecoVision", "cv", subsystem_rom, 1, RETRO_GAME_TYPE_CV },
		{ "MSX 1", "msx", subsystem_rom, 1, RETRO_GAME_TYPE_MSX },
		{ "Nec PC-Engine", "pce", subsystem_rom, 1, RETRO_GAME_TYPE_PCE },
		{ "Nec SuperGrafX", "sgx", subsystem_rom, 1, RETRO_GAME_TYPE_SGX },
		{ "Nec TurboGrafx-16", "tg16", subsystem_rom, 1, RETRO_GAME_TYPE_TG },
		{ "Sega GameGear", "gg", subsystem_rom, 1, RETRO_GAME_TYPE_GG },
		{ "Sega Master System", "sms", subsystem_rom, 1, RETRO_GAME_TYPE_SMS },
		{ "Sega Megadrive", "md", subsystem_rom, 1, RETRO_GAME_TYPE_MD },
		{ "Sega SG-1000", "sg1k", subsystem_rom, 1, RETRO_GAME_TYPE_SG1K },
		{ "ZX Spectrum", "spec", subsystem_rom, 1, RETRO_GAME_TYPE_SPEC },
		{ "Neogeo CD", "neocd", subsystem_iso, 1, RETRO_GAME_TYPE_NEOCD },
		{ NULL },
	};

	environ_cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*)subsystems);
}

char g_driver_name[128];
char g_rom_dir[MAX_PATH];
char g_save_dir[MAX_PATH];
char g_system_dir[MAX_PATH];
char g_autofs_path[MAX_PATH];
extern unsigned int (__cdecl *BurnHighCol) (signed int r, signed int g, signed int b, signed int i);

static bool driver_inited;

void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = "FB Alpha";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = FBA_VERSION GIT_VERSION;
	info->need_fullpath = true;
	info->block_extract = true;
	info->valid_extensions = "iso|zip|7z";
}

/////
static INT32 InputTick();
static void InputMake();

// FBA stubs
unsigned ArcadeJoystick;

int bDrvOkay;
int bRunPause;
bool bAlwaysProcessKeyboardInput;

bool bDoIpsPatch;
void IpsApplyPatches(UINT8 *, char *) {}

TCHAR szAppEEPROMPath[MAX_PATH];
TCHAR szAppHiscorePath[MAX_PATH];
TCHAR szAppSamplesPath[MAX_PATH];
TCHAR szAppBlendPath[MAX_PATH];
TCHAR szAppHDDPath[MAX_PATH];
TCHAR szAppBurnVer[16];

// Replace the char c_find by the char c_replace in the destination c string
char* str_char_replace(char* destination, char c_find, char c_replace)
{
	for (unsigned str_idx = 0; str_idx < strlen(destination); str_idx++)
	{
		if (destination[str_idx] == c_find)
			destination[str_idx] = c_replace;
	}

	return destination;
}

std::vector<retro_input_descriptor> macro_input_descriptors;

static int nDIPOffset;

static void InpDIPSWGetOffset (void)
{
	BurnDIPInfo bdi;
	nDIPOffset = 0;

	for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags == 0xF0)
		{
			nDIPOffset = bdi.nInput;
			log_cb(RETRO_LOG_INFO, "DIP switches offset: %d.\n", bdi.nInput);
			break;
		}
	}
}

void InpDIPSWResetDIPs (void)
{
   int i = 0;
   BurnDIPInfo bdi;
   struct GameInp * pgi = NULL;

   InpDIPSWGetOffset();

   while (BurnDrvGetDIPInfo(&bdi, i) == 0)
   {
      if (bdi.nFlags == 0xFF)
      {
         pgi = GameInp + bdi.nInput + nDIPOffset;
         if (pgi)
            pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);
      }
      i++;
   }
}

static int InpDIPSWInit()
{
   log_cb(RETRO_LOG_INFO, "Initialize DIP switches.\n");

   dipswitch_core_options.clear(); 

   BurnDIPInfo bdi;
   struct GameInp *pgi;

   const char * drvname = BurnDrvGetTextA(DRV_NAME);
   
   if (!drvname)
      return 0;
      
   for (int i = 0, j = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      /* 0xFE is the beginning label for a DIP switch entry */
      /* 0xFD are region DIP switches */
      if ((bdi.nFlags == 0xFE || bdi.nFlags == 0xFD) && bdi.nSetting > 0)
      {
         dipswitch_core_options.push_back(dipswitch_core_option());
         dipswitch_core_option *dip_option = &dipswitch_core_options.back();
         
         // Clean the dipswitch name to creation the core option name (removing space and equal characters)
         char option_name[100];

         // Some dipswitch has no name...
         if (bdi.szText)
         {
            strcpy(option_name, bdi.szText);
         }
         else // ... so, to not hang, we will generate a name based on the position of the dip (DIPSWITCH 1, DIPSWITCH 2...)
         {
            sprintf(option_name, "DIPSWITCH %d", (int)dipswitch_core_options.size());
            log_cb(RETRO_LOG_WARN, "Error in %sDIPList : The DIPSWITCH '%d' has no name. '%s' name has been generated\n", drvname, dipswitch_core_options.size(), option_name);
         }
         
         strncpy(dip_option->friendly_name, option_name, sizeof(dip_option->friendly_name));
         
         str_char_replace(option_name, ' ', '_');
         str_char_replace(option_name, '=', '_');
         
         snprintf(dip_option->option_name, sizeof(dip_option->option_name), "fba-dipswitch-%s-%s", drvname, option_name);

         // Search for duplicate name, and add number to make them unique in the core-options file
         for (int dup_idx = 0, dup_nbr = 1; dup_idx < dipswitch_core_options.size() - 1; dup_idx++) // - 1 to exclude the current one
         {
            if (strcmp(dip_option->option_name, dipswitch_core_options[dup_idx].option_name) == 0)
            {
               dup_nbr++;
               snprintf(dip_option->option_name, sizeof(dip_option->option_name), "fba-dipswitch-%s-%s_%d", drvname, option_name, dup_nbr);
            }
         }

         // Reserve space for the default value
         dip_option->values.reserve(bdi.nSetting + 1); // + 1 for default value
         dip_option->values.assign(bdi.nSetting + 1, dipswitch_core_option_value());

         int values_count = 0;
         bool skip_unusable_option = false;
         for (int k = 0; values_count < bdi.nSetting; k++)
         {
            BurnDIPInfo bdi_value;
            if (BurnDrvGetDIPInfo(&bdi_value, k + i + 1) != 0)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': End of the struct was reached too early\n", drvname, dip_option->friendly_name);
               break;
            }
            
            if (bdi_value.nFlags == 0xFE || bdi_value.nFlags == 0xFD)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': Start of next DIPSWITCH is too early\n", drvname, dip_option->friendly_name);
               break;
            }
            
            struct GameInp *pgi_value = GameInp + bdi_value.nInput + nDIPOffset;

            // When the pVal of one value is NULL => the DIP switch is unusable. So it will be skipped by removing it from the list
            if (pgi_value->Input.pVal == 0)
            {
               skip_unusable_option = true;
               break;
            }
               
            // Filter away NULL entries
            if (bdi_value.nFlags == 0)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': the line '%d' is useless\n", drvname, dip_option->friendly_name, k + 1);
               continue;
            }
            
            dipswitch_core_option_value *dip_value = &dip_option->values[values_count + 1]; // + 1 to skip the default value
            
            BurnDrvGetDIPInfo(&(dip_value->bdi), k + i + 1);
            dip_value->pgi = pgi_value;
            strncpy(dip_value->friendly_name, dip_value->bdi.szText, sizeof(dip_value->friendly_name));

            bool is_default_value = (dip_value->pgi->Input.Constant.nConst & dip_value->bdi.nMask) == (dip_value->bdi.nSetting);

            if (is_default_value)
            {
               dipswitch_core_option_value *default_dip_value = &dip_option->values[0];

               default_dip_value->bdi = dip_value->bdi;
               default_dip_value->pgi = dip_value->pgi;
             
               snprintf(default_dip_value->friendly_name, sizeof(default_dip_value->friendly_name), "%s %s", "(Default)", default_dip_value->bdi.szText);
            }

            values_count++;
         }
         
         if (bdi.nSetting > values_count)
         {
            // Truncate the list at the values_count found to not have empty values
            dip_option->values.resize(values_count + 1); // +1 for default value
            log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': '%d' values were intended and only '%d' were found\n", drvname, dip_option->friendly_name, bdi.nSetting, values_count);
         }
         
         // Skip the unusable option by removing it from the list
         if (skip_unusable_option)
         {
            dipswitch_core_options.pop_back();
            continue;
         }

         pgi = GameInp + bdi.nInput + nDIPOffset;
         
         // Create the string values for the core option
         dip_option->values_str.assign(dip_option->friendly_name);
         dip_option->values_str.append("; ");
         
         log_cb(RETRO_LOG_INFO, "'%s' (%d)\n", dip_option->friendly_name, dip_option->values.size() - 1); // -1 to exclude the Default from the DIP Switch count
         for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
         {
            dip_option->values_str.append(dip_option->values[dip_value_idx].friendly_name);
            if (dip_value_idx != dip_option->values.size() - 1)
               dip_option->values_str.append("|");
            
            log_cb(RETRO_LOG_INFO, "   '%s'\n", dip_option->values[dip_value_idx].friendly_name);
         }
         std::basic_string<char>(dip_option->values_str).swap(dip_option->values_str);

         j++;
      }
   }

   evaluate_neogeo_bios_mode(drvname);

   set_environment();
   apply_dipswitch_from_variables();

   return 0;
}

static void set_controller_infos()
{
	static const struct retro_controller_description controller_description[] = {
		{ "Classic", RETROPAD_CLASSIC },
		{ "Modern", RETROPAD_MODERN },
		{ "Mouse (ball only)", RETROMOUSE_BALL },
		{ "Mouse (full)", RETROMOUSE_FULL }
	};

	std::vector<retro_controller_info> controller_infos(nMaxPlayers+1);

	for (int i = 0; i < nMaxPlayers; i++)
	{
		controller_infos[i].types = controller_description;
		controller_infos[i].num_types = sizeof(controller_description)/sizeof(*controller_description);
	}

	controller_infos[nMaxPlayers].types = NULL;
	controller_infos[nMaxPlayers].num_types = 0;

	environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, controller_infos.data());
}

// Update DIP switches value  depending of the choice the user made in core options
static bool apply_dipswitch_from_variables()
{
	bool dip_changed = false;
#if 0
	log_cb(RETRO_LOG_INFO, "Apply DIP switches value from core options.\n");
#endif
	struct retro_variable var = {0};

	for (int dip_idx = 0; dip_idx < dipswitch_core_options.size(); dip_idx++)
	{
		dipswitch_core_option *dip_option = &dipswitch_core_options[dip_idx];

		// Games which needs a specific bios don't handle alternative bioses very well
		if (is_neogeo_game && !allow_neogeo_mode && strcasecmp(dip_option->friendly_name, "BIOS") == 0)
			continue;

		var.key = dip_option->option_name;
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) == false)
			continue;

		for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
		{
			dipswitch_core_option_value *dip_value = &(dip_option->values[dip_value_idx]);

			if (strcasecmp(var.value, dip_value->friendly_name) != 0)
				continue;

			int old_nConst = dip_value->pgi->Input.Constant.nConst;

			dip_value->pgi->Input.Constant.nConst = (dip_value->pgi->Input.Constant.nConst & ~dip_value->bdi.nMask) | (dip_value->bdi.nSetting & dip_value->bdi.nMask);
			dip_value->pgi->Input.nVal = dip_value->pgi->Input.Constant.nConst;
			if (dip_value->pgi->Input.pVal)
				*(dip_value->pgi->Input.pVal) = dip_value->pgi->Input.nVal;

			if (dip_value->pgi->Input.Constant.nConst == old_nConst)
			{
#if 0
				log_cb(RETRO_LOG_INFO, "DIP switch at PTR: [%-10d] [0x%02x] -> [0x%02x] - No change - '%s' '%s' [0x%02x]\n",
				dip_value->pgi->Input.pVal, old_nConst, dip_value->pgi->Input.Constant.nConst, dip_option->friendly_name, dip_value->friendly_name, dip_value->bdi.nSetting);
#endif
			}
			else
			{
				dip_changed = true;
#if 0
				log_cb(RETRO_LOG_INFO, "DIP switch at PTR: [%-10d] [0x%02x] -> [0x%02x] - Changed   - '%s' '%s' [0x%02x]\n",
				dip_value->pgi->Input.pVal, old_nConst, dip_value->pgi->Input.Constant.nConst, dip_option->friendly_name, dip_value->friendly_name, dip_value->bdi.nSetting);
#endif
			}
		}
	}

	// Override the NeoGeo bios DIP Switch by the main one (for the moment)
	if (is_neogeo_game)
		set_neo_system_bios();

	return dip_changed;
}

int InputSetCooperativeLevel(const bool bExclusive, const bool bForeGround) { return 0; }

void Reinitialise(void)
{
    // Update the geometry, some games (sfiii2) and systems (megadrive) need it.
    struct retro_system_av_info av_info;
    retro_get_system_av_info(&av_info);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

static void ForceFrameStep(int bDraw)
{
	nBurnLayer = 0xff;

#ifdef FBA_DEBUG
	nFramesEmulated++;
#endif
	nCurrentFrame++;

	if (!bDraw)
		pBurnDraw = NULL;
#ifdef FBA_DEBUG
	else
		nFramesRendered++;
#endif
	BurnDrvFrame();
}

// Non-idiomatic (OutString should be to the left to match strcpy())
// Seems broken to not check nOutSize.
char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, int /*nOutSize*/)
{
   if (pszOutString)
   {
      strcpy(pszOutString, pszInString);
      return pszOutString;
   }

   return (char*)pszInString;
}

const int nConfigMinVersion = 0x020921;

// addition to support loading of roms without crc check
static int find_rom_by_name(char *name, const ZipEntry *list, unsigned elems)
{
   unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if( strcmp(list[i].szName, name) == 0 )
      {
         return i;
      }
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Not found: %s (name = %s)\n", list[i].szName, name);
#endif

   return -1;
}

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
   unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
     {
         return i;
     }
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Not found: 0x%X (crc: 0x%X)\n", list[i].nCrc, crc);
#endif

   return -1;
}

static RomBiosInfo* find_bios_info(char *szName, uint32_t crc, struct RomBiosInfo* bioses)
{
   for (int i = 0; bioses[i].filename != NULL; i++)
   {
      if (strcmp(bioses[i].filename, szName) == 0 || bioses[i].crc == crc)
      {
         return &bioses[i];
      }
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Bios not found: %s (crc: 0x%08x)\n", szName, crc);
#endif

   return NULL;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      for (unsigned i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static int archive_load_rom(uint8_t *dest, int *wrote, int i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}

static void locate_archive(std::vector<std::string>& pathList, const char* const romName)
{
	static char path[MAX_PATH];

	// Search rom dir
	snprintf(path, sizeof(path), "%s%c%s", g_rom_dir, path_default_slash_c(), romName);
	if (ZipOpen(path) == 0)
	{
		g_find_list_path.push_back(path);
		return;
	}
	// Search system fba subdirectory (where samples/hiscore are stored)
	snprintf(path, sizeof(path), "%s%cfba%c%s", g_system_dir, path_default_slash_c(), path_default_slash_c(), romName);
	if (ZipOpen(path) == 0)
	{
		g_find_list_path.push_back(path);
		return;
	}
	// Search system directory
	snprintf(path, sizeof(path), "%s%c%s", g_system_dir, path_default_slash_c(), romName);
	if (ZipOpen(path) == 0)
	{
		g_find_list_path.push_back(path);
		return;
	}

	log_cb(RETRO_LOG_ERROR, "[FBA] Couldn't locate the %s archive anywhere, this game probably won't boot.\n", romName);
}

// This code is very confusing. The original code is even more confusing :(
static bool open_archive()
{
   memset(g_find_list, 0, sizeof(g_find_list));

   // FBA wants some roms ... Figure out how many.
   g_rom_count = 0;
   while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
      g_rom_count++;

   g_find_list_path.clear();

   // Check if we have said archives.
   // Check if archives are found. These are relative to g_rom_dir.
   char *rom_name;
   for (unsigned index = 0; index < 32; index++)
   {
      if (BurnDrvGetZipName(&rom_name, index))
         continue;

      log_cb(RETRO_LOG_INFO, "[FBA] Archive: %s\n", rom_name);

      locate_archive(g_find_list_path, rom_name);
      
      // Handle bios for pgm single pcb board (special case)
      if (strcmp(rom_name, "thegladpcb") == 0 || strcmp(rom_name, "dmnfrntpcb") == 0 || strcmp(rom_name, "svgpcb") == 0)
      {
         locate_archive(g_find_list_path, "pgm");
      }

      ZipClose();
   }

   for (unsigned z = 0; z < g_find_list_path.size(); z++)
   {
      if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
      {
         log_cb(RETRO_LOG_ERROR, "[FBA] Failed to open archive %s\n", g_find_list_path[z].c_str());
         return false;
      }

      ZipEntry *list = NULL;
      int count;
      ZipGetList(&list, &count);

      // Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
      for (unsigned i = 0; i < g_rom_count; i++)
      {
         if (g_find_list[i].nState == STAT_OK)
            continue;

         if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
         {
            g_find_list[i].nState = STAT_OK;
            continue;
         }

         int index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);

         BurnDrvGetRomName(&rom_name, i, 0);

         bool bad_crc = false;

         if (index < 0)
         {
            index = find_rom_by_name(rom_name, list, count);
            if (index >= 0)
               bad_crc = true;
         }

         if (index >= 0)
         {
            if (bad_crc)
               log_cb(RETRO_LOG_WARN, "[FBA] Using ROM with bad CRC and name %s from archive %s\n", rom_name, g_find_list_path[z].c_str());
            else
               log_cb(RETRO_LOG_INFO, "[FBA] Using ROM with good CRC and name %s from archive %s\n", rom_name, g_find_list_path[z].c_str());
         }
         else
         {
            continue;
         }

         // Search for the best bios available by category
         if (is_neogeo_game)
         {
            RomBiosInfo *bios;

            // MVS BIOS
            bios = find_bios_info(list[index].szName, list[index].nCrc, mvs_bioses);
            if (bios)
            {
               if (!available_mvs_bios || (available_mvs_bios && bios->priority < available_mvs_bios->priority))
                  available_mvs_bios = bios;
            }

            // AES BIOS
            bios = find_bios_info(list[index].szName, list[index].nCrc, aes_bioses);
            if (bios)
            {
               if (!available_aes_bios || (available_aes_bios && bios->priority < available_aes_bios->priority))
                  available_aes_bios = bios;
            }

            // Universe BIOS
            bios = find_bios_info(list[index].szName, list[index].nCrc, uni_bioses);
            if (bios)
            {
               if (!available_uni_bios || (available_uni_bios && bios->priority < available_uni_bios->priority))
                  available_uni_bios = bios;
            }
         }
         
         // Yay, we found it!
         g_find_list[i].nArchive = z;
         g_find_list[i].nPos = index;
         g_find_list[i].nState = STAT_OK;

         if (list[index].nLen < g_find_list[i].ri.nLen)
            g_find_list[i].nState = STAT_SMALL;
         else if (list[index].nLen > g_find_list[i].ri.nLen)
            g_find_list[i].nState = STAT_LARGE;
      }

      free_archive_list(list, count);
      ZipClose();
   }

   bool is_neogeo_bios_available = false;
   if (is_neogeo_game)
   {
      if (!available_mvs_bios && !available_aes_bios && !available_uni_bios)
      {
         log_cb(RETRO_LOG_WARN, "[FBA] NeoGeo BIOS missing ...\n");
      }
      
      set_neo_system_bios();
      
      // if we have a least one type of bios, we will be able to skip the asia-s3.rom non optional bios
      if (available_mvs_bios || available_aes_bios || available_uni_bios)
      {
         is_neogeo_bios_available = true;
      }
   }

   // Going over every rom to see if they are properly loaded before we continue ...
   for (unsigned i = 0; i < g_rom_count; i++)
   {
      if (g_find_list[i].nState != STAT_OK)
      {
         if(!(g_find_list[i].ri.nType & BRF_OPT))
         {
            // make the asia-s3.rom [0x91B64BE3] (mvs_bioses[0]) optional if we have another bios available
            if (is_neogeo_game && g_find_list[i].ri.nCrc == mvs_bioses[0].crc && is_neogeo_bios_available)
               continue;

            log_cb(RETRO_LOG_ERROR, "[FBA] ROM at index %d with CRC 0x%08x is required ...\n", i, g_find_list[i].ri.nCrc);
            return false;
         }
      }
   }

   BurnExtLoadRom = archive_load_rom;
   return true;
}

#ifdef AUTOGEN_DATS
int CreateAllDatfiles()
{
	INT32 nRet = 0;
	TCHAR szFilename[MAX_PATH];

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Arcade only");
	create_datfile(szFilename, DAT_ARCADE_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Megadrive only");
	create_datfile(szFilename, DAT_MEGADRIVE_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, PC-Engine only");
	create_datfile(szFilename, DAT_PCENGINE_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, TurboGrafx16 only");
	create_datfile(szFilename, DAT_TG16_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, SuprGrafx only");
	create_datfile(szFilename, DAT_SGX_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Sega SG-1000 only");
	create_datfile(szFilename, DAT_SG1000_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, ColecoVision only");
	create_datfile(szFilename, DAT_COLECO_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Master System only");
	create_datfile(szFilename, DAT_MASTERSYSTEM_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Game Gear only");
	create_datfile(szFilename, DAT_GAMEGEAR_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, MSX 1 Games only");
	create_datfile(szFilename, DAT_MSX_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, ZX Spectrum Games only");
	create_datfile(szFilename, DAT_SPECTRUM_ONLY);

	snprintf(szFilename, sizeof(szFilename), "%s%cFB Alpha (%s).dat", "dats", path_default_slash_c(), "ClrMame Pro XML, Neogeo only");
	create_datfile(szFilename, DAT_NEOGEO_ONLY);

	return nRet;
}
#endif

void retro_init()
{
	struct retro_log_callback log;

	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;
	else
		log_cb = log_dummy;

	snprintf(szAppBurnVer, sizeof(szAppBurnVer), "%x.%x.%x.%02x", nBurnVer >> 20, (nBurnVer >> 16) & 0x0F, (nBurnVer >> 8) & 0xFF, nBurnVer & 0xFF);
	BurnLibInit();
#ifdef AUTOGEN_DATS
	CreateAllDatfiles();
#endif
}

void retro_deinit()
{
	if(nGameType == RETRO_GAME_TYPE_NEOCD) {
		audio_mixer_done();
		if (neocd_fbuf)
			free(neocd_fbuf);
		if (neocd_ibuf)
			free(neocd_ibuf);
	}
	BurnLibExit();
	if (g_fba_frame)
		free(g_fba_frame);
	if (g_audio_buf)
		free(g_audio_buf);
}

void retro_reset()
{
   // restore the NeoSystem because it was changed during the gameplay
   if (is_neogeo_game)
      set_neo_system_bios();

   if (pgi_reset)
   {
      pgi_reset->Input.nVal = 1;
      *(pgi_reset->Input.pVal) = pgi_reset->Input.nVal;
   }

   check_variables();

   apply_dipswitch_from_variables();

   ForceFrameStep(1);
}

void retro_run()
{
	int width, height;
	BurnDrvGetVisibleSize(&width, &height);
	pBurnDraw = (uint8_t*)g_fba_frame;

	InputMake();

	ForceFrameStep(nCurrentFrame % nFrameskip == 0);

	unsigned drv_flags = BurnDrvGetFlags();
	uint32_t height_tmp = height;
	size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);

	switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
	{
		case BDF_ORIENTATION_VERTICAL:
		case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
			nBurnPitch = height * pitch_size;
			height = width;
			width = height_tmp;
			break;
		case BDF_ORIENTATION_FLIPPED:
		default:
			nBurnPitch = width * pitch_size;
	}

	video_cb(g_fba_frame, width, height, nBurnPitch);

	// If game is neocd, mix sound tracks into the audio buffer
	if (nGameType == RETRO_GAME_TYPE_NEOCD) {
		memset(neocd_fbuf, 0, nAudSegLen<<2 * sizeof(float));
		audio_mixer_mix(neocd_fbuf, nBurnSoundLen, 1, false);
		convert_float_to_s16(neocd_ibuf, neocd_fbuf, nAudSegLen<<2);
		for (unsigned i = 0; i < nBurnSoundLen; i++)
		{
			g_audio_buf[i * 2]       = CLAMP_I16(g_audio_buf[i * 2] + neocd_ibuf[i * 2]);
			g_audio_buf[(i * 2) + 1] = CLAMP_I16(g_audio_buf[(i * 2) + 1] + neocd_ibuf[(i * 2) + 1]);
		}
	}
	audio_batch_cb(g_audio_buf, nBurnSoundLen);
	bool updated = false;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
	{
		bool old_core_aspect_par = core_aspect_par;
		neo_geo_modes old_g_opt_neo_geo_mode = g_opt_neo_geo_mode;

		check_variables();

		apply_dipswitch_from_variables();

		bool macro_updated = apply_macro_from_variables();
		if (macro_updated)
		{
			// Re-create the list of macro input_descriptors with new values
			init_macro_input_descriptors();
			// Re-assign all the input_descriptors to retroarch
			set_input_descriptors();
		}

		// adjust aspect ratio if the needed
		if (old_core_aspect_par != core_aspect_par)
		{
			struct retro_system_av_info av_info;
			retro_get_system_av_info(&av_info);
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
		}

		// reset the game if the user changed the bios
		if (old_g_opt_neo_geo_mode != g_opt_neo_geo_mode)
		{
			retro_reset();
		}
	}
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_sizes[2];

static int burn_write_state_cb(BurnArea *pba)
{
	memcpy(write_state_ptr, pba->Data, pba->nLen);
	write_state_ptr += pba->nLen;
	return 0;
}

static int burn_read_state_cb(BurnArea *pba)
{
	memcpy(pba->Data, read_state_ptr, pba->nLen);
	read_state_ptr += pba->nLen;
	return 0;
}

static int burn_dummy_state_cb(BurnArea *pba)
{
#ifdef FBA_DEBUG
	log_cb(RETRO_LOG_INFO, "state debug: name %s, len %d\n", pba->szName, pba->nLen);
#endif
	state_sizes[kNetGame] += pba->nLen;
	return 0;
}

size_t retro_serialize_size()
{
	int result = -1;
	environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
	kNetGame = result & 4 ? 1 : 0;
	// hiscores are causing desync in netplay
	if (kNetGame == 1)
		EnableHiscores = false;
	if (state_sizes[kNetGame])
		return state_sizes[kNetGame];

	BurnAcb = burn_dummy_state_cb;
	BurnAreaScan(ACB_FULLSCAN, 0);
	return state_sizes[kNetGame];
}

bool retro_serialize(void *data, size_t size)
{
	int result = -1;
	environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
	kNetGame = result & 4 ? 1 : 0;
	// hiscores are causing desync in netplay
	if (kNetGame == 1)
		EnableHiscores = false;
	if (!state_sizes[kNetGame])
	{
		BurnAcb = burn_dummy_state_cb;
		BurnAreaScan(ACB_FULLSCAN, 0);
	}
	if (size != state_sizes[kNetGame])
		return false;

	BurnAcb = burn_write_state_cb;
	write_state_ptr = (uint8_t*)data;
	BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);   
	return true;
}

bool retro_unserialize(const void *data, size_t size)
{
	int result = -1;
	environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
	kNetGame = result & 4 ? 1 : 0;
	// hiscores are causing desync in netplay
	if (kNetGame == 1)
		EnableHiscores = false;
	if (!state_sizes[kNetGame])
	{
		BurnAcb = burn_dummy_state_cb;
		BurnAreaScan(ACB_FULLSCAN, 0);
	}
	if (size != state_sizes[kNetGame])
		return false;

	BurnAcb = burn_read_state_cb;
	read_state_ptr = (const uint8_t*)data;
	BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, 0);
	// Recalculating the palette, this is probably only necessary when dealing with normal savestates usage
	if (kNetGame == 0)
		BurnRecalcPal();
	return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	int width, height, game_aspect_x, game_aspect_y;
	BurnDrvGetVisibleSize(&width, &height);
	BurnDrvGetAspect(&game_aspect_x, &game_aspect_y);
	if (bVerticalMode)
	{
		int tmp_height = width;
		width = height;
		height = tmp_height;
	}
	int maximum = width > height ? width : height;
	struct retro_game_geometry geom = { (unsigned)width, (unsigned)height, (unsigned)maximum, (unsigned)maximum };


	if (game_aspect_x != 0 && game_aspect_y != 0 && !core_aspect_par)
	{
		geom.aspect_ratio = (float)game_aspect_x / (float)game_aspect_y;
		if (bVerticalMode)
			geom.aspect_ratio = (float)game_aspect_y / (float)game_aspect_x;
		log_cb(RETRO_LOG_INFO, "retro_get_system_av_info: base_width: %d, base_height: %d, max_width: %d, max_height: %d, aspect_ratio: (%d/%d) = %f (core_aspect_par: %d)\n", geom.base_width, geom.base_height, geom.max_width, geom.max_height, game_aspect_x, game_aspect_y, geom.aspect_ratio, core_aspect_par);
	}
	else
	{
		log_cb(RETRO_LOG_INFO, "retro_get_system_av_info: base_width: %d, base_height: %d, max_width: %d, max_height: %d, aspect_ratio: %f\n", geom.base_width, geom.base_height, geom.max_width, geom.max_height, geom.aspect_ratio);
	}

	struct retro_system_timing timing = { (nBurnFPS / 100.0), (nBurnFPS / 100.0) * nAudSegLen };

	info->geometry = geom;
	info->timing   = timing;
}

int VidRecalcPal()
{
   return BurnRecalcPal();
}

// Standard callbacks for 16/24/32 bit color:
static UINT32 __cdecl HighCol15(INT32 r, INT32 g, INT32 b, INT32  /* i */)
{
	UINT32 t;
	t =(r<<7)&0x7c00; // 0rrr rr00 0000 0000
	t|=(g<<2)&0x03e0; // 0000 00gg ggg0 0000
	t|=(b>>3)&0x001f; // 0000 0000 000b bbbb
	return t;
}

static UINT32 __cdecl HighCol16(INT32 r, INT32 g, INT32 b, INT32 /* i */)
{
	UINT32 t;
	t =(r<<8)&0xf800; // rrrr r000 0000 0000
	t|=(g<<3)&0x07e0; // 0000 0ggg ggg0 0000
	t|=(b>>3)&0x001f; // 0000 0000 000b bbbb
	return t;
}

// 24-bit/32-bit
static UINT32 __cdecl HighCol24(INT32 r, INT32 g, INT32 b, INT32  /* i */)
{
	UINT32 t;
	t =(r<<16)&0xff0000;
	t|=(g<<8 )&0x00ff00;
	t|=(b    )&0x0000ff;

	return t;
}

INT32 SetBurnHighCol(INT32 nDepth)
{
	VidRecalcPal();
	
	if (nDepth == 15) {
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
		if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		{
			nBurnBpp = 2;
			BurnHighCol = HighCol15;
		}
	}
	
	if (nDepth == 16) {
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
		if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		{
			nBurnBpp = 2;
			BurnHighCol = HighCol16;
		}
	}
	
	if (nDepth == 24) {
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
		if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		{
			nBurnBpp = 3;
			BurnHighCol = HighCol24;
		}
	}
	
	if (nDepth == 32) {
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
		if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		{
			nBurnBpp = 4;
			BurnHighCol = HighCol24;
		}
	}

	return 0;
}

static void init_audio_buffer(INT32 sample_rate, INT32 fps)
{
	// [issue #206]
	// For games where sample_rate/1000 > fps/100
	// we don't change nBurnSoundRate, but we adjust some length
	if ((sample_rate / 1000) > (fps / 100))
		sample_rate = fps * 10;
	nAudSegLen = (sample_rate * 100 + (fps >> 1)) / fps;
	if (g_audio_buf)
		free(g_audio_buf);
	g_audio_buf = (int16_t*)malloc(nAudSegLen<<2 * sizeof(int16_t));
	// If game is neocd, allocate buffers for sound tracks
	if (nGameType == RETRO_GAME_TYPE_NEOCD) {
		if (neocd_fbuf)
			free(neocd_fbuf);
		if (neocd_ibuf)
			free(neocd_ibuf);
		neocd_fbuf = (float*)malloc(nAudSegLen<<2 * sizeof(float));
		neocd_ibuf = (int16_t*)malloc(nAudSegLen<<2 * sizeof(int16_t));
	}
	nBurnSoundLen = nAudSegLen;
	pBurnSoundOut = g_audio_buf;
}

static void extract_basename(char *buf, const char *path, size_t size, char *prefix)
{
   strcpy(buf, prefix);
   strncat(buf, path_basename(path), size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, path_default_slash_c());

   if (base)
      *base = '\0';
   else
    {
       buf[0] = '.';
       buf[1] = '\0';
    }
}

static bool retro_load_game_common()
{
	const char *dir = NULL;
	// If save directory is defined use it, ...
	if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) {
		strncpy(g_save_dir, dir, sizeof(g_save_dir));
		log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", g_save_dir);
	} else {
		// ... otherwise use rom directory
		strncpy(g_save_dir, g_rom_dir, sizeof(g_save_dir));
		log_cb(RETRO_LOG_ERROR, "Save dir not defined => use roms dir %s\n", g_save_dir);
	}

	// If system directory is defined use it, ...
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		strncpy(g_system_dir, dir, sizeof(g_system_dir));
		log_cb(RETRO_LOG_INFO, "Setting system dir to %s\n", g_system_dir);
	} else {
		// ... otherwise use rom directory
		strncpy(g_system_dir, g_rom_dir, sizeof(g_system_dir));
		log_cb(RETRO_LOG_ERROR, "System dir not defined => use roms dir %s\n", g_system_dir);
	}

	// Initialize EEPROM path
	snprintf (szAppEEPROMPath, sizeof(szAppEEPROMPath), "%s%cfba%c", g_save_dir, path_default_slash_c(), path_default_slash_c());

	// Create EEPROM path if it does not exist
	path_mkdir(szAppEEPROMPath);

	// Initialize Hiscore path
	snprintf (szAppHiscorePath, sizeof(szAppHiscorePath), "%s%cfba%c", g_system_dir, path_default_slash_c(), path_default_slash_c());

	// Initialize Samples path
	snprintf (szAppSamplesPath, sizeof(szAppSamplesPath), "%s%cfba%csamples%c", g_system_dir, path_default_slash_c(), path_default_slash_c(), path_default_slash_c());

	// Initialize HDD path
	snprintf (szAppHDDPath, sizeof(szAppHDDPath), "%s%c", g_rom_dir, path_default_slash_c());

	// Intialize state_sizes (for serialization)
	state_sizes[0] = 0;
	state_sizes[1] = 0;

	nBurnDrvActive = BurnDrvGetIndexByName(g_driver_name);
	if (nBurnDrvActive < nBurnDrvCount) {
		const char * boardrom = BurnDrvGetTextA(DRV_BOARDROM);
		is_neogeo_game = (boardrom && strcmp(boardrom, "neogeo") == 0);

		// Define nMaxPlayers early;
		nMaxPlayers = BurnDrvGetMaxPlayers();
		set_controller_infos();

		set_environment();
		check_variables();

#ifdef USE_CYCLONE
		nSekCpuCore = (cyclone_enabled ? 0 : 1);
#endif
		if (!open_archive()) {
			log_cb(RETRO_LOG_ERROR, "[FBA] Can't launch this game, some files are missing.\n");
			return false;
		}

		// Announcing to fba which samplerate we want
		nBurnSoundRate = g_audio_samplerate;

		// Some game drivers won't initialize with an undefined nBurnSoundLen
		init_audio_buffer(nBurnSoundRate, 6000);

		// Initizalize inputs
		init_input();
		input_initialized = true;

		// Start CD reader emulation if needed
		if (nGameType == RETRO_GAME_TYPE_NEOCD) {
			// If game is neocd, we need to start the audio mixer for sound tracks
			audio_mixer_init(nBurnSoundRate);
			if (CDEmuInit()) {
				log_cb(RETRO_LOG_INFO, "[FBA] Starting neogeo CD\n");
			}
		}

		// Initialize dipswitches
		InpDIPSWInit();

		// Initialize game driver
		BurnDrvInit();

		// If the game is marked as not working, let's stop here
		if (!(BurnDrvIsWorking())) {
			log_cb(RETRO_LOG_ERROR, "[FBA] Can't launch this game, it is marked as not working\n");
			return false;
		}

		// Now we know real game fps, let's initialize sound buffer again
		init_audio_buffer(nBurnSoundRate, nBurnFPS);

		// Get MainRam for RetroAchievements support
		INT32 nMin = 0;
		BurnAcb = StateGetMainRamAcb;
		BurnAreaScan(ACB_FULLSCAN, &nMin);
		if (bMainRamFound) {
			log_cb(RETRO_LOG_INFO, "[Cheevos] System RAM set to %p %zu\n", MainRamData, MainRamSize);
		}

		// Loading minimal savestate (not exactly sure why it is needed)
		snprintf (g_autofs_path, sizeof(g_autofs_path), "%s%cfba%c%s.fs", g_save_dir, path_default_slash_c(), path_default_slash_c(), BurnDrvGetTextA(DRV_NAME));
		BurnStateLoad(g_autofs_path, 0, NULL);

		// Initializing display, autorotate if needed
		INT32 width, height;
		BurnDrvGetVisibleSize(&width, &height);
		unsigned drv_flags = BurnDrvGetFlags();
		size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
		if (drv_flags & BDF_ORIENTATION_VERTICAL)
			nBurnPitch = height * pitch_size;
		else
			nBurnPitch = width * pitch_size;
		unsigned rotation;
		switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
		{
			case BDF_ORIENTATION_VERTICAL:
				rotation = (bVerticalMode ? 0 : 1);
				break;
			case BDF_ORIENTATION_FLIPPED:
				rotation = (bVerticalMode ? 1 : 2);
				break;
			case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
				rotation = (bVerticalMode ? 2 : 3);
				break;
			default:
				rotation = (bVerticalMode ? 3 : 0);;
				break;
		}
		environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);
#ifdef FRONTEND_SUPPORTS_RGB565
		SetBurnHighCol(16);
#else
		SetBurnHighCol(15);
#endif
		BurnDrvGetFullSize(&width, &height);
		g_fba_frame = (uint32_t*)malloc(width * height * sizeof(uint32_t));

		// Apply dipswitches
		apply_dipswitch_from_variables();

		// Initialization done
		log_cb(RETRO_LOG_INFO, "Driver %s was successfully started\n", g_driver_name);
		driver_inited = true;

		return true;
	}
	return false;
}

bool retro_load_game(const struct retro_game_info *info)
{
	if (!info)
		return false;

	extract_basename(g_driver_name, info->path, sizeof(g_driver_name), "");
	extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

	return retro_load_game_common();
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t)
{
	if (!info)
		return false;

	nGameType = game_type;

	char * prefix;
	switch (nGameType) {
		case RETRO_GAME_TYPE_CV:
			prefix = "cv_";
			break;
		case RETRO_GAME_TYPE_GG:
			prefix = "gg_";
			break;
		case RETRO_GAME_TYPE_MD:
			prefix = "md_";
			break;
		case RETRO_GAME_TYPE_MSX:
			prefix = "msx_";
			break;
		case RETRO_GAME_TYPE_PCE:
			prefix = "pce_";
			break;
		case RETRO_GAME_TYPE_SG1K:
			prefix = "sg1k_";
			break;
		case RETRO_GAME_TYPE_SGX:
			prefix = "sgx_";
			break;
		case RETRO_GAME_TYPE_SMS:
			prefix = "sms_";
			break;
		case RETRO_GAME_TYPE_SPEC:
			prefix = "spec_";
			break;
		case RETRO_GAME_TYPE_TG:
			prefix = "tg_";
			break;
		case RETRO_GAME_TYPE_NEOCD:
			prefix = "";
			strcpy(CDEmuImage, info->path);
			break;
		default:
			return false;
			break;
	}

	extract_basename(g_driver_name, info->path, sizeof(g_driver_name), prefix);
	extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

	if(nGameType == RETRO_GAME_TYPE_NEOCD)
		extract_basename(g_driver_name, "neocdz", sizeof(g_driver_name), "");

	return retro_load_game_common();
}

void retro_unload_game(void)
{
	if (driver_inited)
	{
		BurnStateSave(g_autofs_path, 0);
		BurnDrvExit();
		CDEmuExit();
	}
	input_initialized = false;
	driver_inited = false;
}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

unsigned retro_api_version() { return RETRO_API_VERSION; }

static const char *print_label(unsigned i)
{
   switch(i)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return "RetroPad B Button";
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return "RetroPad Y Button";
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return "RetroPad Select Button";
      case RETRO_DEVICE_ID_JOYPAD_START:
         return "RetroPad Start Button";
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return "RetroPad D-Pad Up";
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return "RetroPad D-Pad Down";
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return "RetroPad D-Pad Left";
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return "RetroPad D-Pad Right";
      case RETRO_DEVICE_ID_JOYPAD_A:
         return "RetroPad A Button";
      case RETRO_DEVICE_ID_JOYPAD_X:
         return "RetroPad X Button";
      case RETRO_DEVICE_ID_JOYPAD_L:
         return "RetroPad L Button";
      case RETRO_DEVICE_ID_JOYPAD_R:
         return "RetroPad R Button";
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return "RetroPad L2 Button";
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return "RetroPad R2 Button";
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return "RetroPad L3 Button";
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return "RetroPad R3 Button";
      case RETRO_DEVICE_ID_JOYPAD_EMPTY:
         return "None";
      default:
         return "No known label";
   }
}

static inline INT32 CinpState(INT32 nCode)
{
	INT32 id = keybinds[nCode][0];
	UINT32 port = keybinds[nCode][1];
	INT32 idx = keybinds[nCode][2];
	if(idx == 0)
	{
		return input_cb(port, keybinds[nCode][4], 0, id);
	}
	else
	{
		INT32 s = input_cb(port, keybinds[nCode][4], idx, id);
		INT32 position = keybinds[nCode][3];
		// Using a large deadzone when mapping microswitches to analog axis
		// Or said axis become way too sensitive and some game become unplayable (assault)
		if(s < -10000 && position == JOY_NEG)
			return 1;
		if(s > 10000 && position == JOY_POS)
			return 1;
	}
	return 0;
}

static inline int CinpJoyAxis(int i, int axis)
{
	INT32 idx = axibinds[i][axis][0];
	if(idx != 0xff)
	{
		INT32 id = axibinds[i][axis][1];
		return input_cb(i, RETRO_DEVICE_ANALOG, idx, id);
	}
	else
	{
		INT32 idpos = axibinds[i][axis][1];
		INT32 idneg = axibinds[i][axis][2];
		INT32 spos = input_cb(i, RETRO_DEVICE_JOYPAD, 0, idpos);
		INT32 sneg = input_cb(i, RETRO_DEVICE_JOYPAD, 0, idneg);
		return (spos - sneg) * 32768;
	}
	return 0;
}

static inline int CinpMouseAxis(int i, int axis)
{
	// Hooking this won't hurt, however i don't know yet how i'll be including it in current logic
	INT32 id = axibinds[i][axis][1];
	return input_cb(i, RETRO_DEVICE_MOUSE, 0, id);
}

static INT32 InputTick()
{
	struct GameInp *pgi;
	UINT32 i;

	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		INT32 nAdd = 0;
		if ((pgi->nInput &  GIT_GROUP_SLIDER) == 0) {				// not a slider
			continue;
		}

		if (pgi->nInput == GIT_KEYSLIDER) {
			// Get states of the two keys
			if (CinpState(pgi->Input.Slider.SliderAxis.nSlider[0]))	{
				nAdd -= 0x100;
			}
			if (CinpState(pgi->Input.Slider.SliderAxis.nSlider[1]))	{
				nAdd += 0x100;
			}
		}

		if (pgi->nInput == GIT_JOYSLIDER) {
			// Get state of the axis
			nAdd = CinpJoyAxis(pgi->Input.Slider.JoyAxis.nJoy, pgi->Input.Slider.JoyAxis.nAxis);
			nAdd /= 0x100;
		}

		// nAdd is now -0x100 to +0x100

		// Change to slider speed
		nAdd *= pgi->Input.Slider.nSliderSpeed;
		nAdd /= 0x100;

		if (pgi->Input.Slider.nSliderCenter) {						// Attact to center
			INT32 v = pgi->Input.Slider.nSliderValue - 0x8000;
			v *= (pgi->Input.Slider.nSliderCenter - 1);
			v /= pgi->Input.Slider.nSliderCenter;
			v += 0x8000;
			pgi->Input.Slider.nSliderValue = v;
		}

		pgi->Input.Slider.nSliderValue += nAdd;
		// Limit slider
		if (pgi->Input.Slider.nSliderValue < 0x0100) {
			pgi->Input.Slider.nSliderValue = 0x0100;
		}
		if (pgi->Input.Slider.nSliderValue > 0xFF00) {
			pgi->Input.Slider.nSliderValue = 0xFF00;
		}
	}
	return 0;
}

static void InputMake(void)
{
	poll_cb();

	if (poll_diag_input())
		return;

	struct GameInp* pgi;
	UINT32 i;

	InputTick();

	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		if (pgi->Input.pVal == NULL) {
			continue;
		}

		switch (pgi->nInput) {
			case 0:									// Undefined
				pgi->Input.nVal = 0;
				break;
			case GIT_CONSTANT:						// Constant value
				pgi->Input.nVal = pgi->Input.Constant.nConst;
				*(pgi->Input.pVal) = pgi->Input.nVal;
				break;
			case GIT_SWITCH: {						// Digital input
				INT32 s = CinpState(pgi->Input.Switch.nCode);

				if (pgi->nType & BIT_GROUP_ANALOG) {
					// Set analog controls to full
					if (s) {
						pgi->Input.nVal = 0xFFFF;
					} else {
						pgi->Input.nVal = 0x0001;
					}
#ifdef LSB_FIRST
					*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
					*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
				} else {
					// Binary controls
					if (s) {
						pgi->Input.nVal = 1;
					} else {
						pgi->Input.nVal = 0;
					}
					*(pgi->Input.pVal) = pgi->Input.nVal;
				}

				break;
			}
			case GIT_KEYSLIDER:						// Keyboard slider
			case GIT_JOYSLIDER:	{					// Joystick slider
				INT32 nSlider = pgi->Input.Slider.nSliderValue;
				if (pgi->nType == BIT_ANALOG_REL) {
					nSlider -= 0x8000;
					nSlider >>= 4;
				}

				pgi->Input.nVal = (UINT16)nSlider;
#ifdef LSB_FIRST
				*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
				*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
				break;
			}
			case GIT_MOUSEAXIS:						// Mouse axis
				pgi->Input.nVal = (UINT16)(CinpMouseAxis(pgi->Input.MouseAxis.nMouse, pgi->Input.MouseAxis.nAxis) * nAnalogSpeed);
#ifdef LSB_FIRST
				*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
				*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
				break;
			case GIT_JOYAXIS_FULL:	{				// Joystick axis
				INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

				if (pgi->nType == BIT_ANALOG_REL) {
					nJoy *= nAnalogSpeed;
					nJoy >>= 13;

					// Clip axis to 8 bits
					if (nJoy < -32768) {
						nJoy = -32768;
					}
					if (nJoy >  32767) {
						nJoy =  32767;
					}
				} else {
					nJoy >>= 1;
					nJoy += 0x8000;

					// Clip axis to 16 bits
					if (nJoy < 0x0001) {
						nJoy = 0x0001;
					}
					if (nJoy > 0xFFFF) {
						nJoy = 0xFFFF;
					}
				}

				pgi->Input.nVal = (UINT16)nJoy;
#ifdef LSB_FIRST
				*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
				*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif

				break;
			}
			case GIT_JOYAXIS_NEG:	{				// Joystick axis Lo
				INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
				if (nJoy < 32767) {
					nJoy = -nJoy;

					if (nJoy < 0x0000) {
						nJoy = 0x0000;
					}
					if (nJoy > 0xFFFF) {
						nJoy = 0xFFFF;
					}

					pgi->Input.nVal = (UINT16)nJoy;
				} else {
					pgi->Input.nVal = 0;
				}

#ifdef LSB_FIRST
				*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
				*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
				break;
			}
			case GIT_JOYAXIS_POS:	{				// Joystick axis Hi
				INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
				if (nJoy > 32767) {

					if (nJoy < 0x0000) {
						nJoy = 0x0000;
					}
					if (nJoy > 0xFFFF) {
						nJoy = 0xFFFF;
					}

					pgi->Input.nVal = (UINT16)nJoy;
				} else {
					pgi->Input.nVal = 0;
				}

#ifdef LSB_FIRST
				*(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
				*((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
				break;
			}
		}
	}

	for (i = 0; i < nMacroCount; i++, pgi++) {
		if (pgi->Macro.nMode == 1 && pgi->Macro.nSysMacro == 0) { // Macro is defined
			if (CinpState(pgi->Macro.Switch.nCode)) {
				for (INT32 j = 0; j < 4; j++) {
					if (pgi->Macro.pVal[j]) {
						*(pgi->Macro.pVal[j]) = pgi->Macro.nVal[j];
					}
				}
			}
		}
	}
}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int ret = ~0U;
   for (unsigned int i = 0; i < nBurnDrvCount; i++) {
      nBurnDrvActive = i;
      if (strcmp(BurnDrvGetText(DRV_NAME), name) == 0) {
         ret = i;
         break;
      }
   }
   return ret;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif

// Driver Save State module
// If bAll=0 save/load all non-volatile ram to .fs
// If bAll=1 save/load all ram to .fs

// ------------ State len --------------------
static INT32 nTotalLen = 0;

static INT32 __cdecl StateLenAcb(struct BurnArea* pba)
{
   nTotalLen += pba->nLen;

   return 0;
}

static INT32 StateInfo(INT32* pnLen, INT32* pnMinVer, INT32 bAll)
{
   INT32 nMin = 0;
   nTotalLen = 0;
   BurnAcb = StateLenAcb;

   BurnAreaScan(ACB_NVRAM, &nMin);                  // Scan nvram
   if (bAll) {
      INT32 m;
      BurnAreaScan(ACB_MEMCARD, &m);               // Scan memory card
      if (m > nMin) {                           // Up the minimum, if needed
         nMin = m;
      }
      BurnAreaScan(ACB_VOLATILE, &m);               // Scan volatile ram
      if (m > nMin) {                           // Up the minimum, if needed
         nMin = m;
      }
   }
   *pnLen = nTotalLen;
   *pnMinVer = nMin;

   return 0;
}

// State load
INT32 BurnStateLoadEmbed(FILE* fp, INT32 nOffset, INT32 bAll, INT32 (*pLoadGame)())
{
   const char* szHeader = "FS1 ";                  // Chunk identifier

   INT32 nLen = 0;
   INT32 nMin = 0, nFileVer = 0, nFileMin = 0;
   INT32 t1 = 0, t2 = 0;
   char ReadHeader[4];
   char szForName[33];
   INT32 nChunkSize = 0;
   UINT8 *Def = NULL;
   INT32 nDefLen = 0;                           // Deflated version
   INT32 nRet = 0;

   if (nOffset >= 0) {
      fseek(fp, nOffset, SEEK_SET);
   } else {
      if (nOffset == -2) {
         fseek(fp, 0, SEEK_END);
      } else {
         fseek(fp, 0, SEEK_CUR);
      }
   }

   memset(ReadHeader, 0, 4);
   fread(ReadHeader, 1, 4, fp);                  // Read identifier
   if (memcmp(ReadHeader, szHeader, 4)) {            // Not the right file type
      return -2;
   }

   fread(&nChunkSize, 1, 4, fp);
   if (nChunkSize <= 0x40) {                     // Not big enough
      return -1;
   }

   INT32 nChunkData = ftell(fp);

   fread(&nFileVer, 1, 4, fp);                     // Version of FB that this file was saved from

   fread(&t1, 1, 4, fp);                        // Min version of FB that NV  data will work with
   fread(&t2, 1, 4, fp);                        // Min version of FB that All data will work with

   if (bAll) {                                 // Get the min version number which applies to us
      nFileMin = t2;
   } else {
      nFileMin = t1;
   }

   fread(&nDefLen, 1, 4, fp);                     // Get the size of the compressed data block

   memset(szForName, 0, sizeof(szForName));
   fread(szForName, 1, 32, fp);

   if (nBurnVer < nFileMin) {                     // Error - emulator is too old to load this state
      return -5;
   }

   // Check the game the savestate is for, and load it if needed.
   {
      bool bLoadGame = false;

      if (nBurnDrvActive < nBurnDrvCount) {
         if (strcmp(szForName, BurnDrvGetTextA(DRV_NAME))) {   // The save state is for the wrong game
            bLoadGame = true;
         }
      } else {                              // No game loaded
         bLoadGame = true;
      }

      if (bLoadGame) {
         UINT32 nCurrentGame = nBurnDrvActive;
         UINT32 i;
         for (i = 0; i < nBurnDrvCount; i++) {
            nBurnDrvActive = i;
            if (strcmp(szForName, BurnDrvGetTextA(DRV_NAME)) == 0) {
               break;
            }
         }
         if (i == nBurnDrvCount) {
            nBurnDrvActive = nCurrentGame;
            return -3;
         } else {
            if (pLoadGame == NULL) {
               return -1;
            }
            if (pLoadGame()) {
               return -1;
            }
         }
      }
   }

   StateInfo(&nLen, &nMin, bAll);
   if (nLen <= 0) {                           // No memory to load
      return -1;
   }

   // Check if the save state is okay
   if (nFileVer < nMin) {                        // Error - this state is too old and cannot be loaded.
      return -4;
   }

   fseek(fp, nChunkData + 0x30, SEEK_SET);            // Read current frame
   fread(&nCurrentFrame, 1, 4, fp);               //

   fseek(fp, 0x0C, SEEK_CUR);                     // Move file pointer to the start of the compressed block
   Def = (UINT8*)malloc(nDefLen);
   if (Def == NULL) {
      return -1;
   }
   memset(Def, 0, nDefLen);
   fread(Def, 1, nDefLen, fp);                     // Read in deflated block

   nRet = BurnStateDecompress(Def, nDefLen, bAll);      // Decompress block into driver
   if (Def) {
      free(Def);                                 // free deflated block
      Def = NULL;
   }

   fseek(fp, nChunkData + nChunkSize, SEEK_SET);

   if (nRet) {
      return -1;
   } else {
      return 0;
   }
}

// State load
INT32 BurnStateLoad(TCHAR* szName, INT32 bAll, INT32 (*pLoadGame)())
{
   const char szHeader[] = "FB1 ";                  // File identifier
   char szReadHeader[4] = "";
   INT32 nRet = 0;

   FILE* fp = _tfopen(szName, _T("rb"));
   if (fp == NULL) {
      return 1;
   }

   fread(szReadHeader, 1, 4, fp);                  // Read identifier
   if (memcmp(szReadHeader, szHeader, 4) == 0) {      // Check filetype
      nRet = BurnStateLoadEmbed(fp, -1, bAll, pLoadGame);
   }
    fclose(fp);

   if (nRet < 0) {
      return -nRet;
   } else {
      return 0;
   }
}

// Write a savestate as a chunk of an "FB1 " file
// nOffset is the absolute offset from the beginning of the file
// -1: Append at current position
// -2: Append at EOF
INT32 BurnStateSaveEmbed(FILE* fp, INT32 nOffset, INT32 bAll)
{
   const char* szHeader = "FS1 ";                  // Chunk identifier

   INT32 nLen = 0;
   INT32 nNvMin = 0, nAMin = 0;
   INT32 nZero = 0;
   char szGame[33];
   UINT8 *Def = NULL;
   INT32 nDefLen = 0;                           // Deflated version
   INT32 nRet = 0;

   if (fp == NULL) {
      return -1;
   }

   StateInfo(&nLen, &nNvMin, 0);                  // Get minimum version for NV part
   nAMin = nNvMin;
   if (bAll) {                                 // Get minimum version for All data
      StateInfo(&nLen, &nAMin, 1);
   }

   if (nLen <= 0) {                           // No memory to save
      return -1;
   }

   if (nOffset >= 0) {
      fseek(fp, nOffset, SEEK_SET);
   } else {
      if (nOffset == -2) {
         fseek(fp, 0, SEEK_END);
      } else {
         fseek(fp, 0, SEEK_CUR);
      }
   }

   fwrite(szHeader, 1, 4, fp);                     // Chunk identifier
   INT32 nSizeOffset = ftell(fp);                  // Reserve space to write the size of this chunk
   fwrite(&nZero, 1, 4, fp);                     //

   fwrite(&nBurnVer, 1, 4, fp);                  // Version of FB this was saved from
   fwrite(&nNvMin, 1, 4, fp);                     // Min version of FB NV  data will work with
   fwrite(&nAMin, 1, 4, fp);                     // Min version of FB All data will work with

   fwrite(&nZero, 1, 4, fp);                     // Reserve space to write the compressed data size

   memset(szGame, 0, sizeof(szGame));               // Game name
   sprintf(szGame, "%.32s", BurnDrvGetTextA(DRV_NAME));         //
   fwrite(szGame, 1, 32, fp);                     //

   fwrite(&nCurrentFrame, 1, 4, fp);               // Current frame

   fwrite(&nZero, 1, 4, fp);                     // Reserved
   fwrite(&nZero, 1, 4, fp);                     //
   fwrite(&nZero, 1, 4, fp);                     //

   nRet = BurnStateCompress(&Def, &nDefLen, bAll);      // Compress block from driver and return deflated buffer
   if (Def == NULL) {
      return -1;
   }

   nRet = fwrite(Def, 1, nDefLen, fp);               // Write block to disk
   if (Def) {
      free(Def);                                 // free deflated block and close file
      Def = NULL;
   }

   if (nRet != nDefLen) {                        // error writing block to disk
      return -1;
   }

   if (nDefLen & 3) {                           // Chunk size must be a multiple of 4
      fwrite(&nZero, 1, 4 - (nDefLen & 3), fp);      // Pad chunk if needed
   }

   fseek(fp, nSizeOffset + 0x10, SEEK_SET);         // Write size of the compressed data
   fwrite(&nDefLen, 1, 4, fp);                     //

   nDefLen = (nDefLen + 0x43) & ~3;               // Add for header size and align

   fseek(fp, nSizeOffset, SEEK_SET);               // Write size of the chunk
   fwrite(&nDefLen, 1, 4, fp);                     //

   fseek (fp, 0, SEEK_END);                     // Set file pointer to the end of the chunk

   return nDefLen;
}

// State save
INT32 BurnStateSave(TCHAR* szName, INT32 bAll)
{
   const char szHeader[] = "FB1 ";                  // File identifier
   INT32 nLen = 0, nVer = 0;
   INT32 nRet = 0;

   if (bAll) {                                 // Get amount of data
      StateInfo(&nLen, &nVer, 1);
   } else {
      StateInfo(&nLen, &nVer, 0);
   }
   if (nLen <= 0) {                           // No data, so exit without creating a savestate
      return 0;                              // Don't return an error code
   }

   FILE* fp = _tfopen(szName, _T("wb"));
   if (fp == NULL) {
      return 1;
   }

   fwrite(&szHeader, 1, 4, fp);
   nRet = BurnStateSaveEmbed(fp, -1, bAll);
    fclose(fp);

   if (nRet < 0) {
      return 1;
   } else {
      return 0;
   }
}

// Creates core option for the available macros of the game
// These core options will be stored in the macro_core_options list
// Depending of the game, 4 or 6 RetroPad Buttons will be configurable (L, R, L2, R2, L3, R3)
void init_macro_core_options()
{
   const char * drvname = BurnDrvGetTextA(DRV_NAME);

   macro_core_options.clear(); 

   int nMaxRetroPadButtons = 10; // 10 = RetroPad max available buttons (A, B, X, Y, L, R, L2, R2, L3, R3)
   int nEffectiveFireButtons = nFireButtons;

   if (bStreetFighterLayout && nFireButtons == 8) // Some CPS2 games have fire buttons to control Volume Up and Down (but we will not use them)
      nEffectiveFireButtons = 6;

   unsigned i = nGameInpCount; // why nGameInpCount? cause macros begin just after normal inputs
   struct GameInp* pgi = GameInp + nGameInpCount;

   for(; i < (nGameInpCount + nMacroCount); i++, pgi++)
   {
      // Skip system macros
      if (pgi->Macro.nSysMacro)
      {
         continue;
      }

      // Assign an unique nCode for the macro
      if (!input_initialized)
         pgi->Macro.Switch.nCode = switch_ncode++;

      macro_core_options.push_back(macro_core_option());
      macro_core_option *macro_option = &macro_core_options.back();

      // Clean the macro name to creation the core option name (removing space and equal characters)
      std::vector<char> option_name(strlen(pgi->Macro.szName) + 1); // + 1 for the '\0' ending
      strcpy(option_name.data(), pgi->Macro.szName);
      str_char_replace(option_name.data(), ' ', '_');
      str_char_replace(option_name.data(), '=', '_');

      macro_option->pgi = pgi;
      strncpy(macro_option->friendly_name, pgi->Macro.szName, sizeof(macro_option->friendly_name));
      snprintf(macro_option->option_name, sizeof(macro_option->option_name), "fba-macro-%s-%s", drvname, option_name.data());

      // Reserve space for the default value
      int remaining_input_available = nMaxRetroPadButtons - nEffectiveFireButtons;

      macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_EMPTY, "None"));

      if (remaining_input_available >= 6)
      {
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_L, "RetroPad L Button"));
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_R, "RetroPad R Button"));
      }
      if (remaining_input_available >= 4)
      {
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_L2, "RetroPad L2 Button"));
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_R2, "RetroPad R2 Button"));
         
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_L3, "RetroPad L3 Button"));
         macro_option->values.push_back(macro_core_option_value(RETRO_DEVICE_ID_JOYPAD_R3, "RetroPad R3 Button"));
      }

      std::vector<macro_core_option_value, std::allocator<macro_core_option_value> >(macro_option->values).swap(macro_option->values);

      // Create the string values for the macro option
      macro_option->values_str.assign(macro_option->friendly_name);
      macro_option->values_str.append("; ");

      for (int macro_value_idx = 0; macro_value_idx < macro_option->values.size(); macro_value_idx++)
      {
         macro_option->values_str.append(macro_option->values[macro_value_idx].friendly_name);
         if (macro_value_idx != macro_option->values.size() - 1)
            macro_option->values_str.append("|");
      }
      std::basic_string<char>(macro_option->values_str).swap(macro_option->values_str);

#ifdef FBA_DEBUG
      log_cb(RETRO_LOG_INFO, "'%s' (%d)\n", macro_option->values_str.c_str(), macro_option->values.size() - 1); // -1 to exclude the None from the macro count
#endif
   }
}

// Initialize the macro input descriptors depending of the choice the user made in core options
// As soon as the user has choosen a RetroPad button for a macro, this macro will be added to the input descriptor and can be used as a regular input
// This means that the auto remapping of RetroArch will be possible also for macros  
void init_macro_input_descriptors()
{
   macro_input_descriptors.clear();

   for(unsigned i = 0; i < macro_core_options.size(); i++)
   {
      macro_core_option *macro_option = &macro_core_options[i];

      if (!macro_option->selected_value || macro_option->selected_value->retro_device_id == 255)
      	continue;

      unsigned port = 0;
      unsigned index = 0;
      unsigned id = macro_option->selected_value->retro_device_id;

      // "P1 XXX" - try to exclude the "P1 " from the macro name
      int offset_player_x = 0;
      if (strlen(macro_option->friendly_name) > 3 && macro_option->friendly_name[0] == 'P' && macro_option->friendly_name[2] == ' ')
      {
         port = (unsigned)(macro_option->friendly_name[1] - 49);
         offset_player_x = 3;
      }

      // set the port for the macro
      keybinds[macro_option->pgi->Macro.Switch.nCode][1] = port;

      char* description = macro_option->friendly_name + offset_player_x;

      retro_input_descriptor descriptor;
      descriptor.port = port;
      descriptor.device = RETRO_DEVICE_JOYPAD;
      descriptor.index = index;
      descriptor.id = id;
      descriptor.description = description;
      macro_input_descriptors.push_back(descriptor);

      log_cb(RETRO_LOG_INFO, "MACRO [%-15s] Macro.Switch.nCode: 0x%04x Macro.nMode: %d - assigned to key [%-25s] on port %2d.\n",
      	macro_option->friendly_name, macro_option->pgi->Macro.Switch.nCode, macro_option->pgi->Macro.nMode, print_label(id), port);
   }
}

// Set the input descriptors by combininng the two lists of 'Normal' and 'Macros' inputs
void set_input_descriptors()
{
   std::vector<retro_input_descriptor> input_descriptors(normal_input_descriptors.size() + macro_input_descriptors.size() + 1); // + 1 for the empty ending retro_input_descriptor { 0 }

   unsigned input_descriptor_idx = 0;

   for (unsigned i = 0; i < normal_input_descriptors.size(); i++, input_descriptor_idx++)
   {
      input_descriptors[input_descriptor_idx] = normal_input_descriptors[i];
   }

   for (unsigned i = 0; i < macro_input_descriptors.size(); i++, input_descriptor_idx++)
   {
      input_descriptors[input_descriptor_idx] = macro_input_descriptors[i];
   }

   input_descriptors[input_descriptor_idx].description = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors.data());
}

INT32 GameInpBlank(INT32 bDipSwitch)
{
	UINT32 i = 0;
	struct GameInp* pgi = NULL;

	// Reset all inputs to undefined (even dip switches, if bDipSwitch==1)
	if (GameInp == NULL) {
		return 1;
	}

	// Get the targets in the library for the Input Values
	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		struct BurnInputInfo bii;
		memset(&bii, 0, sizeof(bii));
		BurnDrvGetInputInfo(&bii, i);
		if (bDipSwitch == 0 && (bii.nType & BIT_GROUP_CONSTANT)) {		// Don't blank the dip switches
			continue;
		}

		memset(pgi, 0, sizeof(*pgi));									// Clear input

		pgi->nType = bii.nType;											// store input type
		pgi->Input.pVal = bii.pVal;										// store input pointer to value

		if (bii.nType & BIT_GROUP_CONSTANT) {							// Further initialisation for constants/DIPs
			pgi->nInput = GIT_CONSTANT;
			pgi->Input.Constant.nConst = *bii.pVal;
		}
	}

	for (i = 0; i < nMacroCount; i++, pgi++) {
		pgi->Macro.nMode = 0;
		if (pgi->nInput == GIT_MACRO_CUSTOM) {
			pgi->nInput = 0;
		}
	}

	return 0;
}

static void GameInpInitMacros()
{
	struct GameInp* pgi;
	struct BurnInputInfo bii;

	INT32 nPunchx3[4] = {0, 0, 0, 0};
	INT32 nPunchInputs[4][3];
	INT32 nKickx3[4] = {0, 0, 0, 0};
	INT32 nKickInputs[4][3];

	INT32 nNeogeoButtons[4][4];
	INT32 nPgmButtons[10][16];

	bStreetFighterLayout = false;
	bVolumeIsFireButton = false;
	nMacroCount = 0;

	nFireButtons = 0;

	memset(&nNeogeoButtons, 0, sizeof(nNeogeoButtons));
	memset(&nPgmButtons, 0, sizeof(nPgmButtons));

	for (UINT32 i = 0; i < nGameInpCount; i++) {
		bii.szName = NULL;
		BurnDrvGetInputInfo(&bii, i);
		if (bii.szName == NULL) {
			bii.szName = "";
		}

		bool bPlayerInInfo = (toupper(bii.szInfo[0]) == 'P' && bii.szInfo[1] >= '1' && bii.szInfo[1] <= '4'); // Because some of the older drivers don't use the standard input naming.
		bool bPlayerInName = (bii.szName[0] == 'P' && bii.szName[1] >= '1' && bii.szName[1] <= '4');

		if (bPlayerInInfo || bPlayerInName) {
			INT32 nPlayer = 0;

			if (bPlayerInName)
				nPlayer = bii.szName[1] - '1';
			if (bPlayerInInfo && nPlayer == 0)
				nPlayer = bii.szInfo[1] - '1';

			if (nPlayer == 0) {
				if (strncmp(" fire", bii.szInfo + 2, 5) == 0) {
					nFireButtons++;
				}
			}
			
			if ((strncmp("Volume", bii.szName, 6) == 0) && (strncmp(" fire", bii.szInfo + 2, 5) == 0)) {
				bVolumeIsFireButton = true;
			}
			if (_stricmp(" Weak Punch", bii.szName + 2) == 0) {
				nPunchx3[nPlayer] |= 1;
				nPunchInputs[nPlayer][0] = i;
			}
			if (_stricmp(" Medium Punch", bii.szName + 2) == 0) {
				nPunchx3[nPlayer] |= 2;
				nPunchInputs[nPlayer][1] = i;
			}
			if (_stricmp(" Strong Punch", bii.szName + 2) == 0) {
				nPunchx3[nPlayer] |= 4;
				nPunchInputs[nPlayer][2] = i;
			}
			if (_stricmp(" Weak Kick", bii.szName + 2) == 0) {
				nKickx3[nPlayer] |= 1;
				nKickInputs[nPlayer][0] = i;
			}
			if (_stricmp(" Medium Kick", bii.szName + 2) == 0) {
				nKickx3[nPlayer] |= 2;
				nKickInputs[nPlayer][1] = i;
			}
			if (_stricmp(" Strong Kick", bii.szName + 2) == 0) {
				nKickx3[nPlayer] |= 4;
				nKickInputs[nPlayer][2] = i;
			}
			
			if ((BurnDrvGetHardwareCode() & (HARDWARE_PUBLIC_MASK - HARDWARE_PREFIX_CARTRIDGE)) == HARDWARE_SNK_NEOGEO) {
				if (_stricmp(" Button A", bii.szName + 2) == 0) {
					nNeogeoButtons[nPlayer][0] = i;
				}
				if (_stricmp(" Button B", bii.szName + 2) == 0) {
					nNeogeoButtons[nPlayer][1] = i;
				}
				if (_stricmp(" Button C", bii.szName + 2) == 0) {
					nNeogeoButtons[nPlayer][2] = i;
				}
				if (_stricmp(" Button D", bii.szName + 2) == 0) {
					nNeogeoButtons[nPlayer][3] = i;
				}
			}

			//if ((BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_IGS_PGM) {
			{ // Use nPgmButtons for Autofire too -dink
				if ((_stricmp(" Button 1", bii.szName + 2) == 0) || (_stricmp(" fire 1", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][0] = i;
				}
				if ((_stricmp(" Button 2", bii.szName + 2) == 0) || (_stricmp(" fire 2", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][1] = i;
				}
				if ((_stricmp(" Button 3", bii.szName + 2) == 0) || (_stricmp(" fire 3", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][2] = i;
				}
				if ((_stricmp(" Button 4", bii.szName + 2) == 0) || (_stricmp(" fire 4", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][3] = i;
				}
				if ((_stricmp(" Button 5", bii.szName + 2) == 0) || (_stricmp(" fire 5", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][4] = i;
				}
				if ((_stricmp(" Button 6", bii.szName + 2) == 0) || (_stricmp(" fire 6", bii.szInfo + 2) == 0)) {
					nPgmButtons[nPlayer][5] = i;
				}
			}
		}
	}

	pgi = GameInp + nGameInpCount;
	
	{ // Autofire!!!
			for (INT32 nPlayer = 0; nPlayer < nMaxPlayers; nPlayer++) {
				for (INT32 i = 0; i < nFireButtons; i++) {
					pgi->nInput = GIT_MACRO_AUTO;
					pgi->nType = BIT_DIGITAL;
					pgi->Macro.nMode = 0;
					pgi->Macro.nSysMacro = 15; // 15 = Auto-Fire mode
					if ((BurnDrvGetHardwareCode() & (HARDWARE_PUBLIC_MASK - HARDWARE_PREFIX_CARTRIDGE)) == HARDWARE_SEGA_MEGADRIVE) {
						if (i < 3) {
							sprintf(pgi->Macro.szName, "P%d Auto-Fire Button %c", nPlayer+1, i+'A'); // A,B,C
						} else {
							sprintf(pgi->Macro.szName, "P%d Auto-Fire Button %c", nPlayer+1, i+'X'-3); // X,Y,Z
						}
					} else {
						sprintf(pgi->Macro.szName, "P%d Auto-Fire Button %d", nPlayer+1, i+1);
					}
					if ((BurnDrvGetHardwareCode() & (HARDWARE_PUBLIC_MASK - HARDWARE_PREFIX_CARTRIDGE)) == HARDWARE_SNK_NEOGEO) {
						BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][i]);
					} else {
						BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][i]);
					}
					pgi->Macro.pVal[0] = bii.pVal;
					pgi->Macro.nVal[0] = 1;
					nMacroCount++;
					pgi++;
				}
			}
	}

	for (INT32 nPlayer = 0; nPlayer < nMaxPlayers; nPlayer++) {
		if (nPunchx3[nPlayer] == 7) {		// Create a 3x punch macro
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;

			sprintf(pgi->Macro.szName, "P%i 3x Punch", nPlayer + 1);
			for (INT32 j = 0; j < 3; j++) {
				BurnDrvGetInputInfo(&bii, nPunchInputs[nPlayer][j]);
				pgi->Macro.pVal[j] = bii.pVal;
				pgi->Macro.nVal[j] = 1;
			}

			nMacroCount++;
			pgi++;
		}

		if (nKickx3[nPlayer] == 7) {		// Create a 3x kick macro
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;

			sprintf(pgi->Macro.szName, "P%i 3x Kick", nPlayer + 1);
			for (INT32 j = 0; j < 3; j++) {
				BurnDrvGetInputInfo(&bii, nKickInputs[nPlayer][j]);
				pgi->Macro.pVal[j] = bii.pVal;
				pgi->Macro.nVal[j] = 1;
			}

			nMacroCount++;
			pgi++;
		}

		if (nFireButtons == 4 && (BurnDrvGetHardwareCode() & (HARDWARE_PUBLIC_MASK - HARDWARE_PREFIX_CARTRIDGE)) == HARDWARE_SNK_NEOGEO) {
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons AB", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons AC", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons AD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons BC", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons BD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons CD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons ABC", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons ABD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons ACD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;

			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons BCD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;

			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons ABCD", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][2]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			BurnDrvGetInputInfo(&bii, nNeogeoButtons[nPlayer][3]);
			pgi->Macro.pVal[3] = bii.pVal;
			pgi->Macro.nVal[3] = 1;
			nMacroCount++;
			pgi++;
		}
		
		if (nFireButtons == 4 && (BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_IGS_PGM) {
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 12", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 13", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 14", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 23", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 24", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 34", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 123", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 124", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;
			
			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 134", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;

			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 234", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			nMacroCount++;
			pgi++;

			pgi->nInput = GIT_MACRO_AUTO;
			pgi->nType = BIT_DIGITAL;
			pgi->Macro.nMode = 0;
			sprintf(pgi->Macro.szName, "P%i Buttons 1234", nPlayer + 1);
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][0]);
			pgi->Macro.pVal[0] = bii.pVal;
			pgi->Macro.nVal[0] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][1]);
			pgi->Macro.pVal[1] = bii.pVal;
			pgi->Macro.nVal[1] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][2]);
			pgi->Macro.pVal[2] = bii.pVal;
			pgi->Macro.nVal[2] = 1;
			BurnDrvGetInputInfo(&bii, nPgmButtons[nPlayer][3]);
			pgi->Macro.pVal[3] = bii.pVal;
			pgi->Macro.nVal[3] = 1;
			nMacroCount++;
			pgi++;
		}
	}

	if ((nPunchx3[0] == 7) && (nKickx3[0] == 7)) {
		bStreetFighterLayout = true;
	}
	if (nFireButtons >= 5 && (BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_CAPCOM_CPS2 && !bVolumeIsFireButton) {
		bStreetFighterLayout = true;
	}
}

INT32 GameInpInit()
{
	INT32 nRet = 0;
	// Count the number of inputs
	nGameInpCount = 0;
	nMacroCount = 0;
	nMaxMacro = nMaxPlayers * 52;

	for (UINT32 i = 0; i < 0x1000; i++) {
		nRet = BurnDrvGetInputInfo(NULL,i);
		if (nRet) {														// end of input list
			nGameInpCount = i;
			break;
		}
	}

	// Allocate space for all the inputs
	INT32 nSize = (nGameInpCount + nMaxMacro) * sizeof(struct GameInp);
	GameInp = (struct GameInp*)malloc(nSize);
	if (GameInp == NULL) {
		return 1;
	}
	memset(GameInp, 0, nSize);

	GameInpBlank(1);

	InpDIPSWResetDIPs();

	GameInpInitMacros();

	return 0;
}

// Auto-configure any undefined inputs to defaults
INT32 GameInpDefault()
{
	struct GameInp* pgi;
	struct BurnInputInfo bii;
	UINT32 i;

	pgi_reset = NULL;
	pgi_diag = NULL;

	// Fill all inputs still undefined
	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		if (pgi->nInput) {											// Already defined - leave it alone
			continue;
		}

		// Get the extra info about the input
		bii.szInfo = NULL;
		BurnDrvGetInputInfo(&bii, i);
		if (bii.pVal == NULL) {
			continue;
		}
		if (bii.szInfo == NULL) {
			bii.szInfo = "";
		}

		// Dip switches - set to constant
		if (bii.nType & BIT_GROUP_CONSTANT) {
			pgi->nInput = GIT_CONSTANT;
			continue;
		}

		GameInpAutoOne(pgi, bii.szInfo, bii.szName);
	}

	// Fill in macros still undefined
	/*
	for (i = 0; i < nMacroCount; i++, pgi++) {
		if (pgi->nInput != GIT_MACRO_AUTO || pgi->Macro.nMode) {	// Already defined - leave it alone
			continue;
		}

		GameInpAutoOne(pgi, pgi->Macro.szName, pgi->Macro.szName);
	}
	*/

	return 0;
}

// Call this one when device type is changed
INT32 GameInpReassign()
{
	struct GameInp* pgi;
	struct BurnInputInfo bii;
	UINT32 i;

	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		BurnDrvGetInputInfo(&bii, i);
		GameInpAutoOne(pgi, bii.szInfo, bii.szName);
	}

	return 0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port < nMaxPlayers && fba_devices[port] != device)
	{
		fba_devices[port] = device;
		GameInpReassign();
		set_input_descriptors();
	}
}

char* DecorateGameName(UINT32 nBurnDrv)
{
	static char szDecoratedName[256];
	UINT32 nOldBurnDrv = nBurnDrvActive;

	nBurnDrvActive = nBurnDrv;

	const char* s1 = "";
	const char* s2 = "";
	const char* s3 = "";
	const char* s4 = "";
	const char* s5 = "";
	const char* s6 = "";
	const char* s7 = "";
	const char* s8 = "";
	const char* s9 = "";
	const char* s10 = "";
	const char* s11 = "";
	const char* s12 = "";
	const char* s13 = "";
	const char* s14 = "";

	s1 = BurnDrvGetTextA(DRV_FULLNAME);
	if ((BurnDrvGetFlags() & BDF_DEMO) || (BurnDrvGetFlags() & BDF_HACK) || (BurnDrvGetFlags() & BDF_HOMEBREW) || (BurnDrvGetFlags() & BDF_PROTOTYPE) || (BurnDrvGetFlags() & BDF_BOOTLEG) || (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0)) {
		s2 = " [";
		if (BurnDrvGetFlags() & BDF_DEMO) {
			s3 = "Demo";
			if ((BurnDrvGetFlags() & BDF_HACK) || (BurnDrvGetFlags() & BDF_HOMEBREW) || (BurnDrvGetFlags() & BDF_PROTOTYPE) || (BurnDrvGetFlags() & BDF_BOOTLEG) || (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0)) {
				s4 = ", ";
			}
		}
		if (BurnDrvGetFlags() & BDF_HACK) {
			s5 = "Hack";
			if ((BurnDrvGetFlags() & BDF_HOMEBREW) || (BurnDrvGetFlags() & BDF_PROTOTYPE) || (BurnDrvGetFlags() & BDF_BOOTLEG) || (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0)) {
				s6 = ", ";
			}
		}
		if (BurnDrvGetFlags() & BDF_HOMEBREW) {
			s7 = "Homebrew";
			if ((BurnDrvGetFlags() & BDF_PROTOTYPE) || (BurnDrvGetFlags() & BDF_BOOTLEG) || (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0)) {
				s8 = ", ";
			}
		}
		if (BurnDrvGetFlags() & BDF_PROTOTYPE) {
			s9 = "Prototype";
			if ((BurnDrvGetFlags() & BDF_BOOTLEG) || (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0)) {
				s10 = ", ";
			}
		}		
		if (BurnDrvGetFlags() & BDF_BOOTLEG) {
			s11 = "Bootleg";
			if (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0) {
				s12 = ", ";
			}
		}
		if (BurnDrvGetTextA(DRV_COMMENT) && strlen(BurnDrvGetTextA(DRV_COMMENT)) > 0) {
			s13 = BurnDrvGetTextA(DRV_COMMENT);
		}
		s14 = "]";
	}

	sprintf(szDecoratedName, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s", s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14);

	nBurnDrvActive = nOldBurnDrv;
	return szDecoratedName;
}
