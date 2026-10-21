/*
 * mci_ffmpeg.c - ffmpeg-backed mciSendCommandA for the Linux port: exactly the
 * MCI subset host.c uses (MCI_OPEN/MCI_STATUS/MCI_PLAY/MCI_STOP/MCI_CLOSE) to
 * play the AVI intro/start movies. On Windows the real winmm MCIAvi device does
 * this; here frames are decoded on the message-pump side and composited over
 * the game frame at the MCI child window's rect:
 *
 *  - WinShim_VideoPump() runs from PeekMessageA's SDL pump (WinShim_PumpSDLToQueue),
 *    i.e. while host.c waits in `do { GetMessageA; ... } while (g_AviPlaying)`.
 *    It paces frames against the wall clock (base time = first decoded frame,
 *    per-frame due time from stream pts, so no drift), keeps the last shown
 *    frame on screen until its display time ends, and at end-of-stream (or after
 *    MCI_STOP, which completes the notified MCI_PLAY like real MCIAvi) posts
 *    MM_MCINOTIFY to the MCI_PLAY callback window - that is what makes
 *    Host_AviWndProc clear g_AviPlaying and unblock the wait loop.
 *
 * host.c speaks the REAL MCI structs (MCI_ANIM_OPEN_PARMSA, MCI_STATUS_PARMS,
 * MCI_PLAY_PARMS - declared in win32_shim.h for this build), so the dispatch
 * below reads and writes those fields through the same types: no raw slot
 * arithmetic, one layout answer for everyone. The one Win9x habit kept is the
 * callback window: host.c stores only the low word of g_MainWindow into
 * MCI_PLAY_PARMS.dwCallback (handles were word-sized then), so
 * MciResolveCallbackHwnd resolves the truncated value back to the full handle.
 *
 * Empty TU on Windows (real winmm provides mciSendCommandA).
 */
/* ffmpeg headers BEFORE gm.h on purpose: the generated globals headers
   define object-like macros (offset, ...) that break libav* declarations, and
   lib headers must not see them. gm.h is still the one game header. */
#ifndef _WIN32
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
/* SDL before gm.h for the same reason as the libav* headers: the generated
   globals macros must not reach a library's declarations. SDL_GetTicks is the
   pacing clock below. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif

#include "gm.h"

#ifndef _WIN32

/* mmsystem MCI command ids used by host.c (real mciapi.h values) */
#define MCI_SHIM_OPEN    0x803
#define MCI_SHIM_CLOSE   0x804
#define MCI_SHIM_PLAY    0x806
#define MCI_SHIM_STOP    0x808
#define MCI_SHIM_STATUS  0x814   /* host.c asks it for MCI_DGV_STATUS_HWND (the child) */
#define MCI_SHIM_WINDOW  0x841   /* kept for completeness; host.c does not send it */

/* The movie's own sound goes into the SDL mixer (SdlMix_Movie*): the probe and
   the chunk walking below are compiled and exercised either way, only the sink
   is gated. Set to 0 to keep a movie silent without losing its video - on
   Windows this whole TU is empty (mciavi32 plays the sound itself). */
#define PILOTS_MOVIE_AUDIO 1

/* fake HWND for the MCI child window (MCI_STATUS/MCI_WINDOW output = g_hAviWindow);
   distinct from WinShimMainWindow() = 0x00123400 */
#define SHIM_MCI_CHILD_HWND ((HWND)(intptr_t)0x00123402)

typedef struct
{
  int          is_open;
  MCIDEVICEID  id;
  int          playing;
  int          notify_pending;
  HWND         notify_hwnd;
  HWND         child;              /* MCI_WINDOW output / g_VideoWindowHandle */
  int          win_x, win_y, win_w, win_h; /* MoveWindow recording; w/h 0 = natural size */
  /* decode pipeline */
  AVFormatContext *fmt;
  AVCodecContext  *dec;
  AVPacket        *pkt;
  AVFrame         *frame;
  struct SwsContext *sws;
  uint8_t         *buf[2];         /* ping-pong BGRA frames: one displayed, one decoded into */
  int             stride;          /* bytes per row (width * 4) */
  int             stream_idx;
  int             draining;        /* flush packet handed to the decoder */
  int             eof;             /* decoder drained, or a read/decode error */
  /* pacing */
  int             pend;            /* buf[1 - disp_idx] holds a decoded frame not yet shown */
  int             disp_idx;        /* which buf is on screen */
  int             disp_valid;      /* something has been shown */
  int64_t         frame_pts_us;    /* stream timestamp of the pending frame */
  int64_t         frame_due_us;    /* wall clock (SDL_GetTicks) time to show it */
  int64_t         frame_dur_us;
  int             timing_on;
  int             frames_shown;
  int64_t         base_wall_us;    /* wall clock of the first decoded frame */
  int64_t         base_pts_us;     /* its stream timestamp */
  int64_t         last_end_us;     /* when the last shown frame's display time ends */
  int64_t         last_pts_us;     /* fallback for frames without a timestamp */
  /* the movie's own sound: a second, independent read of the same file, see MciAudioProbe */
  FILE           *aud;             /* our own handle: ffmpeg never sees this stream */
  int             aud_ok;          /* a usable PCM stream was found in the header */
  int             aud_eof;         /* no audio chunk left in movi */
  unsigned int    aud_rate;        /* strf's nSamplesPerSec */
  unsigned int    aud_fcc;         /* the audio chunks' fourcc, '01wb' for stream 1 */
  unsigned long   aud_total;       /* bytes of PCM the file holds (summed while probing) */
  unsigned long   aud_fed;         /* bytes handed to the mixer since PLAY */
  unsigned long   movi_start;      /* first chunk inside the movi list */
  unsigned long   movi_end;        /* end of that list: no chunk of ours beyond it */
  unsigned long   chunk_left;      /* bytes of the current chunk still unread */
  unsigned long   first_off;       /* data offset of the first audio chunk, to rewind on PLAY */
  unsigned long   first_size;
} ShimMciState;

static ShimMciState s_Mci;
static MCIDEVICEID  s_NextDeviceId = 1;

static void MciLogAvError(const char *what, int err)
{
  char buf[128];

  av_strerror(err, buf, sizeof buf);
  DBG_LOG("MciFfmpeg: %s: %s", what, buf);
}

/* --- the movie's own sound ------------------------------------------------------------
 * The AVIs ship a second stream: hdrl/LIST strl with strh.fccType 'auds' and a 16-byte strf
 * WAVEFORMATEX (PCM, one channel, 8 bits - 22050 Hz in INTRO.AVI, 11025 in START and FINAL),
 * whose samples sit in the movi list as '01wb' chunks, named after the stream's index.
 *
 * ffmpeg's avi demuxer takes none of it: `ffprobe INTRO.AVI` prints "non-interleaved AVI" and
 * one stream, the video, so nothing above can be made to decode it. The game's own AVI reader
 * (ProcessVideoCommands, which draws the other movies) never had sound either. So the header is
 * walked by hand: one format record and a chunk cursor is all that is needed, and both are read
 * from a second FILE* so ffmpeg's own cursor is untouched.
 *
 * Only mono 8-bit PCM is accepted, which is what all three shipped movies are. Anything else
 * says so in the log and leaves the movie silent - the picture path does not care.
 */
static unsigned int RiffU32(const unsigned char *p)
{
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16)
       | ((unsigned int)p[3] << 24);
}

static unsigned int RiffU16(const unsigned char *p)
{
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Read a chunk header at an absolute offset. Leaves the file cursor wherever fread put it, so
   every caller positions itself before reading data. */
static int RiffPeek(FILE *f, unsigned long at, unsigned char *fourcc, unsigned long *size)
{
  unsigned char hdr[8];

  if ( fseek(f, (long)at, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8 )
    return 0;
  memcpy(fourcc, hdr, 4);
  *size = RiffU32(hdr + 4);
  return 1;
}

#define FCC_IS(p, s) (memcmp(p, s, 4) == 0)

/* Walk forward to the next chunk of the movie's own stream. Ours are front-loaded in these
   files (the writer put the whole sound before the frames), but a walk that only reads headers
   and seeks over the rest costs nothing even when the frames are megabytes. */
static int MciAudioNextChunk(ShimMciState *m)
{
  unsigned char fourcc[4];
  unsigned long size;
  unsigned long at = (unsigned long)ftell(m->aud);

  m->chunk_left = 0;
  while ( at + 8 <= m->movi_end )
  {
    if ( !RiffPeek(m->aud, at, fourcc, &size) )
      break;
    if ( RiffU32(fourcc) == m->aud_fcc )
    {
      if ( fseek(m->aud, (long)(at + 8), SEEK_SET) != 0 )
        break;
      m->chunk_left = size;
      return 1;
    }
    at += 8 + size + (size & 1);                /* chunks are word-aligned */
  }
  return 0;
}

/* The chunk fourcc of stream n's audio: two hex digits then 'wb', so '01wb' for the second
   stream - which is the shape all three shipped movies have. Read as a little-endian word, the
   same way RiffPeek compares against it. */
static unsigned int MciAudioFcc(unsigned int stream_no)
{
  static const char digits[] = "0123456789ABCDEF";
  unsigned char s[4];

  s[0] = (unsigned char)digits[(stream_no >> 4) & 15];
  s[1] = (unsigned char)digits[stream_no & 15];
  s[2] = 'w';
  s[3] = 'b';
  return RiffU32(s);
}

/* Find the 'auds' stream's format and the chunks that hold its samples. Called with the video
   pipeline already up, from MciOpenElement, with the same resolved path ffmpeg was given. */
static void MciAudioProbe(ShimMciState *m, const char *path)
{
  unsigned char fourcc[4];
  unsigned char list[4];
  unsigned char strh[4];
  unsigned char fmt[16];
  unsigned char top[12];
  unsigned long size, at, end, list_end, strl_at, strl_size, child_at, child_end, csize;
  unsigned long data_at, data_size, chunks = 0;
  unsigned int stream_no = 0;
  unsigned int aud_stream = 0;
  unsigned int tag, channels, bits;
  long file_len;

  m->aud = fopen(path, "rb");
  if ( !m->aud )
  {
    DBG_LOG("MciFfmpeg: movie sound: cannot re-open '%s' (%s)", path, strerror(errno));
    return;
  }
  if ( fread(top, 1, 12, m->aud) != 12 || !FCC_IS(top, "RIFF") || !FCC_IS(top + 8, "AVI ") )
  {
    DBG_LOG("MciFfmpeg: movie sound: not a RIFF AVI, skipping");
    fclose(m->aud);
    m->aud = 0;
    return;
  }
  fseek(m->aud, 0, SEEK_END);
  file_len = ftell(m->aud);
  end = RiffU32(top + 4) + 8;
  if ( file_len > 0 && (unsigned long)file_len < end )
    end = (unsigned long)file_len;               /* a truncated copy of the movie */

  at = 12;
  while ( at + 8 <= end && RiffPeek(m->aud, at, fourcc, &size) )
  {
    unsigned long top_size = size;               /* the walks below reuse size; this one steps on it */

    if ( !FCC_IS(fourcc, "LIST") )
    {
      at += 8 + top_size + (top_size & 1);
      continue;
    }
    if ( fseek(m->aud, (long)(at + 8), SEEK_SET) != 0 || fread(list, 1, 4, m->aud) != 4 )
      break;
    list_end = at + 8 + top_size;

    if ( FCC_IS(list, "hdrl") )
    {
      /* Streams are numbered by the order of their strl chunks and the chunk fourcc carries that
         number in hex, so '01wb' is the second stream's audio. The game's movies are video first
         and sound second, but the count is what decides, not the guess. */
      strl_at = at + 12;
      while ( strl_at + 8 <= list_end && RiffPeek(m->aud, strl_at, fourcc, &size) )
      {
        strl_size = size;
        if ( FCC_IS(fourcc, "LIST") )
        {
          int is_audio = 0;

          if ( fseek(m->aud, (long)(strl_at + 8), SEEK_SET) == 0
            && fread(list, 1, 4, m->aud) == 4 && FCC_IS(list, "strl") )
          {
            child_at = strl_at + 12;
            child_end = strl_at + 8 + strl_size;
            while ( child_at + 8 <= child_end && RiffPeek(m->aud, child_at, fourcc, &size) )
            {
              csize = size;
              if ( FCC_IS(fourcc, "strh") && csize >= 4 )
              {
                if ( fseek(m->aud, (long)(child_at + 8), SEEK_SET) != 0
                  || fread(strh, 1, 4, m->aud) != 4 )
                  break;
                is_audio = FCC_IS(strh, "auds");
                if ( is_audio )
                  aud_stream = stream_no;
              }
              else if ( FCC_IS(fourcc, "strf") && is_audio && csize >= 16 && !m->aud_ok )
              {
                if ( fseek(m->aud, (long)(child_at + 8), SEEK_SET) != 0
                  || fread(fmt, 1, 16, m->aud) != 16 )
                  break;
                tag = RiffU16(fmt);
                channels = RiffU16(fmt + 2);
                m->aud_rate = RiffU32(fmt + 4);
                bits = RiffU16(fmt + 14);
                if ( tag == 1 && channels == 1 && bits == 8 )
                {
                  m->aud_ok = 1;
                  m->aud_fcc = MciAudioFcc(aud_stream);
                }
                else
                {
                  DBG_LOG("MciFfmpeg: movie sound: tag=%u %u ch %u bit at %u Hz is not mono 8-bit PCM, the movie stays silent",
                          tag, channels, bits, m->aud_rate);
                  m->aud_rate = 0;
                }
              }
              child_at += 8 + csize + (csize & 1);
            }
          }
          ++stream_no;
        }
        strl_at += 8 + strl_size + (strl_size & 1);
      }
    }
    else if ( FCC_IS(list, "movi") )
    {
      m->movi_start = at + 12;
      m->movi_end = list_end;
    }
    at += 8 + top_size + (top_size & 1);
  }

  if ( !m->aud_ok || !m->movi_end )
  {
    if ( m->aud_ok )
      DBG_LOG("MciFfmpeg: movie sound: a sound stream but no movi list, skipping");
    fclose(m->aud);
    m->aud = 0;
    m->aud_ok = 0;
    return;
  }

  /* One pass over the movi headers: where the first of our chunks starts (to rewind on PLAY)
     and how much sound there is in total, which is what makes the summary line mean something. */
  for ( data_at = m->movi_start;
        data_at + 8 <= m->movi_end && RiffPeek(m->aud, data_at, fourcc, &data_size);
        data_at += 8 + data_size + (data_size & 1) )
  {
    if ( RiffU32(fourcc) != m->aud_fcc )
      continue;
    if ( !m->aud_total )
    {
      m->first_off = data_at + 8;
      m->first_size = data_size;
    }
    m->aud_total += data_size;
    ++chunks;
  }
  if ( !m->aud_total )
  {
    DBG_LOG("MciFfmpeg: movie sound: the stream has a header but no chunks, skipping");
    fclose(m->aud);
    m->aud = 0;
    m->aud_ok = 0;
    return;
  }
  DBG_LOG("MciFfmpeg: movie sound: stream %u PCM %u Hz mono 8 bit, %lu bytes in %lu chunks",
          aud_stream, m->aud_rate, m->aud_total, chunks);
}

/* Rewind the sound to the first chunk and let the mixer know a movie is playing. */
static void MciAudioStart(ShimMciState *m)
{
#if PILOTS_MOVIE_AUDIO
  if ( !m->aud_ok || !m->aud_total || !SdlMix_MovieOpen(m->aud_rate) )
    return;
  if ( fseek(m->aud, (long)m->first_off, SEEK_SET) != 0 )
    return;
  m->chunk_left = m->first_size;
  m->aud_fed = 0;
  m->aud_eof = 0;
  DBG_LOG("MciFfmpeg: movie sound started: %u Hz mono 8 bit, %lu bytes to give",
          m->aud_rate, m->aud_total);
#else
  /* The mixer that would carry a movie's sound is not ported yet. The probe
     still walks the stream and logs what it found (so the parsing half is fully
     exercised), but nothing is ever fed: clearing aud_ok sends Pump/Stop down
     their "no sound stream" path, which is the honest state. Said once. */
  {
    static int s_SilentLogged;
    if ( m->aud_ok && !s_SilentLogged )
    {
      s_SilentLogged = 1;
      DBG_LOG("MciFfmpeg: movie sound found but the mixer is not ported yet - playing silent");
    }
  }
  m->aud_ok = 0;
#endif
}

/* The other half: queued samples go with the movie, and the accounting is said once. */
static void MciAudioStop(ShimMciState *m)
{
  if ( !m->aud_ok )
    return;
  if ( m->aud_fed )
    DBG_LOG("MciFfmpeg: movie sound stopped: %lu of %lu bytes reached the mixer",
            m->aud_fed, m->aud_total);
#if PILOTS_MOVIE_AUDIO
  SdlMix_MovieClose();
#endif
  m->aud_fed = 0;
  m->chunk_left = 0;
  m->aud_eof = 0;
}

static void MciAudioFree(ShimMciState *m)
{
  MciAudioStop(m);
  if ( m->aud )
  {
    fclose(m->aud);
    m->aud = 0;
  }
  m->aud_ok = 0;
  m->aud_rate = 0;
  m->aud_total = 0;
  m->movi_start = m->movi_end = 0;
  m->first_off = m->first_size = m->chunk_left = 0;
}

/* Move the read cursor forward without playing anything: used when the sound has fallen far
   enough behind the picture that hurrying through it would be worse than losing it. */
static void MciAudioSkip(ShimMciState *m, unsigned long bytes)
{
  while ( bytes )
  {
    if ( !m->chunk_left && !MciAudioNextChunk(m) )
    {
      m->aud_eof = 1;
      return;
    }
    if ( m->chunk_left <= bytes )
    {
      bytes -= m->chunk_left;
      m->aud_fed += m->chunk_left;
      fseek(m->aud, (long)m->chunk_left, SEEK_CUR);
      m->chunk_left = 0;
    }
    else
    {
      fseek(m->aud, (long)bytes, SEEK_CUR);
      m->aud_fed += bytes;
      m->chunk_left -= bytes;
      return;
    }
  }
}

/* Feed the mixer up to where the picture has reached. The clock is the video's own and it is a
   stream clock, not a wall clock: frame_due_us is base_wall_us plus the frame's timestamp, so
   shown_us is how far into the movie is on screen. A starved pump therefore holds the sound with
   the picture instead of letting it run on, which is what the console's slow transitions need. */
static void MciAudioPump(ShimMciState *m)
{
  unsigned char buf[4096];
  int64_t shown_us;
  unsigned long target, want, got, taken;

  if ( !m->aud_ok || m->aud_eof || !m->aud || !m->disp_valid )
    return;
  shown_us = m->last_end_us - m->base_wall_us;
  if ( shown_us < 0 )
    shown_us = 0;
  target = (unsigned long)((shown_us * (int64_t)m->aud_rate) / 1000000);
  if ( target > m->aud_fed + m->aud_rate / 2 )  /* more than half a second behind: the pump was
                                                   starved, and catching up would sound like a
                                                   fast-forward, so the gap is cut instead */
  {
    DBG_LOG("MciFfmpeg: movie sound %lu samples behind the picture, dropping them", target - m->aud_fed);
    MciAudioSkip(m, target - m->aud_fed);
    if ( m->aud_eof )
      return;
  }
  while ( m->aud_fed < target )
  {
    if ( !m->chunk_left && !MciAudioNextChunk(m) )
    {
      m->aud_eof = 1;
      DBG_LOG("MciFfmpeg: movie sound: no more chunks, %lu of %lu bytes played",
              m->aud_fed, m->aud_total);
      return;
    }
    want = target - m->aud_fed;
    if ( want > m->chunk_left )
      want = m->chunk_left;
    if ( want > sizeof buf )
      want = sizeof buf;
    got = (unsigned long)fread(buf, 1, want, m->aud);
    if ( !got )
    {
      m->aud_eof = 1;
      return;
    }
#if PILOTS_MOVIE_AUDIO
    taken = SdlMix_MovieWrite(buf, (unsigned int)got);
#else
    taken = got;                         /* unreachable with PILOTS_MOVIE_AUDIO 0 (aud_ok is
                                            cleared in MciAudioStart): keeps the accounting below
                                            compilable without the mixer */
#endif
    /* Whatever the mixer really took is already out of the chunk and counts towards the
       picture's position - even on a partial take, where only the rejection is rewound
       below. Skipping this left chunk_left and aud_fed behind by `taken`: the file then
       ran ahead of the chunk boundary (the next chunk's bytes read as PCM), the tempo
       feed under-counted, and the next-chunk search started off the boundary. taken==0
       - the usual reason for a partial write - changes nothing here either way. */
    m->chunk_left -= taken;
    m->aud_fed += taken;
    if ( taken < got )                           /* the mixer's ring is full: same bytes next pump */
    {
      fseek(m->aud, (long)-(long)(got - taken), SEEK_CUR);
      return;
    }
  }
}

static void MciResetPlayback(void)
{
  ShimMciState *m = &s_Mci;

  m->playing = 0;
  m->notify_pending = 0;
  m->notify_hwnd = 0;
  m->eof = 0;
  m->draining = 0;
  m->pend = 0;
  m->disp_valid = 0;
  m->timing_on = 0;
  m->frames_shown = 0;
  m->frame_pts_us = 0;
  m->frame_due_us = 0;
  m->frame_dur_us = 0;
  m->base_wall_us = 0;
  m->base_pts_us = 0;
  m->last_end_us = 0;
  m->last_pts_us = 0;
}

static void MciFreeStream(void)
{
  ShimMciState *m = &s_Mci;

  MciAudioFree(m);                                /* the sound's own file handle goes with the stream */
  if ( m->sws )
  {
    sws_freeContext(m->sws);
    m->sws = NULL;
  }
  av_frame_free(&m->frame);
  av_packet_free(&m->pkt);
  avcodec_free_context(&m->dec);
  if ( m->fmt )
    avformat_close_input(&m->fmt);
  if ( m->buf[0] )
    av_freep(&m->buf[0]);
  if ( m->buf[1] )
    av_freep(&m->buf[1]);
  m->stream_idx = -1;
  m->is_open = 0;
  m->child = 0;
  m->win_x = m->win_y = m->win_w = m->win_h = 0;
  MciResetPlayback();
}

/* MCI completion: post MM_MCINOTIFY once (MCI_NOTIFY_SUCCESSFUL), like MCIAvi
   does when a notified MCI_PLAY finishes or is stopped. */
static void MciNotifyDone(void)
{
  ShimMciState *m = &s_Mci;

  if ( !m->notify_pending )
    return;
  m->notify_pending = 0;
  DBG_LOG("MciFfmpeg: posting MM_MCINOTIFY(1) to hwnd=0x%08X (device %u)",
          (unsigned)(uintptr_t)m->notify_hwnd, m->id);
  if ( m->notify_hwnd )
    PostMessageA(m->notify_hwnd, MM_MCINOTIFY, 1 /* MCI_NOTIFY_SUCCESSFUL */, (LPARAM)m->id);
}

/* PlayMCIVideo stores (WORD)g_MainWindow into MCI_PLAY_PARMS.dwCallback (Win9x
   handles fit in a word); map the truncated value back to the full handle. */
static HWND MciResolveCallbackHwnd(DWORD_PTR callback)
{
  HWND h = (HWND)(uintptr_t)(callback & 0xFFFFu);

  if ( (uintptr_t)h == ((uintptr_t)WinShimMainWindow() & 0xFFFFu) )
    h = WinShimMainWindow();
  return h;
}

static int MciOpenElement(const char *win_path)
{
  ShimMciState *m = &s_Mci;
  const AVCodec *codec;
  char path[512];
  int err;

  MciFreeStream();
  WinShim_ResolvePath(win_path, path, sizeof path); /* case-insensitive like Win9x */
  DBG_LOG("MciFfmpeg: MCI_OPEN '%s' -> '%s'", win_path, path);

  if ( (err = avformat_open_input(&m->fmt, path, NULL, NULL)) < 0 )
  {
    MciLogAvError("avformat_open_input", err);
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }
  if ( (err = avformat_find_stream_info(m->fmt, NULL)) < 0 )
  {
    MciLogAvError("avformat_find_stream_info", err);
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }

  codec = NULL;
  err = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if ( err < 0 || !codec )
  {
    MciLogAvError("no video stream", err < 0 ? err : 0);
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }
  m->stream_idx = err;

  if ( !(m->dec = avcodec_alloc_context3(codec)) )
  {
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }
  if ( (err = avcodec_parameters_to_context(m->dec, m->fmt->streams[m->stream_idx]->codecpar)) < 0
    || (err = avcodec_open2(m->dec, codec, NULL)) < 0 )
  {
    MciLogAvError("codec open", err);
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }
  if ( m->dec->width <= 0 || m->dec->height <= 0 )
  {
    DBG_LOG("MciFfmpeg: bad stream dimensions %dx%d", m->dec->width, m->dec->height);
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }

  if ( !(m->frame = av_frame_alloc()) || !(m->pkt = av_packet_alloc()) )
  {
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }

  m->stride = m->dec->width * 4;
  m->buf[0] = (uint8_t *)av_malloc((size_t)m->stride * m->dec->height);
  m->buf[1] = (uint8_t *)av_malloc((size_t)m->stride * m->dec->height);
  if ( !m->buf[0] || !m->buf[1] )
  {
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }

  m->sws = sws_getContext(m->dec->width, m->dec->height, m->dec->pix_fmt,
                          m->dec->width, m->dec->height, AV_PIX_FMT_RGB32,
                          SWS_BILINEAR, NULL, NULL, NULL);
  if ( !m->sws )
  {
    DBG_LOG("MciFfmpeg: sws_getContext failed");
    MciFreeStream();
    return MCIERR_FILE_NOT_FOUND;
  }

  m->is_open = 1;
  DBG_LOG("MciFfmpeg: stream %dx%d pix_fmt=%d tb=%d/%d",
          m->dec->width, m->dec->height, m->dec->pix_fmt,
          m->fmt->streams[m->stream_idx]->time_base.num,
          m->fmt->streams[m->stream_idx]->time_base.den);
  MciAudioProbe(m, path);                         /* ffmpeg looked at the video only; the sound is ours */
  return 0;
}

/* Decode exactly one video frame into buf[1 - disp_idx]; scale to BGRA.
   Returns 1 = frame ready (frame_pts_us/frame_due_us/frame_dur_us set),
   0 = end of stream, -1 = error (treated as end of stream). */
static int MciDecodeFrame(int64_t now_us)
{
  ShimMciState *m = &s_Mci;

  for (;;)
  {
    int r = avcodec_receive_frame(m->dec, m->frame);

    if ( r == 0 )
    {
      uint8_t *dst[4];
      int dst_stride[4];
      AVRational tb = m->fmt->streams[m->stream_idx]->time_base;
      int64_t pts_us, dur_us, rel;

      if ( m->frame->best_effort_timestamp == AV_NOPTS_VALUE )
        pts_us = m->last_pts_us + m->frame_dur_us;
      else
        pts_us = av_rescale_q(m->frame->best_effort_timestamp, tb, (AVRational){ 1, 1000000 });

      {
        AVRational fr = av_guess_frame_rate(m->fmt, m->fmt->streams[m->stream_idx], NULL);
        dur_us = (fr.num > 0 && fr.den > 0) ? (int64_t)(1000000.0 * fr.den / fr.num + 0.5) : 0;
        if ( dur_us <= 0 )
          dur_us = m->frame_dur_us;
        if ( dur_us <= 0 )
          dur_us = 66666;                      /* 15 fps fallback */
      }

      dst[0] = m->buf[1 - m->disp_idx];
      dst[1] = dst[2] = dst[3] = NULL;
      dst_stride[0] = m->stride;
      dst_stride[1] = dst_stride[2] = dst_stride[3] = 0;
      sws_scale(m->sws, (const uint8_t *const *)m->frame->data, m->frame->linesize,
                0, m->dec->height, dst, dst_stride);

      m->frame_pts_us = pts_us;
      m->frame_dur_us = dur_us;
      m->last_pts_us = pts_us;
      m->pend = 1;
      if ( !m->timing_on )
      {
        m->timing_on = 1;
        m->base_wall_us = now_us;
        m->base_pts_us = pts_us;
      }
      rel = pts_us - m->base_pts_us;
      if ( rel < 0 )
        rel = 0;
      m->frame_due_us = m->base_wall_us + rel;
      return 1;
    }
    if ( r == AVERROR_EOF )
      return 0;
    if ( r != AVERROR(EAGAIN) )
    {
      MciLogAvError("receive_frame", r);
      return -1;
    }
    if ( m->draining )                          /* flushed, nothing left (defensive: no spin) */
      return 0;

    r = av_read_frame(m->fmt, m->pkt);
    if ( r < 0 )
    {
      avcodec_send_packet(m->dec, NULL);        /* end of file: flush the decoder */
      m->draining = 1;
      continue;
    }
    if ( m->pkt->stream_index == m->stream_idx )
    {
      r = avcodec_send_packet(m->dec, m->pkt);
      if ( r < 0 && r != AVERROR(EAGAIN) )
      {
        MciLogAvError("send_packet", r);
        av_packet_unref(m->pkt);
        return -1;
      }
    }
    av_packet_unref(m->pkt);
  }
}

/* Called from PeekMessageA's SDL pump while host.c waits for g_AviPlaying. */
void WinShim_VideoPump(void)
{
  ShimMciState *m = &s_Mci;
  int64_t now_us;

  if ( !m->is_open || !m->playing )
    return;
  now_us = (int64_t)SDL_GetTicks() * 1000;

  if ( !m->pend && !m->eof && MciDecodeFrame(now_us) != 1 )
    m->eof = 1;

  if ( m->pend && now_us >= m->frame_due_us )   /* the frame's display time arrived */
  {
    m->disp_idx ^= 1;
    m->pend = 0;
    m->disp_valid = 1;
    m->frames_shown++;
    m->last_end_us = m->frame_due_us + m->frame_dur_us;
  }

  if ( m->disp_valid )                          /* re-present every pump: repairs WM_PAINT over the movie */
    SdlBackendVideoFrame(m->buf[m->disp_idx], m->dec->width, m->dec->height,
                         m->win_x, m->win_y, m->win_w, m->win_h);

  MciAudioPump(m);                              /* the sound follows the picture, not the wall clock */

  if ( m->eof && !m->pend && now_us >= m->last_end_us )
  {
    DBG_LOG("MciFfmpeg: playback finished (%d frames shown)", m->frames_shown);
    m->playing = 0;
    MciNotifyDone();
  }
}

void WinShim_MciNotifyMove(HWND hWnd, int X, int Y, int nWidth, int nHeight)
{
  ShimMciState *m = &s_Mci;

  if ( !m->child || hWnd != m->child )
    return;
  m->win_x = X;
  m->win_y = Y;
  m->win_w = nWidth;
  m->win_h = nHeight;
}

MMRESULT mciSendCommandA(MCIDEVICEID idDevice, UINT uMsg, DWORD dwFlags, DWORD_PTR dwParam)
{
  ShimMciState *m = &s_Mci;

  (void)idDevice;

  switch ( uMsg )
  {
    case MCI_SHIM_OPEN:
    {
      MCI_ANIM_OPEN_PARMSA *openParms = (MCI_ANIM_OPEN_PARMSA *)(uintptr_t)dwParam;
      int err;

      if ( !dwParam )
        return MCIERR_MISSING_PARAMETER;
      /* host.c sends the real MCI_ANIM_OPEN_PARMSA: lpstrElementName carries the
         file, wDeviceID comes back in the same block. Read/write the fields
         through the struct itself, so the layout has exactly one answer (the one
         win32_shim.h defines) instead of the old raw slot arithmetic, which was
         tuned to a dwParam2[] overlay no caller here uses. */
      err = openParms->lpstrElementName
          ? MciOpenElement(openParms->lpstrElementName) : MCIERR_MISSING_PARAMETER;
      if ( err )
        return (MMRESULT)err;
      m->id = s_NextDeviceId++;
      openParms->wDeviceID = m->id;
      m->child = SHIM_MCI_CHILD_HWND;   /* the handle host.c will MoveWindow/ask for */
      DBG_LOG("MciFfmpeg: device id %u", m->id);
      return 0;
    }

    case MCI_SHIM_STATUS:
    {
      MCI_STATUS_PARMS *status = (MCI_STATUS_PARMS *)(uintptr_t)dwParam;

      if ( !dwParam )
        return MCIERR_MISSING_PARAMETER;
      if ( status->dwItem == MCI_DGV_STATUS_HWND )
      {
        /* host.c Host_AviOpen asks the device for its playback window the same way
           the original did (MCI_STATUS + MCI_DGV_STATUS_HWND) and then MoveWindow's
           it onto the movie rect - here that move is WinShim_MciNotifyMove. */
        m->child = SHIM_MCI_CHILD_HWND;
        status->dwReturn = (DWORD_PTR)m->child;
        return 0;
      }
      DBG_LOG("MciFfmpeg: STATUS item 0x%X unanswered", status->dwItem);
      return 0;
    }

    case MCI_SHIM_WINDOW:
    {
      MCI_WINDOW_PARMSA *wnd = (MCI_WINDOW_PARMSA *)(uintptr_t)dwParam;

      if ( !wnd )
        return MCIERR_MISSING_PARAMETER;
      m->child = SHIM_MCI_CHILD_HWND;
      wnd->hWnd = m->child;
      return 0;
    }

    case MCI_SHIM_PLAY:
      if ( !m->is_open )
        return MCIERR_MISSING_PARAMETER;
      MciResetPlayback();
      av_seek_frame(m->fmt, m->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(m->dec);
      m->notify_pending = (dwFlags & MCI_NOTIFY) != 0;  /* MCI_NOTIFY */
      m->notify_hwnd = MciResolveCallbackHwnd(
          dwParam ? ((MCI_PLAY_PARMS *)(uintptr_t)dwParam)->dwCallback : 0);
      m->playing = 1;
      MciAudioStart(m);                           /* the sound rewinds with the picture */
      DBG_LOG("MciFfmpeg: PLAY notify=%d callback=0x%08X",
              m->notify_pending, (unsigned)(uintptr_t)m->notify_hwnd);
      return 0;

    case MCI_SHIM_STOP:
      DBG_LOG("MciFfmpeg: STOP");
      if ( m->playing )
      {
        m->playing = 0;
        MciAudioStop(m);                          /* a skipped movie takes its queued sound with it */
        MciNotifyDone();                        /* stopping completes the notified MCI_PLAY */
      }
      return 0;

    case MCI_SHIM_CLOSE:
      DBG_LOG("MciFfmpeg: CLOSE");
      m->playing = 0;
      MciNotifyDone();
      MciFreeStream();
      return 0;

    default:
      DBG_LOG("MciFfmpeg: unhandled command 0x%X ignored", uMsg);
      return 0;
  }
}

int mciGetErrorStringA(MCIERROR mcierr, LPSTR lpBuffer, UINT uLength)
{
  const char *text;
  UINT i;

  switch ( mcierr )
  {
    case 0:
      text = "The command completed successfully.";
      break;
    case MCIERR_FILE_NOT_FOUND:
      text = "The specified file cannot be found.";
      break;
    case MCIERR_MISSING_PARAMETER:
      text = "A required parameter is missing.";
      break;
    default:
      text = "An unknown MCI error occurred.";
      break;
  }
  if ( !lpBuffer || !uLength )
    return 0;
  for ( i = 0; i + 1 < uLength && text[i]; ++i )
    lpBuffer[i] = text[i];
  lpBuffer[i] = '\0';
  return 1;
}

#endif /* !_WIN32 */
