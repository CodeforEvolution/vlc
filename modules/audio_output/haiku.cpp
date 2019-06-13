/*****************************************************************************
 * haiku.cpp: Haiku audio output
 *****************************************************************************
 * Copyright (C) 1999, 2000, 2001 the VideoLAN team
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Eric Petit <titer@videolan.org>
 *          Jacob Secunda <secundaja@gmail.com> - Port to Haiku
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Headers
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_block_helper.h>
#include <vlc_cpu.h>

#include <media/MediaDefs.h>
#include <media/Roster.h>
#include <media/SoundPlayer.h>

/*****************************************************************************
 * aout_sys_t: Haiku audio output method descriptor
 *****************************************************************************/
typedef struct aout_sys_t
{
    BSoundPlayer*        player;
    block_bytestream_t*  buffers;
    bool                 mute;
    float                volume;
} aout_sys_t;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Start        (audio_output_t* aout, audio_sample_format_t* restrict fmt)
static void Stop        (audio_output_t* aout)    
static int  TimeGet     (audio_output_t* aout, vlc_tick_t* delay);
static void Play        (audio_output_t* aout, block_t* block, vlc_tick_t tick);
static void PlayBuffer  (void* aout, void* buffer, size_t size,
                            const media_raw_audio_format& format);
static void Pause       (audio_output_t* aout, bool paused, vlc_tick_t date);
static void Flush       (audio_output_t* aout);
static int  VolumeSet   (audio_output_t* aout, float volume)
static int  MuteSet     (audio_output_t* aout, bool mute)
static int  Open        (vlc_object_t* obj);
static void Close       (vlc_object_t* obj);

/*****************************************************************************
 * Haiku audio output module descriptor
 *****************************************************************************/
vlc_module_begin ()
    set_shortname(N_("Haiku"))
    set_description(N_("Haiku Mediakit audio output"))
    set_capability("audio output", 100)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_callbacks(Open, Close)
    add_shortcut("haiku")
vlc_module_end ()

/*****************************************************************************
 * Start
 *****************************************************************************/
static int Start(audio_output_t* aout, audio_sample_format_t* restrict fmt)
{
    aout_sys_t* sys = aout->sys;
    
    block_BytestreamInit(sys->buffers);

    /* TODO: Support for Encoded Audio
    switch (fmt->i_format)
    {
        case VLC_CODEC_A52:
        case VLC_CODEC_EAC3:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_bytes_per_frame = 4;
            fmt->i_frame_length = 1;
            break;
        case VLC_CODEC_DTS:
        case VLC_CODEC_TRUEHD:
        case VLC_CODEC_MLP:
            fmt->i_format = VLC_CODEC_SPDIFL;
            fmt->i_rate = 768000;
            fmt->i_bytes_per_frame = 16;
            fmt->i_frame_length = 1;
            break;
    }
    */
    
    int nb_channels = aout_FormatNbChannels(fmt);
    
    if (nb_channels == 0)
        return VLC_EGENERIC;
    
    /* BSoundPlayer does not support more than 2 channels AFAIK */
    if(nb_channels > 2) {
        nb_channels = 2;
        fmt.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
    }
    
    media_raw_audio_format* format;
    format = (media_raw_audio_format*)
        malloc(sizeof(media_raw_audio_format));
    
    if (unlikely(format == NULL))
        return VLC_ENOMEM;
    
    format = media_raw_audio_format::wildcard;
    
    format->frame_rate = fmt->i_rate;
    format->channel_count = nb_channels;
    
    fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
    
    uint32 audio_format;
    switch (fmt->i_format) {
        case VLC_CODEC_S8:
        {
            audio_format = media_raw_audio_format::B_AUDIO_CHAR;
            break;
        }
        case VLC_CODEC_U8:
        {
            audio_format = media_raw_audio_format::B_AUDIO_UCHAR;
            break;
        }
        case VLC_CODEC_S16N:
        {
            audio_format = media_raw_audio_format::B_AUDIO_SHORT;
            break;   
        }
        case VLC_CODEC_S32N:
        {
            audio_format = media_raw_audio_format::B_AUDIO_INT;
            break;   
        }
        case VLC_CODEC_FL32:
        {
            audio_format = media_raw_audio_format::B_AUDIO_FLOAT;
            break;   
        }
        case VLC_CODEC_FL64:
        {
            fmt->i_format = VLC_CODEC_FL32;
            audio_format = media_raw_audio_format::B_AUDIO_FLOAT;
            break;
        }
        default:
        {
            if (!AOUT_FMT_LINEAR(fmt) || aout_FormatNbChannels(fmt) == 0)
                return VLC_EGENERIC;

            fmt->i_format = HAVE_FPU ? VLC_CODEC_FL32 : VLC_CODEC_S16N;
            audio_format = HAVE_FPU ? media_raw_audio_format:B_AUDIO_FLOAT :
                media_raw_audio_format::B_AUDIO_SHORT
      
            break;
        }
    }
    aout_FormatPrepare(fmt);

    format->format = audio_format;
    format->byte_order = B_MEDIA_HOST_ENDIAN;
    
    status_t error = B_ERROR;
    BMediaRoster* roster = BMediaRoster::Roster(&error);
    
    if (error != B_OK)
        format->buffer_size = media_raw_audio_format::wildcard.buffer_size;
    else
        format->buffer_size = roster->AudioBufferSizeFor(format->channels,
            format->format, format->frame_rate, B_UNKNOWN_BUS);
    
    sys->player = new BSoundPlayer(format, "VLC Audio Player", PlayBuffer,
        NULL, aout);
    if(sys->player->InitCheck() != B_OK)
    {
        msg_Err(aout, "BSoundPlayer InitCheck() failed");
        delete sys->player;
        free(sys);
        return VLC_EGENERIC;
    }
    
    sys->player->Start();

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Stop
 *****************************************************************************/
static void Stop(audio_output_t* aout)
{
    aout_sys_t* sys = aout->sys;
    
    sys->player->SetHasData(false);
    sys->player->Stop(true, false);

    block_BytestreamRelease(sys->buffers);
}

/*****************************************************************************
 * TimeGet
 *****************************************************************************/
static void TimeGet(audio_output_t* aout, vlc_tick_t* delay)
{
    aout_sys_t* sys = aout->sys;
    
    *delay = sys->player->Latency();
}

/*****************************************************************************
 * Play
 *****************************************************************************/
static void Play(audio_output_t* aout, block_t* block, vlc_tick_t date)
{
    aout_sys_t* sys = aout->sys;
    
    block_BytestreamPush(sys->buffers, block);
    
    sys->player->SetHasData(true);
    
    (void)date;
}

/*****************************************************************************
 * PlayBuffer
 *****************************************************************************/
static void PlayBuffer(void* aout, void* buffer, size_t size,
                          const media_raw_audio_format &format)
{
    // NEEDS TESTING
    audio_output_t* aout = (audio_output_t*)aout;
    aout_sys_t* sys = aout->sys;
    
    size_t length = size;
    size_t remaining = block_BytestreamRemaining(sys->buffers);
    length = length > remaining ? remaining : length;

    int result = block_GetBytes(sys->buffers, buffer, length);
    
    if (result != VLC_SUCCESS)
        sys->player->SetHasData(false);
}

/*****************************************************************************
 * Pause
 *****************************************************************************/
static void Pause(audio_output_t* aout, bool paused, vlc_tick_t date)
{
    aout_sys_t* sys = aout->sys;
    
    if (paused)
        sys->player->SetHasData(false);
    else
        sys->player->SetHasData(true);
    
    (void)date;
}

/*****************************************************************************
 * Flush
 *****************************************************************************/
static void Flush(audio_output_t* aout)
{
    aout_sys_t* sys = aout->sys;
    
    sys->player->SetHasData(false);
    
    block_BytestreamEmpty(sys->buffers);
}

/*****************************************************************************
 * VolumeSet
 *****************************************************************************/
static int VolumeSet(audio_output_t* aout, float volume)
{
    aout_sys_t* sys = aout->sys;

    sys->volume = volume
    aout_VolumeReport(aout, volume);

    if (sys->mute == false)
        sys->player->SetVolume(volume);

    return VLC_SUCCESS;
}

/*****************************************************************************
 * MuteSet
 *****************************************************************************/
static int MuteSet(audio_output_t* aout, bool mute)
{
    aout_sys_t* sys = aout->sys;
    
    sys->mute = mute;
    aout_MuteReport(aout, mute);
    
    if (mute == false)
        sys->player->SetVolume(sys->volume);
    else
        sys->player->SetVolume(0.0);

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Open
 *****************************************************************************/
static int Open(vlc_object_t* obj)
{
    audio_output_t* aout = (audio_output_t*)obj;
    aout_sys_t* sys = malloc(sizeof(*sys));

    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout->time_get = TimeGet;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->drain = NULL;
    aout->volume_set = VolumeSet;
    aout->mute_set = MuteSet;
    aout->device_select = NULL;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close
 *****************************************************************************/
static void Close(vlc_object_t* obj)
{
    audio_output_t* aout = (audio_output_t*)obj;
    aout_sys_t* sys = aout->sys;
    
    delete sys->player;
    block_BytestreamRelease(sys->buffers);
    
    free(sys);
}
