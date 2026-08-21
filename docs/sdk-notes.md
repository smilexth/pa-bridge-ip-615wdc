# The CTS SDK, decoded

> **Provenance.** This SDK is BUENA / SPA-Audio's, downloaded from their public *IP-Series
> System SDK Package* (`buenapa.com` → Download; file
> `static/upload/other/20250818/1755512660485720.zip`, SHA-256 `b8598ca1e430…20adf5a3`,
> retrieved 2026-08-19). It ships with no licence of any kind, so it is **not redistributed
> here** — `scripts/fetch-sdk.sh` fetches it from the vendor. Everything below is what was
> needed to interoperate with hardware we own, not a copy of their material.

BUENA ships `cts_sdk.h` with Chinese comments in **GBK**, which is why it looks like mojibake
in every modern editor. Read it with:

```bash
iconv -f GBK -t UTF-8 vendor/sdk/Include/cts_sdk.h | less
```

`SDK/note.txt` in the vendor archive states the scope plainly: *"only applicable to our
company's product models starting with IP-"*.

## Which build to use

| Archive path | Target |
|---|---|
| `SDK Linux/SDK__Linux__X64__V308__New.tar.xz` | **x86-64** ← the one that links here |
| `SDK Linux/SDK__Linux__V308__New.tar.gz` | i386 |
| `linux SDK arm/SDK__Linux__V5086__ARM.tar.gz` | ARM 32-bit |
| `linux SDK arm/SDK__Linux__X64__V5086__gcc-arm-8.3.tar.xz` | ARM 32-bit despite the name |

The x86-64 build is **V308**, older than the ARM **V5086**. Everything this bridge needs is
present in V308; some newer entry points (`TSDK_GroupAddTermEx`, the NVR calls) are not, so
check `nm -D --defined-only libCtsSdk.so` before reaching for anything exotic.

## Conventions

- `CERR_SUCCESS = 0`; every failure is a **negative** `enSdkErrCode`. Useful ones:
  `-802 CERR_INVALID_PARAMETER`, `-916 CERR_GROUP_NOT_SETUP`, `-917 CERR_TERM_LIST_FULL`,
  `-710 CERR_DEV_NOT_REGISTERED`, `-910 CERR_LISTEN_PORT_ERROR`.
- Capacity: 6000 terminals, 300 broadcast groups, 300 terminals per group.
- The callback **must not block** — the header says so in capitals, and it is the only thread
  the SDK gives you for registrations, state changes and audio.

## The calls this bridge uses

```c
int TSDK_Init(ON_CTS_SDK_CALL_BACK cb, BOOL bServerMode, int usr_data);
int TSDK_DeInit(void);

int TSDK_SetupGroup   (int nBroadNum, TSdkGroupTermList *pTermList); // empty groups allowed
int TSDK_GroupAddTerm (int nBroadNum, TSdkGroupTermList *pTermList);

int TSDK_StartBroadVoice(int nBroadNum);                       // 8 kHz
int TSDK_BroadPcmVoice  (int nBroadNum, LPBYTE pcm, int len);  // len % 320 == 0
int TSDK_StopBroadVoice (int nBroadNum);
```

`bServerMode = TRUE` makes this process the server terminals register to — that is what lets
the bridge replace `TbsServer.exe` rather than sit beside it.

`TSdkGroupTermList` is a bare `DWORD TermList[MAX_BROAD_TERM]`; unused slots must be zero.

🔑 **`dwTermID` is `zone << 16 | device`.** The header says so in a comment and it is the single
most expensive detail in this SDK: the config tool displays "Term ID 1", the real ID is
`0x00010001` = 65537, and feeding it `1` returns `-802 CERR_INVALID_PARAMETER` every time —
which then makes `TSDK_StartBroadVoice()` fail with `-916 CERR_GROUP_NOT_SETUP`, so the symptom
you chase is a silent call rather than a bad ID. Take the value from
`CB_Event_TermRegister` / `CB_Event_TermConnect` and never type it.

The 16 kHz variants (`TSDK_StartBroadVoiceEx`, `TSDK_BroadPcmVoiceEx`, multiples of 640) exist
and would suit a wideband codec, but Asterisk's AudioSocket is 8 kHz `slin`, so the narrowband
pair is the exact match.

## Callback events worth handling

| Event | Meaning |
|---|---|
| `CB_Event_TermRegister` (301) | terminal wants to register — **return 0 to accept**, −1 to refuse |
| `CB_Event_TermConnect` (302) | terminal connected — return 0 to allow; this is when it becomes groupable |
| `CB_Event_TermCnnLost` (303) | connection dropped |
| `CB_Post_TermState` (202) | online / playing MP3 / talking (`enSdkDevState`) |
| `CB_Post_Mp3PlayFinish` (204) | file playback finished |
| `CB_Post_TermSos` (201) | terminal pressed its call button |

## Volume: use the coarse scale

```c
int TSDK_Req_GetAudLevel  (DWORD dwTermID);                        // answer via CB_Asw_GetAudLevel
int TSDK_Req_SetAudLevel  (DWORD dwTermID, TSdkAudioLevel   *lvl); // eAmpLevel 0..10  <- use this
int TSDK_Req_SetAudLevelEx(DWORD dwTermID, TSdkAudioLevelEx *lvl); // nAmpValue 0..100 <- broken
```

⚠️ On TM35 / BIOS 5.7.9 the `Ex` call returns `CERR_SUCCESS` and then derives a nonsense coarse
level: value 100 → level 10 (deafening), and 60, 40 and 30 all → level 1 (inaudible). Nothing
in between exists. `TSDK_Req_SetAudLevel()` with `eAmpLevel` 0–10 sets the level directly and
behaves; the firmware then reports sensible readings back (level 6 ⇒ internal value 85, level 4
⇒ 81). Both structs also carry mic-capture levels, irrelevant on a speaker with no microphone.

## Beyond paging, if it's ever wanted

`TSDK_StartBroadMp3` + `TSDK_BroadMp3File(group, path)` play a file to a group — cron plus this
replaces the vendor server's scheduling. `TSDK_Req_SetAudLevel` sets terminal volume (0–10).
`TSDK_Req_OpenTermTalk` / `TSDK_Send_TalkAudio` do two-way talk, on models that have a
microphone — the IP-615WDC does not.
