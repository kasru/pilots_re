/*
 * mci_ffmpeg.c - ffmpeg-backed mciSendCommandA for the Linux port: exactly the
 * MCI subset video.c uses (MCI_OPEN/MCI_WINDOW/MCI_PLAY/MCI_STOP/MCI_CLOSE) to
 * play the AVI intro/start movies. On Windows the real winmm MCIAvi device does
 * this; here frames are decoded on the message-pump side and composited over
 * the game frame at the MCI child window's rect:
 *
 *  - WinShim_VideoPump() runs from PeekMessageA's SDL pump (WinShim_PumpSDLToQueue),
 *    i.e. while video.c busy-waits in `while (g_VideoPlaying)` { PeekMessageA; Sleep(5) }.
 *    It paces frames against the wall clock (base time = first decoded frame,
 *    per-frame due time from stream pts, so no drift), keeps the last shown
 *    frame on screen until its display time ends, and at end-of-stream (or after
 *    MCI_STOP, which completes the notified MCI_PLAY like real MCIAvi) posts
 *    MM_MCINOTIFY to the MCI_PLAY callback window - that is what makes
 *    VideoWndProc clear g_VideoPlaying (video.c:544) and unblock the wait loop.
 *
 * Struct offsets match what video.c writes: MCI_OPEN_PARMS wDeviceID at +4,
 * lpstrElementName at +12; MCI_WINDOW_PARMS hWnd at +4 (video.c pack(1));
 * MCI_PLAY_PARMS dwCallback at +0 (PlayMCIVideo truncates g_MainWindow to a
 * word - Win9x handles were word-sized, so resolve it back to the full handle).
 *
 * Empty TU on Windows (real winmm provides mciSendCommandA).
 */
/* ffmpeg headers BEFORE pilots_fwd.h on purpose: the generated globals headers
   define object-like macros (offset, ...) that break libav* declarations, and
   lib headers must not see them. pilots_fwd.h is still the one game header. */
#ifndef _WIN32
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif

#include "pilots_fwd.h"

#ifndef _WIN32

/* mmsystem MCI command ids used by video.c */
#define MCI_SHIM_OPEN    0x803
#define MCI_SHIM_CLOSE   0x804
#define MCI_SHIM_PLAY    0x806
#define MCI_SHIM_STOP    0x808
#define MCI_SHIM_WINDOW  0x814

/* fake HWND for the MCI child window (MCI_WINDOW output = g_VideoWindowHandle);
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
} ShimMciState;

static ShimMciState s_Mci;
static MCIDEVICEID  s_NextDeviceId = 1;

static void MciLogAvError(const char *what, int err)
{
  char buf[128];

  av_strerror(err, buf, sizeof buf);
  DBG_LOG("MciFfmpeg: %s: %s", what, buf);
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

/* Called from PeekMessageA's SDL pump while video.c waits for g_VideoPlaying. */
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
      const char *elem;
      int err;

      if ( !dwParam )
        return MCIERR_MISSING_PARAMETER;
      elem = (const char *)(uintptr_t)*(DWORD_PTR *)((char *)(uintptr_t)dwParam + 12);
      err = elem ? MciOpenElement(elem) : MCIERR_MISSING_PARAMETER;
      if ( err )
        return (MMRESULT)err;
      m->id = s_NextDeviceId++;
      *(DWORD *)((char *)(uintptr_t)dwParam + 4) = m->id;  /* MCI_OPEN_PARMS.wDeviceID */
      DBG_LOG("MciFfmpeg: device id %u", m->id);
      return 0;
    }

    case MCI_SHIM_WINDOW:
      if ( !dwParam )
        return MCIERR_MISSING_PARAMETER;
      m->child = SHIM_MCI_CHILD_HWND;
      *(HWND *)((char *)(uintptr_t)dwParam + 4) = m->child; /* MCI_WINDOW_PARMS.hWnd */
      DBG_LOG("MciFfmpeg: MCI_WINDOW child=0x%08X", (unsigned)(uintptr_t)m->child);
      return 0;

    case MCI_SHIM_PLAY:
      if ( !m->is_open )
        return MCIERR_MISSING_PARAMETER;
      MciResetPlayback();
      av_seek_frame(m->fmt, m->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(m->dec);
      m->notify_pending = (dwFlags & 1u) != 0;  /* MCI_NOTIFY */
      m->notify_hwnd = MciResolveCallbackHwnd(dwParam ? *(DWORD_PTR *)(char *)(uintptr_t)dwParam : 0);
      m->playing = 1;
      DBG_LOG("MciFfmpeg: PLAY notify=%d callback=0x%08X",
              m->notify_pending, (unsigned)(uintptr_t)m->notify_hwnd);
      return 0;

    case MCI_SHIM_STOP:
      DBG_LOG("MciFfmpeg: STOP");
      if ( m->playing )
      {
        m->playing = 0;
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
