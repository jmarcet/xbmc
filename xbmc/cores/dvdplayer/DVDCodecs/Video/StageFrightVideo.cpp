/*
 *      Copyright (C) 2010-2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
/***************************************************************************/
/*
 * Interface to the Android Stagefright library for
 * H/W accelerated H.264 decoding
 *
 * Copyright (C) 2011 Mohamed Naufal
 * Copyright (C) 2011 Martin Storsjö
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "system.h"

#include "StageFrightVideo.h"

#include "DVDClock.h"
#include "threads/Event.h"
#include "utils/log.h"
#include "utils/fastmemcpy.h"

#include <binder/ProcessState.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <utils/List.h>
#include <utils/RefBase.h>
#include <gui/SurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>
#include <EGL/egl.h>

#include <new>
#include <map>
#include <queue>

#define OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00
#define STAGEFRIGHT_DEBUG_VERBOSE 1
#define CLASSNAME "CStageFrightVideo"
#define MINBUFIN 50

const char *MEDIA_MIMETYPE_VIDEO_WMV  = "video/x-ms-wmv";

using namespace android;

static int64_t pts_dtoi(double pts)
{
  return (int64_t)(pts);
}

static double pts_itod(int64_t pts)
{
  return (double)pts;
}

struct Frame
{
  status_t status;
  int32_t width, height;
  int64_t pts;
  MediaBuffer* medbuf;
};

class StagefrightContext : public MediaBufferObserver
{
public:
  StagefrightContext()
    : source(NULL)
    , width(-1), height(-1)
    , cur_frame(NULL), prev_frame(NULL)
    , client(NULL), decoder(NULL), decoder_component(NULL)
    , drop_state(false)
  {}

  virtual void signalBufferReturned(MediaBuffer *buffer)
  {
    buffer->setObserver(NULL);
    buffer->release();
  }

  MediaBuffer* getBuffer(size_t size)
  {
    MediaBuffer* buf = new MediaBuffer(size);
    buf->setObserver(this);
    buf->add_ref();
    return buf;
  }

  sp<MediaSource> *source;
  List<Frame*> in_queue;
  pthread_mutex_t in_mutex;
  pthread_cond_t condition;

  Frame *cur_frame;
  Frame *prev_frame;
  bool source_done;

  OMXClient *client;
  sp<MediaSource> *decoder;
  const char *decoder_component;
  
  bool drop_state;
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  unsigned int cycle_time;
#endif
};

class CustomSource : public MediaSource
{
public:
  CustomSource(StagefrightContext *ctx, sp<MetaData> meta)
  {
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: creating source\n", CLASSNAME);
#endif
    s = ctx;
    source_meta = meta;
  }

  virtual sp<MetaData> getFormat()
  {
    return source_meta;
  }

  virtual status_t start(MetaData *params)
  {
    return OK;
  }

  virtual status_t stop()
  {
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer,
                        const MediaSource::ReadOptions *options)
  {
    Frame *frame;
    status_t ret;
    *buffer = NULL;

    pthread_mutex_lock(&s->in_mutex);
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: reading source(%d)\n", CLASSNAME,s->in_queue.size());
#endif
    
    if (s->in_queue.empty())
    {
      pthread_mutex_unlock(&s->in_mutex);
      return ERROR_END_OF_STREAM;
    }

    frame = *s->in_queue.begin();
    ret = frame->status;

    if (ret == OK)
      *buffer = frame->medbuf->clone();

    s->in_queue.erase(s->in_queue.begin());
    pthread_mutex_unlock(&s->in_mutex);
    
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> exiting reading source(%d); pts:%llu\n", s->in_queue.size(),frame->pts);
#endif

    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);

    return ret;
  }

private:
  sp<MetaData> source_meta;
  StagefrightContext *s;
};

/***********************************************************/

bool CStageFrightVideo::Open(CDVDStreamInfo &hints)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Open\n", CLASSNAME);
#endif

  // stagefright crashes with null size. Trap this...
  if (!hints.width || !hints.height)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"null size, cannot handle");
    return false;
  }

  const char* mimetype;
  switch (hints.codec)
  {
  case CODEC_ID_H264:
    mimetype = MEDIA_MIMETYPE_VIDEO_AVC;
    break;
  case CODEC_ID_MPEG4:
    mimetype = MEDIA_MIMETYPE_VIDEO_MPEG4;
    break;
  case CODEC_ID_MPEG2VIDEO:
    mimetype = MEDIA_MIMETYPE_VIDEO_MPEG2;
    break;
  case CODEC_ID_VP8:
    mimetype = MEDIA_MIMETYPE_VIDEO_VPX;
    break;
  case CODEC_ID_VC1:
    mimetype = MEDIA_MIMETYPE_VIDEO_WMV;
    break;
  default:
    return false;
    break;
  }

  sp<MetaData> meta, outFormat;
  int32_t colorFormat = 0;

  m_context = new StagefrightContext;
  m_context->width     = hints.width;
  m_context->height    = hints.height;
 
  meta = new MetaData;
  if (meta == NULL)
  {
    goto fail;
  }
  meta->setCString(kKeyMIMEType, mimetype);
  meta->setInt32(kKeyWidth, m_context->width);
  meta->setInt32(kKeyHeight, m_context->height);
  meta->setData(kKeyAVCC, kTypeAVCC, hints.extradata, hints.extrasize);

  android::ProcessState::self()->startThreadPool();

  m_context->source    = new sp<MediaSource>();
  *m_context->source   = new CustomSource(m_context, meta);
  m_context->client    = new OMXClient;

  if (m_context->source == NULL || !m_context->client)
  {
    goto fail;
  }

  if (m_context->client->connect() !=  OK)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot connect OMX client");
    goto fail;
  }  

  m_context->decoder  = new sp<MediaSource>();
  *m_context->decoder = OMXCodec::Create(m_context->client->interface(), meta,
                                         false, *m_context->source, NULL,
                                         OMXCodec::kClientNeedsFramebuffer | OMXCodec::kHardwareCodecsOnly,
                                         m_context->mNatWindow);
  if (!((*m_context->decoder != NULL) && ((*m_context->decoder)->start() ==  OK)))
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot start decoder");
    m_context->client->disconnect();
    goto fail;
  }

  outFormat = (*m_context->decoder)->getFormat();
  outFormat->findInt32(kKeyColorFormat, &colorFormat);
  if (colorFormat != OMX_COLOR_FormatYUV420Planar)
  {
    CLog::Log(LOGERROR, "%s::%s - %s: %d\n", CLASSNAME, __func__,"Unsupported color format",colorFormat);
    m_context->client->disconnect();
    goto fail;
  }

  outFormat->findCString(kKeyDecoderComponent, &m_context->decoder_component);
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - Decoder: %s\n", CLASSNAME, __func__,m_context->decoder_component);
#endif

  pthread_mutex_init(&m_context->in_mutex, NULL);
  pthread_cond_init(&m_context->condition, NULL);
  
  return true;

fail:
  delete m_context->client;
  return false;
}

int  CStageFrightVideo::Decode(BYTE *pData, int iSize, double dts, double pts)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::Decode - d:%p; s:%d; dts:%f; pts:%f\n", CLASSNAME, pData, iSize, dts, pts);
  if (m_context->cycle_time != 0)
    CLog::Log(LOGDEBUG, ">>> cycle dur:%d\n", XbmcThreads::SystemClockMillis() - m_context->cycle_time);
  m_context->cycle_time = time;
#endif

  Frame *frame;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  int ret = VC_BUFFER;

  if (demuxer_content)
  {
    frame = (Frame*)malloc(sizeof(Frame));
    if (!frame)
      return VC_ERROR;

    frame->status  = OK;
    frame->pts = (dts != DVD_NOPTS_VALUE) ? pts_dtoi(dts) : ((pts != DVD_NOPTS_VALUE) ? pts_dtoi(pts) : 0);
    frame->medbuf = m_context->getBuffer(demuxer_bytes);
    if (!frame->medbuf)
    {
      free(frame);
      return VC_ERROR;
    }
    fast_memcpy(frame->medbuf->data(), demuxer_content, demuxer_bytes);
    frame->medbuf->meta_data()->clear();
    frame->medbuf->meta_data()->setInt64(kKeyTime, frame->pts);

    pthread_mutex_lock(&m_context->in_mutex);
    m_context->in_queue.push_back(frame);
    pthread_cond_signal(&m_context->condition);
    pthread_mutex_unlock(&m_context->in_mutex);
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s::Decode: pushed IN frame (%d); tm:%d\n", CLASSNAME,m_context->in_queue.size(), XbmcThreads::SystemClockMillis() - time);
#endif
  }
  
  if (m_context->in_queue.size() < MINBUFIN)
    return ret;

	#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  time = XbmcThreads::SystemClockMillis();
	CLog::Log(LOGDEBUG, "%s: >>> Handling frame\n", CLASSNAME);
	#endif
  int32_t w, h;
	frame = (Frame*)malloc(sizeof(Frame));
	if (!frame) {
    m_context->cur_frame = NULL;
	  return VC_ERROR;
  }
  
	frame->medbuf = NULL;
	frame->status = (*m_context->decoder)->read(&frame->medbuf);
	if (frame->status == INFO_FORMAT_CHANGED)
	{
	  if (frame->medbuf)
      frame->medbuf->release();
	  free(frame);
    m_context->cur_frame = NULL;
    return ret;
	}
	else if (frame->status == OK)
	{
	  sp<MetaData> outFormat = (*m_context->decoder)->getFormat();
	  outFormat->findInt32(kKeyWidth , &w);
	  outFormat->findInt32(kKeyHeight, &h);
	  frame->pts = 0;

	  // The OMX.SEC decoder doesn't signal the modified width/height
	  if (m_context->decoder_component && !strncmp(m_context->decoder_component, "OMX.SEC", 7) &&
		  (w & 15 || h & 15))
	  {
		if (((w + 15)&~15) * ((h + 15)&~15) * 3/2 == frame->medbuf->range_length())
		{
		  w = (w + 15)&~15;
		  h = (h + 15)&~15;
		}
	  }
	  frame->width = w;
	  frame->height = h;
	  if (frame->medbuf)
	  {
      frame->medbuf->meta_data()->findInt64(kKeyTime, &(frame->pts));
      if (m_context->drop_state)
      {
        frame->medbuf->release();
        frame->medbuf = NULL;
      }
	  }
	}
	else
	{
	  CLog::Log(LOGERROR, "%s - decoding error (%d)\n", CLASSNAME,frame->status);
	  if (frame->medbuf)
      frame->medbuf->release();
	  free(frame);
    m_context->cur_frame = NULL;
    return VC_ERROR;
	}
  
  m_context->cur_frame = frame;
	#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
	CLog::Log(LOGDEBUG, "%s: >>> pushed OUT frame; tm:%d\n", CLASSNAME,XbmcThreads::SystemClockMillis() - time);
	#endif

  if (m_context->cur_frame)
    ret |= VC_PICTURE;

  return ret;
}

bool CStageFrightVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::GetPicture\n", CLASSNAME);
#endif

  bool ret = true;
  status_t status;

  if (!m_context->cur_frame)
    return false;
    
  Frame *frame = m_context->cur_frame;
  status  = frame->status;

  if (status != OK)
  {
    CLog::Log(LOGERROR, "%s::%s - Error getting picture from frame(%d)\n", CLASSNAME, __func__,status);
    if (frame->medbuf) {
      frame->medbuf->release();
    }
    free(frame);
    m_context->cur_frame = NULL;
    return false;
  }

  pDvdVideoPicture->format = RENDER_FMT_YUV420P;
  pDvdVideoPicture->pts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->color_range  = 0;
  pDvdVideoPicture->color_matrix = 4;
  pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->iWidth  = frame->width;
  pDvdVideoPicture->iHeight = frame->height;
  pDvdVideoPicture->iDisplayWidth = frame->width;
  pDvdVideoPicture->iDisplayHeight = frame->height;
  pDvdVideoPicture->pts = pts_itod(frame->pts);
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, ">>> pic pts:%f, data:%p, tm:%d\n", pDvdVideoPicture->pts, frame->medbuf, XbmcThreads::SystemClockMillis() - time);
#endif

  unsigned int luma_pixels = frame->width * frame->height;
  unsigned int chroma_pixels = luma_pixels/4;
  BYTE* data = NULL;
  if (frame->medbuf)
    data = (BYTE*)(frame->medbuf->data() + frame->medbuf->range_offset());
  pDvdVideoPicture->iLineSize[0] = frame->width;
  pDvdVideoPicture->iLineSize[1] = frame->width / 2;
  pDvdVideoPicture->iLineSize[2] = frame->width / 2;
  pDvdVideoPicture->iLineSize[3] = 0;
  pDvdVideoPicture->data[0] = data;
  pDvdVideoPicture->data[1] = pDvdVideoPicture->data[0] + (frame->width * frame->height);
  pDvdVideoPicture->data[2] = pDvdVideoPicture->data[1] + (frame->width/2 * frame->height/2);
  pDvdVideoPicture->data[3] = 0;

  pDvdVideoPicture->iFlags |= data == NULL ? DVP_FLAG_DROPPED : 0;
  m_context->prev_frame = m_context->cur_frame;
  m_context->cur_frame = NULL;

  return true;
}

bool CStageFrightVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::ClearPicture\n", CLASSNAME);
#endif
  if (m_context->prev_frame) {
    if (m_context->prev_frame->medbuf)
      m_context->prev_frame->medbuf->release();
    free(m_context->prev_frame);
    m_context->prev_frame = NULL;
  }

  return true;
}

void CStageFrightVideo::Close()
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Close\n", CLASSNAME);
#endif

  Frame *frame;

#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning IN(%d)\n", m_context->in_queue.size());
#endif
  while (!m_context->in_queue.empty())
  {
    frame = *m_context->in_queue.begin();
    m_context->in_queue.erase(m_context->in_queue.begin());
    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);
  }

  if (m_context->cur_frame)
  {
    if (m_context->cur_frame->medbuf)
      m_context->cur_frame->medbuf->release();
    free(m_context->cur_frame);
  }
  if (m_context->prev_frame)
  {
    if (m_context->prev_frame->medbuf)
      m_context->prev_frame->medbuf->release();
    free(m_context->prev_frame);
  }

  (*m_context->decoder)->stop();
  m_context->client->disconnect();

  if (m_context->decoder_component)
    free(&m_context->decoder_component);

  delete m_context->client;
  delete m_context->decoder;
  delete m_context->source;

  pthread_mutex_destroy(&m_context->in_mutex);
  pthread_cond_destroy(&m_context->condition);

  delete m_context;
}

void CStageFrightVideo::Reset(void)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Reset\n", CLASSNAME);
#endif
  ::Sleep(100);
}

void CStageFrightVideo::SetDropState(bool bDrop)
{
#if defined(STAGEFRIGHT_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::SetDropState (%d)\n", CLASSNAME,bDrop);
#endif
  
  m_context->drop_state = bDrop;
}


