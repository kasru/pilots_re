/*
 * Сохранения: автосохранение (PILOTS<слот>.<saveExt> — настройки и переменные,
 * переживающие перезапуск) и сохранённые игры (PILOTS<слот>.SAV — полное состояние
 * всех комнат эпизода).
 *
 * Начало файла: магия {'=VS=', версия формата}, настройки GM_SETTINGS, блоки переменных
 * «всегда» и «при перезапуске». В сохранённой игре далее — блоки «только в сохранении»
 * и состояния комнат: номер, число записей, карта тайлов, записи пула, блоки переменных
 * экземпляров.
 */
#include "gm.h"

/* Магия начала сохранения: байты «=VS=». */
#define GM_FILE_MAGIC 0x3D53563Du

/* Прочитать и проверить магию в начале открытого файла сохранения. */
static bool GmSave_ReadMagic(void)
{
    uint32_t magic[2];

    return Host_SaveRead(magic, sizeof(magic))
        && magic[0] == GM_FILE_MAGIC
        && magic[1] == GM_FILE_VERSION_CURRENT;
}

/*
 * Зашифровать или расшифровать (операция обратима) блок переменных: XOR с 32-байтовым
 * ключом, позиция в ключе = (смещение переменной + номер байта + 1) & 31.
 */
void GmSave_Xor(uint8_t *data, int size, int varOffset)
{
    do
    {
        varOffset++;
        *data++ ^= g_SaveXorKey[varOffset & 31];
    } while (--size != 0);
}

/*
 * Прочитать блоки переменных по описателю (с последнего блока к первому) из файла
 * сохранения в g_GlobalVars и расшифровать. NULL — читать нечего.
 */
bool GmSave_ReadVars(GM_VAR_DESC *desc)
{
    int index;

    if (desc == NULL)
        return true;

    index = desc->count;
    do
    {
        const GM_VAR_RANGE *range = &desc->ranges[--index];
        uint8_t *data = &g_GlobalVars[range->offset];

        if (!Host_SaveRead(data, range->size))
            return false;
        GmSave_Xor(data, (int)range->size, range->offset);
    } while (index != 0);
    return true;
}

/* Записать блоки переменных по описателю: зашифровать на месте, записать, расшифровать обратно. */
bool GmSave_WriteVars(GM_VAR_DESC *desc)
{
    int index;

    if (desc == NULL)
        return true;

    index = desc->count;
    do
    {
        const GM_VAR_RANGE *range = &desc->ranges[--index];
        uint8_t *data = &g_GlobalVars[range->offset];

        GmSave_Xor(data, (int)range->size, range->offset);
        if (!Host_SaveWrite(data, range->size))
            return false;
        GmSave_Xor(data, (int)range->size, range->offset);
    } while (index != 0);
    return true;
}

/* Обнулить блоки переменных по описателю. */
void GmSave_ClearVars(GM_VAR_DESC *desc)
{
    int index;

    if (desc == NULL)
        return;

    index = desc->count;
    do
    {
        const GM_VAR_RANGE *range = &desc->ranges[--index];

        GmMemZero(&g_GlobalVars[range->offset], range->size);
    } while (index != 0);
}

/* Прочитать блок настроек из файла сохранения в g_Settings. */
bool GmSave_ReadSettings(void)
{
    return Host_SaveRead(&g_Settings, sizeof(g_Settings));
}

/* Записать начало сохранения: магия, настройки, блоки переменных «всегда» и «при перезапуске». */
bool GmSave_WriteHeader(void)
{
    Host_SaveWrite(g_SaveMagic, sizeof(g_SaveMagic));
    Host_SaveWrite(&g_Settings, sizeof(g_Settings));
    GmSave_WriteVars(g_VarsKeepAlways);
    return GmSave_WriteVars(g_VarsKeepOnRestart);
}

/* Записать в сохранение одно двойное слово (номер комнаты, число записей). */
bool GmSave_WriteInt(uint32_t value)
{
    return Host_SaveWrite(&value, sizeof(value));
}

/*
 * Сдвинуть указатели кадров у сохранённых спрайтов на delta: при записи — в смещения
 * от начала арены (обратно — тут же после записи; при чтении смещения превращаются
 * в адреса в GmSave_ExpandPoolEntries). Экземпляры объектов не трогаются.
 */
void GmSave_RebaseSpriteFrames(GM_POOL_ENTRY *records, int count, int delta)
{
    for (; count != 0; count--, records++)
    {
        if ((records->flags & GMOBJ_INSTANCE) == 0)
            records->sprite.frame = (GM_FRAME *)((uintptr_t)records->sprite.frame + (uintptr_t)delta);
    }
}

/*
 * Запись пула в файле — 16 байт x86-раскладки: 12 байт скаляров и четвёртое двойное
 * слово (у спрайта смещение кадра от начала арены, у экземпляра слот переменных).
 * Рантайм-запись на LP64 шире (указатели по 8 байт), поэтому блоки состояния
 * сжимаются при записи и разворачиваются при чтении — в обоих случаях по рантайм-
 * указателю entry и отдельным байтам формата, а не сырым копированием структуры.
 * На x86 те же функции дают байт-в-байт прежний файл.
 */
static void GmSave_CompressPoolEntries(uint8_t *dst, const GM_POOL_ENTRY *entry, int count)
{
    for (; count != 0; count--, entry++)
    {
        uint8_t scalars[12];
        uint32_t slot = (entry->flags & GMOBJ_INSTANCE)
            ? (uint32_t)(uintptr_t)entry->object.vars
            : (uint32_t)(uintptr_t)entry->sprite.frame;

        memcpy(scalars, entry, sizeof(scalars));
        memcpy(dst, scalars, sizeof(scalars));
        memcpy(dst + 12, &slot, sizeof(slot));
        dst += 16;
    }
}

static void GmSave_ExpandPoolEntries(GM_POOL_ENTRY *entry, const uint8_t *src, int count, int arenaBase)
{
    for (; count != 0; count--, entry++)
    {
        uint8_t scalars[12];
        uint32_t slot;

        memcpy(scalars, src, sizeof(scalars));
        memcpy(&slot, src + 12, sizeof(slot));
        memset(entry, 0, sizeof(GM_POOL_ENTRY));
        memcpy(entry, scalars, sizeof(scalars));
        if ((entry->flags & GMOBJ_INSTANCE) == 0)
            entry->sprite.frame = (GM_FRAME *)(uintptr_t)(slot + (uint32_t)arenaBase);
        else
            entry->object.vars = (uint8_t *)(uintptr_t)slot;
        src += 16;
    }
}

/*
 * Автосохранение при загрузке эпизода. При первом запуске: проверить магию, прочитать
 * настройки (громкости — текущие целевые), запомнить позицию блоков переменных,
 * прочитать блоки «всегда» и «при перезапуске» и обнулить последние; повреждённый файл
 * обрезается и пишется заново. При повторной загрузке эпизода блоки перечитываются
 * с запомненной позиции.
 */
bool GmAutosave_Load(int slot)
{
    if (!Host_OpenSave(g_GameFilePath, g_Settings.saveExt, slot))
        return false;

    if (!g_FirstRun)
    {
        Host_SaveSeek(g_SaveVarsOffset);
        GmSave_ReadVars(g_VarsKeepAlways);
        GmSave_ReadVars(g_VarsKeepOnRestart);
        Host_CloseSave();
        return true;
    }

    while (!GmSave_ReadMagic())
    {
        Host_SaveTruncate();
        Host_CloseSave();
        GmAutosave_Write(slot);
        if (!Host_OpenSave(g_GameFilePath, g_Settings.saveExt, slot))
            return false;
    }

    GmSave_ReadSettings();
    g_Settings.soundVolume = g_TargetSoundVolume;
    g_Settings.musicVolume = g_TargetMusicVolume;
    g_SaveVarsOffset = Host_SaveTell();
    GmSave_ReadVars(g_VarsKeepAlways);
    GmSave_ReadVars(g_VarsKeepOnRestart);
    GmSave_ClearVars(g_VarsKeepOnRestart);
    g_FirstRun = false;
    Host_CloseSave();
    return true;
}

/*
 * Записать автосохранение: при первом запуске — начало файла целиком (с roomsVisited = 0),
 * иначе только блоки переменных на запомненную позицию.
 */
bool GmAutosave_Write(int slot)
{
    if (!Host_OpenSave(g_GameFilePath, g_Settings.saveExt, slot))
        return false;

    if (g_FirstRun)
    {
        g_Settings.roomsVisited = 0;
        GmSave_WriteHeader();
    }
    else
    {
        Host_SaveSeek(g_SaveVarsOffset);
        GmSave_WriteVars(g_VarsKeepAlways);
        GmSave_WriteVars(g_VarsKeepOnRestart);
    }
    Host_CloseSave();
    return true;
}

/* Удалить файл сохранённой игры. */
bool GmSaveGame_Delete(int slot)
{
    return Host_DeleteSave(g_GameFilePath, g_SaveGameExt, slot);
}

/*
 * Загрузить сохранённую игру: проверить магию, прочитать настройки (включённость звука,
 * ввода и фильмов остаётся текущей), загрузить эпизод без запуска, прочитать блоки
 * переменных и состояния комнат, войти в комнату, восстановить частоту и счётчик кадров,
 * музыку и курсор и перезапустить такт. При успехе не возвращается; false — файл
 * не открылся или повреждён (тогда он обрезается).
 */
bool GmSaveGame_Load(int slot)
{
    bool soundOn;
    bool inputEnabled;
    bool moviesOn;
    int musicTrack;
    int cursorGraphic;
    uint32_t roomIndex;

    if (!Host_OpenSave(g_GameFilePath, g_SaveGameExt, slot))
        return false;
    if (!GmSave_ReadMagic())
    {
        Host_SaveTruncate();
        Host_CloseSave();
        return false;
    }

    soundOn = g_Settings.soundOn;
    inputEnabled = g_Settings.inputEnabled;
    moviesOn = g_Settings.moviesOn;
    GmSave_ReadSettings();
    g_Settings.soundOn = soundOn;
    g_Settings.inputEnabled = inputEnabled;
    g_Settings.moviesOn = moviesOn;
    g_Settings.soundVolume = g_TargetSoundVolume;
    g_Settings.musicVolume = 0;
    GmAudio_SetMusicVolume(0);
    musicTrack = g_Settings.currentMusic;
    cursorGraphic = g_Settings.cursorGraphic;

    g_FirstRun = true;
    g_LoadingSave = true;
    g_GameRunning = false;
    GmLoad_Run(g_Settings.startEntry);
    GmSave_ReadVars(g_VarsKeepAlways);
    GmSave_ReadVars(g_VarsKeepOnRestart);
    GmSave_ReadVars(g_VarsSaveGameOnly);

    roomIndex = 0;
    do
    {
        int32_t value;
        GM_ROOM *room;
        uint32_t stateSize;
        GM_POOL_ENTRY *entry;
        int count;

        Host_SaveRead(&value, sizeof(value));
        room = &g_Rooms[value];
        Host_SaveRead(&value, sizeof(value));
        room->savedObjectCount = value;

        stateSize = sizeof(GM_POOL_ENTRY) * (uint32_t)value + sizeof(GM_TILE) * (uint32_t)g_MapTileCount;
        room->savedState = GmHeap_Alloc(stateSize);

        /* Блок в файле форматного размера (16 байт на запись): читаем в черновик
           и разворачиваем в рантайм-раскладку блока состояния. */
        {
            uint32_t tileBytes = sizeof(GM_TILE) * (uint32_t)g_MapTileCount;
            uint32_t fmtSize = 16u * (uint32_t)value + tileBytes;
            uint8_t *fmt = GmHeap_Alloc(fmtSize);

            Host_SaveRead(fmt, fmtSize);
            memcpy(room->savedState, fmt, tileBytes);
            entry = (GM_POOL_ENTRY *)((GM_TILE *)room->savedState + g_MapTileCount);
            GmSave_ExpandPoolEntries(entry, fmt + tileBytes, value, (int)(intptr_t)g_Arena.freePtr);
            GmHeap_Free(fmt);
        }
        for (count = value; count != 0; count--, entry++)
        {
            if ((entry->flags & GMOBJ_INSTANCE) && (entry->flags & GMOBJ_VARS_ALLOCATED))
            {
                /* Особенность оригинала: блок переменных экземпляра пишется и читается длиной
                   varsSizeMinus1 — последний байт переменных не сохраняется. */
                uint32_t varsSize = g_Classes[entry->object.classIndex].varsSizeMinus1;

                entry->object.vars = GmHeap_Alloc(varsSize);
                Host_SaveRead(entry->object.vars, varsSize);
            }
        }
    } while (++roomIndex < (uint32_t)g_RoomsLoaded);

    g_FirstRun = false;
    g_LoadingSave = false;
    GmRoom_Enter(g_Settings.currentRoom, false);
    Host_CloseSave();
    g_SysVars->frameRate = g_Settings.frameRate;
    g_SysVars->frameCount = (uint32_t)g_Settings.frameCount;
    if (musicTrack >= 0)
        Native_PlayMusic(musicTrack);
    g_Settings.musicVolume = 0;
    if (cursorGraphic >= 0)
        GmCursor_SetGraphic(cursorGraphic, false);
    g_GameRunning = true;
    GmGame_Restart();
}

/*
 * Сохранить игру: сохранить текущую комнату, записать настройки (частота и счётчик кадров,
 * эпизод, комната), блоки переменных и состояния всех загруженных комнат (указатели кадров —
 * смещениями от начала арены), затем вернуться в комнату.
 * Особенность оригинала: файл открывается без усечения, и короткое сохранение поверх
 * длинного оставляет хвост старых данных.
 */
bool GmSaveGame_Save(int slot)
{
    uint32_t roomIndex;

    if (!Host_OpenSave(g_GameFilePath, g_SaveGameExt, slot))
        return false;

    g_GameRunning = false;
    GmRoom_SaveState(g_CurrentRoom);
    g_Settings.frameRate = g_SysVars->frameRate;
    g_Settings.frameCount = (int)g_SysVars->frameCount;
    g_Settings.roomsVisited = g_RoomsLoaded;
    g_Settings.startEntry = g_StartEntry;
    g_Settings.currentRoom = g_CurrentRoom;
    GmSave_WriteHeader();
    GmSave_WriteVars(g_VarsSaveGameOnly);

    roomIndex = 0;
    do
    {
        GM_ROOM *room = &g_Rooms[roomIndex];

        if (room->loadBuffer != NULL)
        {
            int count = room->savedObjectCount;
            GM_POOL_ENTRY *entry = (GM_POOL_ENTRY *)((GM_TILE *)room->savedState + g_MapTileCount);
            int arenaBase = (int)(intptr_t)g_Arena.freePtr;

            GmSave_WriteInt(roomIndex);
            GmSave_WriteInt((uint32_t)count);
            GmSave_RebaseSpriteFrames(entry, count, -arenaBase);
            {
                /* Сжимаем рантайм-записи в формат (16 байт) в черновике: блок
                   состояния нужен в исходном виде ещё ниже по коду. */
                uint32_t tileBytes = sizeof(GM_TILE) * (uint32_t)g_MapTileCount;
                uint32_t fmtSize = 16u * (uint32_t)count + tileBytes;
                uint8_t *fmt = GmHeap_Alloc(fmtSize);

                memcpy(fmt, room->savedState, tileBytes);
                GmSave_CompressPoolEntries(fmt + tileBytes, entry, count);
                Host_SaveWrite(fmt, fmtSize);
                GmHeap_Free(fmt);
            }
            GmSave_RebaseSpriteFrames(entry, count, arenaBase);

            for (; count != 0; count--, entry++)
            {
                if ((entry->flags & GMOBJ_INSTANCE) && (entry->flags & GMOBJ_VARS_ALLOCATED))
                    Host_SaveWrite(entry->object.vars, g_Classes[entry->object.classIndex].varsSizeMinus1);
            }
        }
    } while (++roomIndex < (uint32_t)g_RoomCount);

    Host_CloseSave();
    GmRoom_Enter(g_CurrentRoom, true);
    g_GameRunning = true;
    return true;
}
