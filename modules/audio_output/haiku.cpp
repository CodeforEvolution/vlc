

/*****************************************************************************
 * OpenAudio
 *****************************************************************************/
int OpenAudio ( vlc_object_t * p_this )
{
    aout_instance_t * p_aout = (aout_instance_t*) p_this;
    p_aout->output.p_sys = (aout_sys_t*) malloc( sizeof( aout_sys_t ) );
    if( p_aout->output.p_sys == NULL )
        return -1;

    aout_sys_t * p_sys = p_aout->output.p_sys;

    aout_VolumeSoftInit( p_aout );

    int i_nb_channels = aout_FormatNbChannels( &p_aout->output.output );
    /* BSoundPlayer does not support more than 2 channels AFAIK */
    if( i_nb_channels > 2 )
    {
        i_nb_channels = 2;
        p_aout->output.output.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
    }

    media_raw_audio_format * p_format;
    p_format = (media_raw_audio_format*)
        malloc( sizeof( media_raw_audio_format ) );

    p_format->channel_count = i_nb_channels;
    p_format->frame_rate = p_aout->output.output.i_rate;
    p_format->format = media_raw_audio_format::B_AUDIO_FLOAT;
#ifdef WORDS_BIGENDIAN
    p_format->byte_order = B_MEDIA_BIG_ENDIAN;
#else
    p_format->byte_order = B_MEDIA_LITTLE_ENDIAN;
#endif
    p_format->buffer_size = 8192;

    p_aout->output.output.i_format = VLC_FOURCC('f','l','3','2');
    p_aout->output.i_nb_samples = 2048 / i_nb_channels;
    p_aout->output.pf_play = DoNothing;

    p_sys->p_player = new BSoundPlayer( p_format, "player", Play, NULL, p_aout );
    if( p_sys->p_player->InitCheck() != B_OK )
    {
        msg_Err( p_aout, "BSoundPlayer InitCheck failed" );
        delete p_sys->p_player;
        free( p_sys );
        return -1;
    }

    /* Start playing */
    p_sys->latency = p_sys->p_player->Latency();
    p_sys->p_player->Start();
    p_sys->p_player->SetHasData(true);

    return 0;
}

/*****************************************************************************
 * CloseAudio
 *****************************************************************************/
void CloseAudio ( vlc_object_t * p_this )
{
    aout_instance_t * p_aout = (aout_instance_t *) p_this;
    aout_sys_t * p_sys = (aout_sys_t *) p_aout->output.p_sys;

    /* Clean up */
    p_sys->p_player->Stop();
    delete p_sys->p_player;
    free(p_sys);
}

/*****************************************************************************
 * Play
 *****************************************************************************/
static void Play( void * _p_aout, void * _p_buffer, size_t i_size,
                  const media_raw_audio_format &format )
{
    aout_instance_t * p_aout = (aout_instance_t*) _p_aout;
    float * p_buffer = (float*) _p_buffer;
    aout_sys_t * p_sys = (aout_sys_t*) p_aout->output.p_sys;
    aout_buffer_t * p_aout_buffer;

    p_aout_buffer = aout_OutputNextBuffer( p_aout,
                                           mdate() + p_sys->latency,
                                           false );

    if( p_aout_buffer != NULL )
    {
        vlc_memcpy( p_buffer, p_aout_buffer->p_buffer,
                    MIN( i_size, p_aout_buffer->i_nb_bytes ) );
        if( p_aout_buffer->i_nb_bytes < i_size )
        {
            vlc_memset(  p_buffer + p_aout_buffer->i_nb_bytes,
                         0, i_size - p_aout_buffer->i_nb_bytes );
        }
        aout_BufferFree( p_aout_buffer );
    }
    else
    {
        vlc_memset( p_buffer, 0, i_size );
    }
}

// New stuff - Don't worry, a squash commit will make sure this comment never existed

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
#include <vlc_cpu.h>

#include <media/MediaDefs.h>
#include <media/SoundPlayer.h>

/*****************************************************************************
 * aout_sys_t: Haiku audio output method descriptor
 *****************************************************************************/
typedef struct aout_sys_t
{
    BSoundPlayer*  player;
    block_t*       data;
    bool           mute;
    float          volume;
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
static void Drain       (audio_output_t* aout);
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
        default:
            assert(AOUT_FMT_LINEAR(fmt));
            assert(aout_FormatNbChannels(fmt) > 0);
            fmt->i_format = HAVE_FPU ? VLC_CODEC_FL32 : VLC_CODEC_S16N;
            fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
            break;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Stop
 *****************************************************************************/
static void Stop(audio_output_t* aout)
{
    aout_sys_t* sys = aout->sys;
    
    sys->player->SetHasData(false);
    sys->player->Stop();
    
    block_Release(data);
}

/*****************************************************************************
 * TimeGet
 *****************************************************************************/
static void TimeGet(audio_output_t* aout, vlc_tick_t* delay)
{
    aout_sys_t* sys = aout->sys;
    aout_sample_format_t* format = &sys->format;
    
    *delay = sys->player->Latency() + vlc_ticks_from_samples(, format->i_rate);
}

/*****************************************************************************
 * Play
 *****************************************************************************/
static void Play(audio_output_t* aout, block_t* block, vlc_tick_t date)
{
    aout_sys_t* sys = aout->sys;
    
    sys->data = block;
    sys->player->SetHasData(true);
    
    (void)date;
}

/*****************************************************************************
 * PlayBuffer
 *****************************************************************************/
static void PlayBuffer(void* aout, void* buffer, size_t size,
                          const media_raw_audio_format &format)
{
    audio_output_t* aout = (audio_output_t*)aout;
    aout_sys_t* sys = aout->sys;

    aout_buffer = aout_OutputNextBuffer(aout, mdate() + sys->latency, false);
    
    if (aout_buffer != NULL)
    {
        vlc_memcpy(buffer, aout_buffer->buffer,
            MIN(size, aout_buffer->nb_bytes));
        if(aout_buffer->nb_bytes < size)
        {
            vlc_memset(buffer + aout_buffer->nb_bytes,
                0, size - aout_buffer->nb_bytes);
        }
        aout_BufferFree(aout_buffer);
    }
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
    block_Release(sys->data);
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
    aout->time_get = aout_TimeGetDefault;
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
    aout->stop(aout);
    
    delete aout->sys->player;
}
