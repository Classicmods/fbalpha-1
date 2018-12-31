#ifndef __RETRO_CD_EMU__
#define __RETRO_CD_EMU__

#include <audio/audio_mixer.h>
extern audio_mixer_sound_t *cdsound;
extern audio_mixer_voice_t *cdvoice;

TCHAR* GetIsoPath();
INT32 CDEmuInit();
INT32 CDEmuExit();
INT32 CDEmuStop();
INT32 CDEmuPlay(UINT8 M, UINT8 S, UINT8 F);
INT32 CDEmuLoadSector(INT32 LBA, char* pBuffer);
UINT8* CDEmuReadTOC(INT32 track);
UINT8* CDEmuReadQChannel();
INT32 CDEmuGetSoundBuffer(INT16* buffer, INT32 samples);
void wav_exit();

extern CDEmuStatusValue CDEmuStatus;
extern TCHAR CDEmuImage[MAX_PATH];

#endif
