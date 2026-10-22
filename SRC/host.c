/*
 * Хост Windows: WinMain и главный цикл, главное окно и его оконная процедура,
 * палитра и вывод кадра через DIB-секцию, заставка AVI через MCI, файл данных игры
 * и файлы сохранений, справка WinHelp, запуск внешних программ.
 */
#include "gm.h"

#include <digitalv.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Особое значение g_Window.errorMessage: не текст ошибки, а признак «получен WM_QUIT»
   (ставит Host_PumpMessages во время фильма, главный цикл по нему выходит без сообщения). */
#define HOST_QUIT_MARK ((const char *)1)

/* Размер окна AVI по умолчанию, если в вызове не задан. */
#define HOST_AVI_DEFAULT_WIDTH  320
#define HOST_AVI_DEFAULT_HEIGHT 240

/* Длина путей к AVI и BMP заставки (буферы в Host_AviOpen). В оригинале — 72 байта,
   причём путь EXE копировался в буфер до проверки длины; здесь MAX_PATH. */
#define HOST_AVI_PATH_SIZE MAX_PATH

/* Запас памяти, который не отдаётся арене игры. */
#define HOST_ARENA_RESERVE 80000

/* Номер ресурса иконки окна. */
#define HOST_ICON_RESOURCE 100

/* Сигнатура BMP-файла «BM». */
#define HOST_BMP_SIGNATURE 0x4D42

/* ========================================================================== */
/* Сообщения об ошибках                                                       */
/* ========================================================================== */

/* Окно сообщения с иконкой ошибки и звуковым сигналом. */
int Host_MessageBox(LPCSTR caption, LPCSTR text)
{
    MessageBeep(MB_ICONHAND);
    return MessageBoxA(g_MainWindow, text, caption, MB_ICONHAND);
}

/* Окно ошибки с заголовком «Game Maker Player». */
int Host_ErrorBox(LPCSTR text)
{
    return Host_MessageBox(s_GameMakerPlayer, text);
}

/* Показать отложенное сообщение об ошибке (если есть) и сбросить его. */
void Host_ShowPendingError(void)
{
    if (g_Window.errorMessage)
    {
        Host_ErrorBox(g_Window.errorMessage);
        g_Window.errorMessage = NULL;
    }
}

/* Запомнить сообщение об ошибке, если другое ещё не запомнено. Всегда возвращает false. */
bool Host_SetError(const char *message)
{
    if (!g_Window.errorMessage)
        g_Window.errorMessage = message;
    return false;
}

/* ========================================================================== */
/* Окно, справка, завершение                                                  */
/* ========================================================================== */

/* Путь к справке: путь EXE с заменой расширения на .HLP. */
char *Host_SetHelpPath(const char *exePath)
{
    strcpy(g_Window.helpPath, exePath);
    *strrchr(g_Window.helpPath, '.') = '\0';
    return strcat(g_Window.helpPath, s_HelpExt);
}

/* Закрыть окно: закрыть справку и уничтожить главное окно. */
void Host_CloseWindow(void)
{
    if (g_MainWindow)
    {
        WinHelpA(g_MainWindow, g_Window.helpPath, HELP_QUIT, 0);
        DestroyWindow(g_MainWindow);
    }
}

/* Завершение хоста: завершить игру, закрыть файл данных, удалить палитру,
   DIB-секцию и её DC, освободить память арены. */
void Host_Shutdown(void)
{
    g_MainWindow = NULL;
    GmGame_Shutdown();
    Host_CloseDataFile();
    if (g_Window.hPalette)
    {
        DeleteObject(g_Window.hPalette);
        g_Window.hPalette = NULL;
    }
    if (g_hDibDC)
        DeleteDC(g_hDibDC);
    if (g_hDibSection)
        DeleteObject(g_hDibSection);
    if (g_Window.hArenaMem)
    {
        GlobalFree(g_Window.hArenaMem);
        g_Window.hArenaMem = NULL;
        g_Window.arenaMem = NULL;
    }
}

/* Курсор окна: стандартная стрелка, если игра просит системный курсор,
   иначе NULL (курсор рисует движок). */
HCURSOR Host_GetCursor(void)
{
    if (g_Window.header->systemCursor)
        return LoadCursorA(NULL, IDC_ARROW);
    return NULL;
}

/* Справка WinHelp: поиск по ключевому слову topic, а если topic — ровно «?», содержание. */
int Host_ShowHelp(const char *topic)
{
    const char *helpData = topic;
    UINT helpCommand = HELP_PARTIALKEY;

    if (topic[0] == '?' && topic[1] == '\0')
    {
        helpData = NULL;
        helpCommand = HELP_CONTENTS;
    }
    return WinHelpA(g_MainWindow, g_Window.helpPath, helpCommand, (ULONG_PTR)helpData);
}

/* Запустить программу (командная строка commandLine) и дождаться её завершения. */
bool Host_RunProgram(LPSTR commandLine)
{
    STARTUPINFOA startupInfo = g_StartupInfoTemplate;
    PROCESS_INFORMATION processInfo = g_ProcessInfoTemplate;

    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpReserved = NULL;
    startupInfo.lpReserved2 = NULL;
    startupInfo.cbReserved2 = 0;
    startupInfo.lpDesktop = NULL;
    startupInfo.dwFlags = 0;
    /* Особенность оригинала: описатели процесса и потока не закрываются. */
    return CreateProcessA(NULL, commandLine, NULL, NULL, TRUE, NORMAL_PRIORITY_CLASS,
                          NULL, NULL, &startupInfo, &processInfo)
        && WaitForSingleObject(processInfo.hProcess, INFINITE) != WAIT_FAILED;
}

/* ========================================================================== */
/* Заставка AVI через MCI                                                     */
/* ========================================================================== */

/* Проверить код ошибки MCI; при ошибке показать её текст. Истина — ошибки нет. */
bool Host_MciCheck(MCIERROR error)
{
    char text[256];

    if (!error)
        return true;
    mciGetErrorStringA(error, text, sizeof(text));
    Host_ErrorBox(text);
    return false;
}

/* Послать команду MCI устройству AVI (если оно открыто). */
bool Host_MciSend(UINT command, DWORD flags, DWORD_PTR params)
{
    if (!g_AviDeviceId)
        return false;
    return Host_MciCheck(mciSendCommandA(g_AviDeviceId, command, flags, params));
}

/* Послать устройству AVI команду MCI с пустыми параметрами (MCI_STOP, MCI_CLOSE). */
bool Host_MciSendNoParams(UINT command, DWORD_PTR flags)
{
    MCI_GENERIC_PARMS params = { 0 };

    /* Особенность оригинала: если устройство не открыто, результат не определён
       (возвращается случайное значение регистра); вызывающие его не проверяют.
       Здесь в этом случае возвращается false. Поле dwCallback в оригинале — мусор,
       но оно не читается, так как MCI_NOTIFY не передаётся. */
    if (!g_AviDeviceId)
        return false;
    return Host_MciSend(command, (DWORD)flags, (DWORD_PTR)&params);
}

/* Начать воспроизведение AVI с уведомлением MM_MCINOTIFY главному окну. */
bool Host_AviPlay(void)
{
    MCI_PLAY_PARMS playParms = { 0 };

    /* Оригинал передаёт только младшее слово описателя окна (Windows узнаёт окно и по нему):
       32-разрядный mciavi32 при нераспознанном слове откатывается на родителя окна AVI, и
       уведомление доходит. На LP64 mciavi32 обращается по dwCallback целиком — обрезанное
       слово не называет окно, MM_MCINOTIFY никогда не приходит и игра висит на последнем
       кадре. Там передаём весь описатель. */
#if defined(_WIN64)
    playParms.dwCallback = (DWORD_PTR)g_MainWindow;
#else
    playParms.dwCallback = (uint16_t)(uintptr_t)g_MainWindow;
#endif
    return Host_MciSend(MCI_PLAY, MCI_NOTIFY, (DWORD_PTR)&playParms);
}

/* Общая часть WM_PALETTECHANGED/WM_QUERYNEWPALETTE окна во время AVI: передать
   сообщение окну воспроизведения и реализовать свою палитру. */
static LRESULT Host_AviPaletteMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_Window.hPalette)
        return 0;
    if (msg == WM_QUERYNEWPALETTE || (HWND)wParam != g_hAviWindow)
        g_AviPaletteReady = false;
    SendMessageA(g_hAviWindow, msg, wParam, lParam);
    if (g_IsWin3x || msg == WM_PALETTECHANGED)
        g_AviPaletteReady = true;
    SelectPalette(g_Window.hdc, g_Window.hPalette, msg == WM_PALETTECHANGED);
    if (!RealizePalette(g_Window.hdc))
        return 0;
    InvalidateRect(hWnd, NULL, FALSE);
    return 1;
}

/* Отпускание кнопки мыши, которое надо пропустить (WM_LBUTTONUP/WM_RBUTTONUP), или 0.
   Отступление от оригинала: щелчок, остановивший AVI, нажимается над анимацией, а
   отпускается уже над экраном игры — в оригинале это отпускание доходило до игры
   как самостоятельный щелчок (например, по флажку уровня под анимацией). */
static UINT s_swallowButtonUp;

/* Остановить AVI нажатием кнопки мыши buttonDown и пропустить её отпускание. */
static void Host_AviStopByMouse(UINT buttonDown)
{
    s_swallowButtonUp = buttonDown == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_RBUTTONUP;
    Host_MciSendNoParams(MCI_STOP, 0);
}

/* Оконная процедура главного окна на время AVI: размеры и палитра, перерисовка
   (при первой — окно AVI ставится на место), щелчок мыши, пробел, Enter и WM_CLOSE
   останавливают воспроизведение, MM_MCINOTIFY — конец. */
LRESULT CALLBACK Host_AviWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT paint;
    MINMAXINFO *minMaxInfo;

    switch (msg)
    {
    case WM_WINDOWPOSCHANGED:
        InvalidateRect(hWnd, NULL, FALSE);
        return DefWindowProcA(hWnd, msg, wParam, lParam);

    case WM_GETMINMAXINFO:
        minMaxInfo = (MINMAXINFO *)lParam;
        minMaxInfo->ptMaxTrackSize.x = g_Window.frameExtraWidth + g_Window.viewWidth;
        minMaxInfo->ptMaxSize.x = g_Window.frameExtraWidth + g_Window.viewWidth;
        minMaxInfo->ptMaxTrackSize.y = g_Window.frameExtraHeight + g_Window.viewHeight;
        minMaxInfo->ptMaxSize.y = g_Window.frameExtraHeight + g_Window.viewHeight;
        return 0;

    case WM_SIZE:
        g_Window.clientWidth = LOWORD(lParam);
        g_Window.clientHeight = HIWORD(lParam);
        return 0;

    case WM_PARENTNOTIFY:
        /* Щелчок в дочернем окне воспроизведения: mciavi32 гонит его через
           DefWindowProc, и WM_PARENTNOTIFY доходит до родителя - измерено
           харнессом tests/windows/clickskip.ps1 (щелчок реальным курсором по
           центру AVIWnd32 прерывает ролик за секунды против его ~35 с, и
           ветка ниже в журнале появляется раньше PlayAvi: end). Подкласс
           дочернего окна не нужен - вот эта ветка и есть цепочка пропуска. */
        if (LOWORD(wParam) == WM_LBUTTONDOWN || LOWORD(wParam) == WM_RBUTTONDOWN)
        {
            DBG_LOG("PlayAvi: click on the AVI child (WM_PARENTNOTIFY wp=%X) -> stop",
                    (unsigned)wParam);
            Host_AviStopByMouse(LOWORD(wParam));
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        DBG_LOG("PlayAvi: click on the game window -> stop");
        Host_AviStopByMouse(msg);
        return 0;

    case WM_CHAR:
        if (wParam == VK_SPACE || wParam == VK_RETURN)
            Host_MciSendNoParams(MCI_STOP, 0);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            return 0;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);

    case WM_PALETTECHANGED:
        if ((HWND)wParam == hWnd)
            return 0;
        return Host_AviPaletteMessage(hWnd, msg, wParam, lParam);

    case WM_QUERYNEWPALETTE:
        return Host_AviPaletteMessage(hWnd, msg, wParam, lParam);

    case WM_PAINT:
        BeginPaint(hWnd, &paint);
        if (g_AviPaletteReady)
        {
            if (!g_AviWindowPlaced)
            {
                int avX = g_AviX;
                int avY = g_AviY;
                g_AviWindowPlaced = true;
                /* Px игры -> px окна: тождественность в обычном окне, в
                   fullscreen MCI-ребёнок должен сесть в rect картинки. */
                SdlBackendGameToClient(&avX, &avY);
                MoveWindow(g_hAviWindow, avX, avY,
                           g_AviWidth ? g_AviWidth : HOST_AVI_DEFAULT_WIDTH,
                           g_AviHeight ? g_AviHeight : HOST_AVI_DEFAULT_HEIGHT, TRUE);
            }
            GmFrame_Present(paint.rcPaint.left, paint.rcPaint.top,
                            paint.rcPaint.right, paint.rcPaint.bottom, g_AviDissolve);
            g_AviDissolve = 0;
        }
        EndPaint(hWnd, &paint);
        return 0;

    case MM_MCINOTIFY:
        g_AviPlaying = false;
        return 0;

    case WM_CLOSE:
        g_AviQuitRequested = true;
        Host_MciSendNoParams(MCI_STOP, 0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

/* Полноэкранный режим (и выход из него) двигает rect картинки, а MCI-ребёнок —
   реальное окно, поставленное в игровых координатах (g_AviX/g_AviY) одним разом
   при первом WM_PAINT: без этого переставления фильм во время переключения
   остаётся на старых клиентских px — в углу экрана, пока фон (BMP в кадровом
   буфере) уже отцентрован по новой раскладке. Замерено на Windows: Alt+Enter
   во время start.AVI. Перенос v0.1 VideoRealignMciChild (video.c) — там вызов
   висел в SdlUpdateLayout на каждом изменении rect и потерялся при переезде
   MCI в host.c. На не-Windows ребёнка нет вовсе — кадр рисует сам порт
   (SdlBackendVideoFrame) и следует за раскладкой на каждый кадр. */
void Host_RealignAviWindow(void)
{
#ifdef _WIN32
    int avX = g_AviX;
    int avY = g_AviY;
    int avW = g_AviWidth ? g_AviWidth : HOST_AVI_DEFAULT_WIDTH;
    int avH = g_AviHeight ? g_AviHeight : HOST_AVI_DEFAULT_HEIGHT;

    if (!g_hAviWindow || !g_MainWindow || !g_AviWindowPlaced)
        return;                     /* до первого WM_PAINT место поставит сама раскладка */
    /* Px игры -> px окна: та же конверсия, что при первой постановке в WM_PAINT. */
    SdlBackendGameToClient(&avX, &avY);
    DBG_LOG("PlayAvi: realign child -> (%d,%d) %dx%d", avX, avY, avW, avH);
    MoveWindow(g_hAviWindow, avX, avY, avW, avH, TRUE);
#endif
}

/* Взять из таблицы типов дисков первый диск типа driveType: вычеркнуть его
   из таблицы и записать в rootOut «X:». Ложь — таких дисков больше нет. */
bool Host_TakeDriveOfType(char *rootOut, uint8_t *driveTypes, uint8_t driveType)
{
    for (int drive = 0; drive < 32; drive++)
    {
        if (driveTypes[drive] == driveType)
        {
            driveTypes[drive] = 0;
            rootOut[0] = (char)('A' + drive);
            strcpy(rootOut + 1, ":");
            return true;
        }
    }
    return false;
}

/* Следующий диск для поиска AVI: сначала жёсткие, затем CD, сетевые, RAM-диски. */
bool Host_NextSearchDrive(char *rootOut, uint8_t *driveTypes)
{
    return Host_TakeDriveOfType(rootOut, driveTypes, DRIVE_FIXED)
        || Host_TakeDriveOfType(rootOut, driveTypes, DRIVE_CDROM)
        || Host_TakeDriveOfType(rootOut, driveTypes, DRIVE_REMOTE)
        || Host_TakeDriveOfType(rootOut, driveTypes, DRIVE_RAMDISK);
}

/* Найти AVI name (относительный путь, без расширения) и его BMP-фон: сначала
   в каталоге EXE, затем в корнях дисков по порядку типов. Истина — AVI найден. */
bool Host_FindAviOnDrives(char *aviPath, char *bmpPath, const char *name)
{
    uint8_t driveTypes[32];
    DWORD driveMask = GetLogicalDrives();
    size_t dirLength;
    size_t nameLength;

    for (int drive = 0; drive < 32; drive++)
    {
        driveTypes[drive] = 0;
        if ((1u << drive) & driveMask)
        {
            g_DriveRootPath[0] = (char)('A' + drive);
            driveTypes[drive] = (uint8_t)GetDriveTypeA(g_DriveRootPath);
        }
    }

    strcpy(aviPath, g_ExecutablePath);
    *strrchr(aviPath, '\\') = '\0';
    dirLength = strlen(aviPath);
    nameLength = strlen(name);
    do
    {
        /* «\» + имя + «.AVI» + нуль должны поместиться в буфер. Особенность
           оригинала: если каталог EXE слишком длинный, dirLength не сбрасывается
           в 2 и корни дисков тоже не проверяются. */
        if (dirLength + 1 + nameLength + 4 < HOST_AVI_PATH_SIZE)
        {
            strcat(aviPath, s_Backslash);
            strcat(aviPath, name);
            strcpy(bmpPath, aviPath);
            strcat(aviPath, s_AviExt);
            strcat(bmpPath, s_BmpExt);
            if (GetFileAttributesA(aviPath) != INVALID_FILE_ATTRIBUTES)
                return true;
            dirLength = 2; /* дальше перебираются корни дисков «X:» */
        }
    } while (Host_NextSearchDrive(aviPath, driveTypes));
    return false;
}

/* Пути к AVI и BMP рядом с EXE (берётся только имя файла из name);
   истина, если AVI существует. */
bool Host_FindAviNextToExe(char *aviPath, char *bmpPath, const char *name)
{
    char basePath[MAX_PATH];

    strcpy(basePath, g_ExecutablePath);
    strrchr(basePath, '\\')[1] = '\0';
    strcat(basePath, strrchr(name, '\\') + 1);
    strcpy(aviPath, basePath);
    strcat(aviPath, s_AviExt);
    strcpy(bmpPath, basePath);
    strcat(bmpPath, s_BmpExt);
    return GetFileAttributesA(aviPath) != INVALID_FILE_ATTRIBUTES;
}

/* Загрузить BMP-фон заставки прямо в кадровый буфер: файл должен быть 8-разрядным,
   без сжатия, размером с экран игры, с согласованными размерами и смещениями.
   Палитра ставится из файла (с системными цветами). Возвращает результат _close
   или -1, если файла нет. */
int Host_LoadAviBackdrop(const char *bmpPath)
{
    BITMAPFILEHEADER fileHeader;
    BITMAPINFOHEADER infoHeader;
    RGBQUAD colors[256];
    uint8_t rgb[768];
    DWORD pixelsOffset;
    int fd;

    fd = _open(bmpPath, _O_RDONLY | _O_BINARY);
    if (fd == -1)
        return fd;

    _read(fd, &fileHeader, sizeof(fileHeader));
    _read(fd, &infoHeader, sizeof(infoHeader));
    if (!infoHeader.biClrUsed)
        infoHeader.biClrUsed = 256;
    /* Особенность оригинала: biClrUsed больше 256 не проверяется до чтения палитры. */
    _read(fd, colors, 4 * infoHeader.biClrUsed);
    pixelsOffset = 4 * infoHeader.biClrUsed + sizeof(fileHeader) + sizeof(infoHeader);

    if (fileHeader.bfType == HOST_BMP_SIGNATURE
        && pixelsOffset + g_Window.arena->frameBufferSize == fileHeader.bfSize
        && fileHeader.bfReserved1 == 0
        && fileHeader.bfReserved2 == 0
        && fileHeader.bfOffBits == pixelsOffset
        && infoHeader.biPlanes == 1
        && infoHeader.biBitCount == 8
        && infoHeader.biCompression == BI_RGB
        && g_Window.header->screenWidth == infoHeader.biWidth
        && g_Window.header->screenHeight == infoHeader.biHeight)
    {
        for (DWORD i = 0; i < infoHeader.biClrUsed; i++)
        {
            rgb[3 * i + 0] = colors[i].rgbRed;
            rgb[3 * i + 1] = colors[i].rgbGreen;
            rgb[3 * i + 2] = colors[i].rgbBlue;
        }
        Host_SetPalette(rgb, (int)infoHeader.biClrUsed, 0, true);
        _read(fd, g_Window.arena->frameBuffer, (unsigned int)g_Window.arena->frameBufferSize);
    }
    else
    {
        Host_ErrorBox(s_InvalidMovieBackdrop);
    }
    return _close(fd);
}

/* Открыть AVI заставки (если в заголовке игры режим заставки — AVI): найти файлы,
   открыть устройство MCI с дочерним окном воспроизведения, спрятать окно
   (нулевой размер в точке (g_AviX, g_AviY)) и загрузить фон.
   Истина — устройство открыто. */
bool Host_AviOpen(void)
{
    char aviPath[HOST_AVI_PATH_SIZE];
    char bmpPath[HOST_AVI_PATH_SIZE];
    MCI_ANIM_OPEN_PARMSA openParms = { 0 };
    MCI_STATUS_PARMS statusParms = { 0 };

    g_AviDissolve = g_Window.header->transition;
    if (g_Window.header->introMode != GMINTRO_AVI)
        return false;
    if (!Host_FindAviNextToExe(aviPath, bmpPath, g_AviBasePath)
        && !Host_FindAviOnDrives(aviPath, bmpPath, g_AviBasePath))
        return false;

    openParms.lpstrElementName = aviPath;
    openParms.dwStyle = WS_CHILD;
    openParms.hWndParent = g_MainWindow;
    if (!Host_MciCheck(mciSendCommandA(0, MCI_OPEN,
                                       MCI_OPEN_ELEMENT | MCI_ANIM_OPEN_WS | MCI_ANIM_OPEN_PARENT,
                                       (DWORD_PTR)&openParms)))
        return false;

    g_AviDeviceId = openParms.wDeviceID;
    statusParms.dwItem = MCI_DGV_STATUS_HWND;
    Host_MciSend(MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&statusParms);
    g_hAviWindow = (HWND)statusParms.dwReturn;
    {
        /* Нулевой размер (скрыто до первого WM_PAINT), но координаты уже в
           px окна — как и в Host_AviWndProc. */
        int avX = g_AviX;
        int avY = g_AviY;
        SdlBackendGameToClient(&avX, &avY);
        MoveWindow(g_hAviWindow, avX, avY, 0, 0, FALSE);
    }
    /* В оригинале возвращается младший байт результата Host_LoadAviBackdrop;
       вызывающий результат не проверяет и смотрит на g_AviDeviceId. */
    Host_LoadAviBackdrop(bmpPath);
    return true;
}

/* Выбрать накопившиеся сообщения, отбрасывая клавиатуру и мышь (после AVI). */
int Host_DrainMessages(void)
{
    MSG message;
    BOOL hasMessage;

    while ((hasMessage = PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) != FALSE)
    {
        if (message.message != WM_KEYDOWN
            && message.message != WM_LBUTTONDOWN
            && message.message != WM_RBUTTONDOWN
            && message.message != WM_LBUTTONUP
            && message.message != WM_RBUTTONUP
            && message.message != WM_MOUSEMOVE)
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    return hasMessage;
}

/* Проиграть AVI name (NULL — заставка «intro») из каталога фильмов в прямоугольнике
   (x, y, width, height); модальный цикл сообщений до конца воспроизведения.
   WM_CLOSE во время показа закрывает и игру. Ложь — AVI не найден или не открылся. */
bool Host_PlayAvi(const char *name, int x, int y, int width, int height)
{
    MSG msg;
    POINT cursorPos;
#pragma warning(suppress : 4996) /* GetVersion устарела, но нужна для распознавания Win32s */
    DWORD version = GetVersion();

    /* Старший разряд — не NT; основная версия меньше 4 — Win32s на Windows 3.x. */
    g_IsWin3x = (version & 0x80000000u) && (version & 0xFF) < 4;
    g_AviActive = true;
    g_AviDeviceId = 0;
    g_AviQuitRequested = false;
    g_AviPlaying = false;
    g_AviWindowPlaced = false;
    g_AviPaletteReady = false;
    g_AviX = x;
    g_AviY = y;
    g_AviWidth = width;
    g_AviHeight = height;

    strcpy(g_AviBasePath, g_Window.header->movieDir);
    strcat(g_AviBasePath, s_Backslash);
    strcat(g_AviBasePath, name ? name : s_DefaultIntroName);
    DBG_LOG("PlayAvi: start name=%s rect=%d,%d %dx%d", name ? name : s_DefaultIntroName, x, y, width, height);
    Host_AviOpen();
    if (!g_AviDeviceId)
    {
        g_AviActive = false;
        DBG_LOG("PlayAvi: open failed (no device)");
        return false;
    }
    DBG_LOG("PlayAvi: MCI device %d, avi window %p", (int)g_AviDeviceId, g_hAviWindow);

    if (Host_AviPlay())
    {
        if (name)
            InvalidateRect(g_MainWindow, NULL, FALSE);
        else
            ShowWindow(g_MainWindow, SW_SHOWNORMAL);
        if (!g_AviPaletteReady)
        {
            if (GetDeviceCaps(g_Window.hdc, RASTERCAPS) & RC_PALETTE)
                PostMessageA(g_MainWindow, WM_QUERYNEWPALETTE, 0, 0);
            else
                g_AviPaletteReady = true;
        }
        g_AviPlaying = true;
        /* Фича порта: mciavi32 рисует ребёнка 1:1 и не масштабируется вслед за
           картинкой, поэтому в fullscreen картинка падает до натурального
           размера (только центрирующее смещение) — фон и ребёнок остаются
           выровненными. */
        SdlBackendSetNativeMovie(1);
        do
        {
            GetMessageA(&msg, NULL, 0, 0);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } while (g_AviPlaying);
        SdlBackendSetNativeMovie(0);
    }

    Host_MciSendNoParams(MCI_CLOSE, 0);
    DestroyWindow(g_hAviWindow);
    g_hAviWindow = NULL;
    g_AviActive = false;
    if (g_AviQuitRequested)
    {
        PostMessageA(g_MainWindow, WM_CLOSE, 0, 0);
    }
    else
    {
        GmRoom_RepaintAll();
        Host_DrainMessages();
        /* Подвинуть курсор на место, чтобы окно получило WM_SETCURSOR/WM_MOUSEMOVE. */
        GetCursorPos(&cursorPos);
        SetCursorPos(cursorPos.x, cursorPos.y);
    }
    DBG_LOG("PlayAvi: end playing=%d quit=%d", (int)g_AviPlaying, (int)g_AviQuitRequested);
    return true;
}

/* ========================================================================== */
/* Главное окно                                                               */
/* ========================================================================== */

/* Нажата клавиша направления direction (0..7): запомнить её (с Shift — разряд 3)
   и точку щелчка — текущее положение мыши. */
static void Host_OnDirectionKey(uint8_t direction)
{
    g_Window.actionKey = direction;
    if (g_Window.actionKey <= GMDIR8_NW && GetAsyncKeyState(VK_SHIFT))
        g_Window.actionKey |= GMDIR8_SHIFT_HELD;
    g_Window.clickX = g_Window.mouseX;
    g_Window.clickY = g_Window.mouseY;
}

/* WM_KEYDOWN: восемь клавиш направлений и четыре особые клавиши из настроек игры. */
static void Host_OnKeyDown(WPARAM key)
{
    static const GM_EVENT specialEvents[4] = { GMEV_ACTION1, GMEV_ACTION2, GMEV_NEXT_FOCUS, GMEV_HELP };

    if (!(g_Window.settings->inputFlags & GMDEV_KEYBOARD))
        return;
    for (uint8_t dir = GMDIR8_N; dir <= GMDIR8_NW; dir++)
    {
        if (g_Window.settings->dirKeys[dir] == key)
        {
            Host_OnDirectionKey(dir);
            return;
        }
    }
    for (int i = 0; i < 4; i++)
    {
        if (g_Window.settings->specialKeys[i] == key)
        {
            g_Window.event = specialEvents[i];
            g_Window.lastChar = (uint8_t)key;
            return;
        }
    }
}

/* Кнопка мыши: событие event, точка щелчка из lParam. */
static void Host_OnMouseButton(GM_EVENT event, LPARAM lParam)
{
    g_Window.event = event;
    g_Window.clickX = LOWORD(lParam);
    g_Window.clickY = HIWORD(lParam);
}

/* Реализовать палитру игры в окне; при смене цветов перерисовать всё окно. */
static LRESULT Host_RealizePalette(void)
{
    if (g_Window.hPalette)
    {
        SelectPalette(g_Window.hdc, g_Window.hPalette, FALSE);
        if (RealizePalette(g_Window.hdc))
        {
            GmDirty_Add(0, 0, g_Window.clientWidth, g_Window.clientHeight);
            return 1;
        }
    }
    return 0;
}

/* Оконная процедура игры (во время AVI сообщения уходят в Host_AviWndProc).
   Перерисовка — через грязные прямоугольники; клавиатура и мышь — в поля g_Window,
   которые главный цикл передаёт в такт игры; MM_WOM_DONE — микширование звука. */
LRESULT CALLBACK Host_MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RECT updateRect;
    MINMAXINFO *minMaxInfo;

    if (g_AviActive)
        return Host_AviWndProc(hWnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_PAINT:
        if (GetUpdateRect(hWnd, &updateRect, FALSE))
        {
            GmDirty_Add(updateRect.left, updateRect.top, updateRect.right, updateRect.bottom);
            /* DefWindowProc проверяет область (BeginPaint/EndPaint). */
            return DefWindowProcA(hWnd, msg, wParam, lParam);
        }
        return 0;

    case WM_PALETTECHANGED:
        if ((HWND)wParam == hWnd)
            return 0;
        return Host_RealizePalette();

    case WM_QUERYNEWPALETTE:
        return Host_RealizePalette();

    case WM_CHAR:
        if (g_Window.settings->inputFlags & GMDEV_KEYBOARD)
            g_Window.lastChar = (uint8_t)wParam;
        return 0;

    case WM_KEYDOWN:
        Host_OnKeyDown(wParam);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        s_swallowButtonUp = 0;
        if (g_Window.settings->inputFlags & GMDEV_MOUSE)
            Host_OnMouseButton(GMEV_MOUSE_DOWN, lParam);
        return 0;

    case WM_LBUTTONUP:
        if (s_swallowButtonUp == msg)
        {
            s_swallowButtonUp = 0;
            return 0;
        }
        if (g_Window.settings->inputFlags & GMDEV_MOUSE)
        {
            g_Window.lastChar = g_Window.settings->specialKeys[0];
            Host_OnMouseButton(GMEV_ACTION1, lParam);
            DBG_LOG("click L: %d,%d", (int)LOWORD(lParam), (int)HIWORD(lParam));
        }
        return 0;

    case WM_RBUTTONUP:
        if (s_swallowButtonUp == msg)
        {
            s_swallowButtonUp = 0;
            return 0;
        }
        if (g_Window.settings->inputFlags & GMDEV_MOUSE)
        {
            g_Window.lastChar = g_Window.settings->specialKeys[1];
            Host_OnMouseButton(GMEV_ACTION2, lParam);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(Host_GetCursor());
            return 0;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);

    case WM_MOUSEMOVE:
        if (g_Window.settings->inputFlags & GMDEV_MOUSE)
        {
            g_Window.mouseX = LOWORD(lParam);
            g_Window.mouseY = HIWORD(lParam);
            GmCursor_OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_GETMINMAXINFO:
        minMaxInfo = (MINMAXINFO *)lParam;
        minMaxInfo->ptMaxTrackSize.x = g_Window.frameExtraWidth + g_Window.viewWidth;
        minMaxInfo->ptMaxSize.x = g_Window.frameExtraWidth + g_Window.viewWidth;
        minMaxInfo->ptMaxTrackSize.y = g_Window.frameExtraHeight + g_Window.viewHeight;
        minMaxInfo->ptMaxSize.y = g_Window.frameExtraHeight + g_Window.viewHeight;
        return 0;

    case WM_SIZE:
        g_Window.clientWidth = LOWORD(lParam);
        g_Window.clientHeight = HIWORD(lParam);
        return 0;

    case MM_WOM_DONE:
        GmAudio_Mix();
        return 0;

    case WM_CLOSE:
        Host_CloseWindow();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

/* Показать игру: проверить, что экран не меньше окна (иначе ошибка
   «Game REQUIRES larger screen.»), затем сыграть AVI-заставку или просто показать окно. */
bool Host_ShowWindowOrIntro(void)
{
    if (g_Window.screenWidth < g_Window.clientWidth || g_Window.screenHeight < g_Window.clientHeight)
    {
        Host_SetError(s_GameRequiresLargerScreen);
        return false;
    }

    Host_DrainMessages();
    if (!Host_PlayAvi(NULL, g_Window.header->introX, g_Window.header->introY,
                      g_Window.header->introWidth, g_Window.header->introHeight))
    {
        ShowWindow(g_MainWindow, g_Window.nShowCmd);
        UpdateWindow(g_MainWindow);
        SendMessageA(g_MainWindow, WM_QUERYNEWPALETTE, 0, 0);
    }
    return true;
}

/* Создать главное окно с клиентской областью размером с экран игры. Рамка изменяемая
   или тонкая (по заголовку игры); если окно не меньше экрана, рамка уводится за его край.
   Возвращает DC окна (он же g_Window.hdc). */
HDC Host_CreateWindow(GM_GAME_HEADER *header)
{
    DWORD windowStyle;

    g_Window.clientWidth = header->screenWidth;
    g_Window.viewWidth = g_Window.clientWidth;
    g_FrameBitmapInfo.bmiHeader.biWidth = g_Window.clientWidth;
    g_Window.clientHeight = header->screenHeight;
    g_Window.viewHeight = g_Window.clientHeight;
    g_FrameBitmapInfo.bmiHeader.biHeight = g_Window.clientHeight;
    g_Window.screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_Window.screenHeight = GetSystemMetrics(SM_CYSCREEN);

    /* Рамка изменяемая (WS_OVERLAPPEDWINDOW) или тонкая, без кнопки разворачивания.
       Стиль нужен только чтобы посчитать frameExtra для WM_GETMINMAXINFO; само окно
       создаёт SDL (флаг SDL_WINDOW_RESIZABLE). */
    windowStyle = header->resizable ? WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN
                                    : WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

    /* Размер рамки и заголовка. Отступление от оригинала: он складывал его из
       GetSystemMetrics (SM_CXFRAME, SM_CYCAPTION, SM_CYBORDER), что верно только для
       рамок Windows 95; на современных Windows клиентская область выходила меньше
       экрана игры. AdjustWindowRectEx даёт настоящую рамку. Окно верхнего уровня
       всегда получает заголовок, поэтому WS_CAPTION учитывается явно. */
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&frame, windowStyle | WS_CAPTION, FALSE, 0);
    g_Window.frameExtraWidth = frame.right - frame.left;
    g_Window.frameExtraHeight = frame.bottom - frame.top;

    /* Если окно не меньше экрана, клиентская область ужимается под экран. */
    if (header->resizable)
    {
        if (g_Window.screenWidth <= g_Window.clientWidth)
            g_Window.clientWidth = g_Window.screenWidth;
        if (g_Window.screenHeight <= g_Window.clientHeight)
            g_Window.clientHeight = g_Window.screenHeight;
    }

    g_MainWindow = SdlBackendCreateWindow(g_Window.clientWidth, g_Window.clientHeight,
                                          header->title, header->resizable);
    g_Window.hdc = GetDC(g_MainWindow);
    return g_Window.hdc;
}

/* ========================================================================== */
/* Видео: палитра, вывод кадра                                                */
/* ========================================================================== */

/* Подготовить видео и память арены: создать окно (если ещё нет). Под SDL
   DIB-секция не создаётся — кадровый буфер выделит модуль кадра из арены, а
   вывод идёт через палитруный LUT в текстуру SDL. Из арены вычитается запас,
   остаток выделяется и запирается. Истина — память арены получена. */
bool Host_InitVideo(GM_ARENA *arena, GM_GAME_HEADER *header, GM_SETTINGS *settings)
{
    g_Window.arena = arena;
    g_Window.header = header;
    g_Window.settings = settings;
    if (!g_MainWindow)
        Host_CreateWindow(header);

    /* Под SDL DIB-секция не создаётся: кадровый буфер выделит модуль кадра из арены
       (GmFrame_Init), а вывод идёт через палитруный LUT в текстуру SDL. */
    arena->frameBuffer = NULL;
    arena->freeBytes -= HOST_ARENA_RESERVE;
    if (!g_Window.arenaMem)
    {
        g_Window.hArenaMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)arena->freeBytes);
        g_Window.arenaMem = GlobalLock(g_Window.hArenaMem);
    }
    arena->freePtr = g_Window.arenaMem;
    /* wvsprintfA понимает только подмножество CRT-спецификаторов (без 'z'),
       поэтому старшие 32 бита считаются отдельно и только там, где указатель
       их вообще имеет (на x86 сдвиг на 32 — UB). */
    {
        unsigned arenaHigh = 0;
#if UINTPTR_MAX != 0xFFFFFFFFu
        arenaHigh = (unsigned)((uintptr_t)g_Window.arenaMem >> 32);
#endif
        DBG_LOG("Host_InitVideo: arena %u bytes at 0x%08X%08X (%s 2 ГБ)",
                (unsigned)arena->freeBytes,
                arenaHigh,
                (unsigned)(uintptr_t)g_Window.arenaMem,
                (uintptr_t)g_Window.arenaMem < 0x80000000u ? "ниже" : "ВЫШЕ");
        (void)arenaHigh;    /* DBG_LOG в release — no-op, а сдвиг старших 32 бит на x86 — UB */
    }
    return g_Window.arenaMem != NULL;
}

/* Частота кадров → задержка кадра 1000/частота мс (0 — без ожидания). */
bool Host_SetFrameRate(uint8_t frameRate)
{
    g_Window.frameDelayMs = 0;
    if (frameRate)
        g_Window.frameDelayMs = 1000 / frameRate;
    return true;
}

/* Логическая палитра на 256 цветов. */
typedef struct HOST_LOGPALETTE
{
    WORD palVersion;
    WORD palNumEntries;
    PALETTEENTRY palPalEntry[256];
} HOST_LOGPALETTE;

/* Поставить палитру из count цветов RGB. Перед сменой окно заливается чёрным
   (gridColor = 0) или расчерчивается сеткой 8×8 цвета g_TransitionGridColors[gridColor − 1]
   (стиль перехода). keepSystemColors — 10 первых и 10 последних цветов берутся системные.
   Палитра ставится и в DIB-секцию. Истина — палитра создана. */
bool Host_SetPalette(const uint8_t *rgb, int count, char gridColor, bool keepSystemColors)
{
    HOST_LOGPALETTE palette;

    DBG_LOG("SetPalette: count=%d grid=%d keepSystem=%d", count, (int)gridColor, (int)keepSystemColors);
    if (g_Window.clientWidth && g_Window.clientHeight)
    {
        /* Переход рисуется SDL-рендерером (GDI-отрисовка поверх окна SDL не видна). */
        if (gridColor)
            SdlBackendDrawGrid(g_TransitionGridColors[gridColor - 1]);
        else
            SdlBackendClearBlack();
    }

    palette.palVersion = 0x300;
    palette.palNumEntries = (WORD)count;
    for (int i = 0; i < count; i++)
    {
        palette.palPalEntry[i].peRed = rgb[3 * i + 0];
        palette.palPalEntry[i].peGreen = rgb[3 * i + 1];
        palette.palPalEntry[i].peBlue = rgb[3 * i + 2];
        palette.palPalEntry[i].peFlags = PC_NOCOLLAPSE;
    }
    if (keepSystemColors)
    {
        GetSystemPaletteEntries(g_Window.hdc, 0, 10, &palette.palPalEntry[0]);
        GetSystemPaletteEntries(g_Window.hdc, 246, 10, &palette.palPalEntry[246]);
    }
    if (g_Window.hPalette)
        DeleteObject(g_Window.hPalette);
    g_Window.hPalette = CreatePalette((const LOGPALETTE *)&palette);

    /* LUT SDL: палитра конвертирует кадр при каждом выводе. DIB-секции нет,
       поэтому SetDIBColorTable не нужен. */
    SdlBackendUpdatePalette(rgb, count);

    if (g_MainWindow)
        SendMessageA(g_MainWindow, WM_QUERYNEWPALETTE, 0, 0);
    return g_Window.hPalette != NULL;
}

/* Вывести прямоугольник кадра в окно: конверсия индексных битов (строки снизу
   вверх, высота кадра frameHeight) через палитруный LUT в теневой кадр SDL.
   Показ отдельно — Host_Flush. */
int Host_BlitRect(void *bits, int x, int y, int width, int height, int frameHeight)
{
    SdlBackendPresentRect(bits, frameHeight, g_FrameStride, x, y, width, height);
    return 1;
}

/* Вывести в окно прямоугольник кадра или (dissolve) весь кадр растворением:
   16 проходов квадратами 16×16 с шагом 64 по смещениям g_DissolvePatternX/Y.
   frameStride не используется. Под SDL растворение сводится к одному кадру:
   все тайлы конвертируются в теневой буфер, показ идёт один раз в Host_Flush. */
bool Host_Present(void *bits, int x, int y, int width, int height, int frameStride, int frameHeight, bool dissolve)
{
    (void)frameStride;

    if (g_Window.clientWidth && g_Window.clientHeight)
    {
        if (dissolve)
        {
            for (int pass = 0; pass < 16; pass++)
            {
                for (int tileX = g_DissolvePatternX[pass]; tileX < g_Window.clientWidth; tileX += 64)
                {
                    for (int tileY = g_DissolvePatternY[pass]; tileY < g_Window.clientHeight; tileY += 64)
                    {
                        Host_BlitRect(bits, tileX, tileY, 16, 16, frameHeight);
                    }
                }
            }
        }
        else
        {
            Host_BlitRect(bits, x, y, width, height, frameHeight);
        }
    }
    return true;
}

/* Показать накопленный кадр (SDL_RenderPresent). Если кадр не грязный — no-op. */
int Host_Flush(void)
{
    SdlBackendFlushPresent();
    return 1;
}

/* Выбрать сообщения во время фильма и вернуть событие такта; *now — время такта
   (0 на входе — сбросить событие перед выборкой). При WM_QUIT отметить выход
   и вернуть GMEV_ACTION2, чтобы фильм прервался. */
GM_EVENT Host_PumpMessages(DWORD *now)
{
    MSG message;
    GM_EVENT event;

    if (!*now)
        g_Window.event = GMEV_NONE;
    SdlBackendPumpEvents();
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            g_Window.errorMessage = HOST_QUIT_MARK;
            return GMEV_ACTION2;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    *now = timeGetTime();
    event = g_Window.event;
    g_Window.event = GMEV_NONE;
    return event;
}

/* ========================================================================== */
/* Файл данных игры                                                           */
/* ========================================================================== */

/* Открыть файл данных (только чтение, двоичный режим). Истина — описатель > 0. */
bool Host_OpenDataFile(const char *path)
{
    g_Window.dataFile = _open(path, _O_RDONLY | _O_BINARY);
    return g_Window.dataFile > 0;
}

/* Позиция в файле данных. */
int Host_DataTell(void)
{
    return _lseek(g_Window.dataFile, 0, SEEK_CUR);
}

/* Переход в файле данных (origin — SEEK_SET/SEEK_CUR/SEEK_END). */
bool Host_DataSeek(int offset, int origin)
{
    return _lseek(g_Window.dataFile, offset, origin) != -1;
}

/* Прочитать ровно size байт из файла данных. */
bool Host_DataRead(void *dst, uint32_t size)
{
    return _read(g_Window.dataFile, dst, size) == (int)size;
}

/* Прочитать до size байт из файла данных; возвращает прочитанное число байт. */
int Host_DataReadSome(void *dst, uint32_t size)
{
    return _read(g_Window.dataFile, dst, size);
}

/* Закрыть файл данных. */
int Host_CloseDataFile(void)
{
    int result = g_Window.dataFile;

    if (g_Window.dataFile > 0)
    {
        result = _close(g_Window.dataFile);
        g_Window.dataFile = -1;
    }
    return result;
}

/* Есть ли в хвосте файла path данные игры: тот же признак, что проверяет
   GmLoad_OpenGameFile, — последние 12 байт {размер индекса, версия, магия «=VS=»}. */
static bool Host_HasGameData(const char *path)
{
    GM_FILE_TRAILER trailer;
    bool found = false;
    int fd = _open(path, _O_RDONLY | _O_BINARY);

    if (fd == -1)
        return false;
    if (_lseek(fd, -(long)sizeof(trailer), SEEK_END) != -1
        && _read(fd, &trailer, sizeof(trailer)) == (int)sizeof(trailer))
    {
        found = trailer.formatVersion == GM_TRAILER_VERSION_CURRENT
             && memcmp(&trailer.magic, "=VS=", 4) == 0;
    }
    _close(fd);
    return found;
}

/* Путь к файлу игры. В оригинале данные игры дописаны в хвост самого PILOTS.EXE,
   и движок открывает собственный исполняемый файл. У пересобранного EXE хвоста
   с данными нет — тогда берётся PILOTS.EXE из того же каталога. От этого пути
   зависят также имена сохранений (PILOTS1.SAV) и справки (PILOTS.HLP), как в оригинале.
   path — путь своего EXE (буфер MAX_PATH), заменяется на месте. */
static void Host_ResolveGameFilePath(char *path)
{
    char *fileName;

    if (Host_HasGameData(path))
        return;
    fileName = strrchr(path, '\\');
    fileName = fileName ? fileName + 1 : path;
    *fileName = '\0';
    strncat(path, "PILOTS.EXE", MAX_PATH - 1 - strlen(path));
}

/* ========================================================================== */
/* Файлы сохранений                                                           */
/* ========================================================================== */

/* Имя сохранения: путь EXE в верхнем регистре без «.EXE» + номер слота + «.» +
   расширение (PILOTS1.SAV, PILOTS0.230). Возвращает длину строки. */
int Host_BuildSavePath(LPSTR out, const char *exePath, const char *ext, int slot)
{
    char basePath[MAX_PATH];

    strcpy(basePath, exePath);
    _strupr(basePath);
    *strstr(basePath, s_ExeExt) = '\0';
    return wsprintfA(out, s_SavePathFormat, basePath, slot, ext);
}

/* Запасной путь сохранения во временном каталоге (если каталог игры недоступен на запись). */
int Host_BuildTempSavePath(LPSTR out, const char *path)
{
    char tempDir[MAX_PATH];

    GetTempPathA(sizeof(tempDir), tempDir);
    /* Особенность оригинала: имя берётся вместе с «\», а GetTempPath уже
       заканчивается на «\», так что в пути получается «\\\» — Windows это допускает. */
    return wsprintfA(out, s_TempSavePathFormat, tempDir, strrchr(path, '\\'));
}

/* Открыть файл сохранения на чтение и запись (с созданием, без усечения);
   при неудаче — во временном каталоге. Истина — описатель > 0. */
bool Host_OpenSave(const char *exePath, const char *ext, int slot)
{
    char savePath[MAX_PATH];
    char tempPath[MAX_PATH];

    Host_BuildSavePath(savePath, exePath, ext, slot);
    g_Window.saveFile = _open(savePath, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (g_Window.saveFile <= 0)
    {
        Host_BuildTempSavePath(tempPath, savePath);
        g_Window.saveFile = _open(tempPath, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
    }
    return g_Window.saveFile > 0;
}

/* Удалить файл сохранения, а если его нет — запасную копию во временном каталоге. */
bool Host_DeleteSave(const char *exePath, const char *ext, int slot)
{
    char savePath[MAX_PATH];
    char tempPath[MAX_PATH];

    Host_BuildSavePath(savePath, exePath, ext, slot);
    if (remove(savePath) == 0)
        return true;
    Host_BuildTempSavePath(tempPath, savePath);
    return remove(tempPath) == 0;
}

/* Прочитать ровно size байт из файла сохранения. */
bool Host_SaveRead(void *dst, uint32_t size)
{
    return _read(g_Window.saveFile, dst, size) == (int)size;
}

/* Записать ровно size байт в файл сохранения. */
bool Host_SaveWrite(const void *src, uint32_t size)
{
    return _write(g_Window.saveFile, src, size) == (int)size;
}

/* Позиция в файле сохранения. */
int Host_SaveTell(void)
{
    return _lseek(g_Window.saveFile, 0, SEEK_CUR);
}

/* Перейти в файле сохранения на смещение offset от начала. */
bool Host_SaveSeek(int offset)
{
    return _lseek(g_Window.saveFile, offset, SEEK_SET) != -1;
}

/* Обрезать файл сохранения до нуля (при неверной магии). */
int Host_SaveTruncate(void)
{
    return _chsize(g_Window.saveFile, 0);
}

/* Закрыть файл сохранения. */
int Host_CloseSave(void)
{
    int result = g_Window.saveFile;

    if (g_Window.saveFile > 0)
    {
        result = _close(g_Window.saveFile);
        g_Window.saveFile = -1;
    }
    return result;
}

/* ========================================================================== */
/* WinMain                                                                    */
/* ========================================================================== */

/* Точка входа: запомнить путь файла игры, инициализировать игру и крутить главный
   цикл: выбрать сообщения; при ошибке показать её и закрыть окно; если окно не
   свёрнуто и прошла задержка кадра — один такт игры. Возврат — wParam последнего
   выбранного сообщения (WM_QUIT при обычном выходе). */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    char exePath[MAX_PATH];
    char modulePath[MAX_PATH];
    MSG message = { 0 };
    DWORD now;
    bool cursorInWindow;
    POINT cursorPos;

    (void)hPrevInstance;
    (void)lpCmdLine;

    DbgLogInit();
    DbgInstallExceptionHandler();
    DBG_ENTER("WinMain");

    if (!SdlBackendStartup())
    {
        Host_ErrorBox("SDL_Init failed");
        return 1;
    }
    /* Разбор командной строки ДО того, как движок открыл хоть один файл —
       именно это и делает -datapath работающим. */
    SdlBackendParseCmdLine(lpCmdLine);

    GetModuleFileNameA(hInstance, exePath, sizeof(exePath));
    strcpy(modulePath, exePath);        /* справка намеренно НЕ идёт за -datapath */
    if (SdlBackendDataPathGiven())
    {
        /* -datapath (фича порта): движок живёт в выбранном каталоге — путь
           данных строится от текущего каталога, буквально как в оригинале
           («.\PILOTS.EXE»); имена сохранений и поиск AVI выводятся из
           g_ExecutablePath, так что уходят за ключом тоже. Разрешение пути —
           за шимом (регистронезависимый обход, как у всех открытий). */
        strcpy(exePath, ".\\PILOTS.EXE");
    }
    else
    {
        /* Без ключа: пересобранный EXE без данных игры читает их из PILOTS.EXE
           рядом с собой (в оригинале данные дописаны в хвост собственного exe). */
        Host_ResolveGameFilePath(exePath);
    }

    g_hAppInstance = hInstance;
    g_Window.nShowCmd = nShowCmd;
    g_Window.hdc = NULL;
    g_Window.hPalette = NULL;
    g_Window.hArenaMem = NULL;
    g_Window.arenaMem = NULL;
    g_Window.errorMessage = NULL;
    g_Window.frameDelayMs = 0;
    g_hDibSection = NULL;
    g_DibBits = NULL;
    g_hDibDC = NULL;
    g_hAviWindow = NULL;
    g_MainWindow = NULL;
    g_Window.clickY = 0;
    g_Window.clickX = 0;
    g_Window.mouseY = 0;
    g_Window.mouseX = 0;
    g_Window.clientHeight = 0;
    g_Window.clientWidth = 0;
    g_Window.saveFile = -1;
    g_Window.dataFile = -1;
    /* Особенность оригинала: «пустое» значение GMEV_NONE (0x0E) пишется во все три поля ввода. */
    g_Window.lastChar = GMEV_NONE;
    g_Window.actionKey = GMEV_NONE;
    g_Window.event = GMEV_NONE;

    /* В оригинале буфер пути был 64 байта; здесь MAX_PATH (см. gm.h). */
    strcpy(g_ExecutablePath, exePath);
    if (!GmGame_Initialize(g_ExecutablePath))
    {
        Host_ShowPendingError();
        Host_CloseWindow();
        Host_Shutdown();
        SdlBackendShutdown();
        DbgLogClose();
        return 0;
    }
    DBG_LOG("GmGame_Initialize: ok");
    Host_SetHelpPath(modulePath);       /* .hlp рядом с exe, а не за -datapath (как в v0.1) */

    DBG_ENTER("GameMainLoop");
    for (;;)
    {
        SdlBackendPumpEvents();
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                goto shutdown;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (g_Window.errorMessage)
        {
            if (g_Window.errorMessage == HOST_QUIT_MARK)
                goto shutdown;
            Host_ShowPendingError();
            Host_CloseWindow();
            continue;
        }
        if (IsIconic(g_MainWindow))
            continue;
        now = timeGetTime();
        if (g_Window.frameDelayMs + g_LastFrameTime >= now)
            continue;

        cursorInWindow = true;
        g_LastFrameTime = now;
        if (g_Window.settings->inputFlags & GMDEV_MOUSE)
        {
            GetCursorPos(&cursorPos);
            ScreenToClient(g_MainWindow, &cursorPos);
            if (cursorPos.x < 0 || cursorPos.x >= g_Window.clientWidth
                || cursorPos.y < 0 || cursorPos.y >= g_Window.clientHeight)
                cursorInWindow = false;
        }
        if (!GmGame_Tick(g_Window.clientWidth, g_Window.clientHeight,
                         g_Window.mouseX, g_Window.mouseY,
                         g_Window.clickX, g_Window.clickY,
                         g_Window.event, g_Window.actionKey, g_Window.lastChar,
                         cursorInWindow))
            break;
        g_Window.lastChar = GMEV_NONE;
        g_Window.actionKey = GMEV_NONE;
        g_Window.event = GMEV_NONE;
    }
    /* Игра завершилась сама: закрыть окно. Особенность оригинала: возвращается
       wParam последнего выбранного сообщения, а не WM_QUIT. */
    Host_CloseWindow();

shutdown:
    Host_Shutdown();
    SdlBackendShutdown();
    DBG_EXIT("WinMain");
    DbgLogClose();
    return (int)message.wParam;
}
