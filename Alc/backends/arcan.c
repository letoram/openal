/**
 * OpenAL cross platform audio library
 * Copyright (C) 2010 by Chris Robinson
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#ifndef PLATFORM_HEADER
#define PLATFORM_HEADER "../../../platform/platform.h"
#endif

#include <arcan_shmif.h>
#include "alMain.h"
#include "alu.h"
#include "threads.h"
#include "compat.h"

#include "backends/base.h"

typedef struct ALCarcanBackend {
    DERIVE_FROM_TYPE(ALCbackend);

    volatile int killNow;
    althrd_t thread;
} ALCarcanBackend;

static int ALCarcanBackend_mixerProc(void *ptr);

static void ALCarcanBackend_Construct(ALCarcanBackend *self, ALCdevice *device);
static DECLARE_FORWARD(ALCarcanBackend, ALCbackend, void, Destruct)
static ALCenum ALCarcanBackend_open(ALCarcanBackend *self, const ALCchar *name);
static void ALCarcanBackend_close(ALCarcanBackend *self);
static ALCboolean ALCarcanBackend_reset(ALCarcanBackend *self);
static ALCboolean ALCarcanBackend_start(ALCarcanBackend *self);
static void ALCarcanBackend_stop(ALCarcanBackend *self);
static DECLARE_FORWARD2(ALCarcanBackend, ALCbackend, ALCenum, captureSamples, void*, ALCuint)
static DECLARE_FORWARD(ALCarcanBackend, ALCbackend, ALCuint, availableSamples)
static DECLARE_FORWARD(ALCarcanBackend, ALCbackend, ClockLatency, getClockLatency)
static DECLARE_FORWARD(ALCarcanBackend, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCarcanBackend, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCarcanBackend)

DEFINE_ALCBACKEND_VTABLE(ALCarcanBackend);


static const ALCchar arcanDevice[] = "arcan";


static void ALCarcanBackend_Construct(ALCarcanBackend *self, ALCdevice *device)
{
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCarcanBackend, ALCbackend, self);
}

/*
 * for synch with an arcan client running on a video thread
 */
struct primary_udata {
    uint64_t magic;
    uint8_t resize_pending;
};

static int ALCarcanBackend_mixerProc(void *ptr)
{
    ALCarcanBackend *self = (ALCarcanBackend*)ptr;
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    struct timespec now, start;
    ALuint64 avail, done;
    const long restTime = (long)((ALuint64)device->UpdateSize * 1000000000 /
                                 device->Frequency / 2);

    struct arcan_shmif_cont* acon = arcan_shmif_primary(SHMIF_INPUT);
    if (!acon)
        return 1;

    SetRTPriority();
    althrd_setname(althrd_current(), MIXER_THREAD_NAME);

    done = 0;
    if(altimespec_get(&start, AL_TIME_UTC) != AL_TIME_UTC)
    {
        ERR("Failed to get starting time\n");
        return 1;
    }

    if (!arcan_shmif_lock(acon)){
        ERR("Failed to retrieve lock\n");
        return 1;
    }
/* this is slightly dangerous as there might be a race with the video subsystem,
 * though practically we can still get away with it due to the layout of shmif */
    size_t frame_sz = FrameSizeFromDevFmt(device->FmtChans, device->FmtType);
    if (frame_sz * device->UpdateSize != acon->abufsize){
        arcan_shmif_resize_ext(acon, acon->w, acon->h,
            (struct shmif_resize_ext){
            .abuf_sz = frame_sz * device->UpdateSize, .abuf_cnt = 4
        });
    }
    arcan_shmif_unlock(acon);

    if (frame_sz * device->UpdateSize > acon->abufsize){
        ERR("Alsa/arcan - couldn't negotiate the desired buffer size");
        return 1;
    }

		struct {
			uint64_t magic;
			volatile uint8_t resize_pending;
		}* sstruct = acon->user;

/* it's not entirely safe to do this here */
    while(!self->killNow && device->Connected)
    {
        if(altimespec_get(&now, AL_TIME_UTC) != AL_TIME_UTC)
        {
            ERR("Failed to get current time\n");
            return 1;
        }

        avail  = (now.tv_sec - start.tv_sec) * device->Frequency;
        avail += (ALint64)(now.tv_nsec - start.tv_nsec) * device->Frequency / 1000000000;
        if(avail < done)
        {
            /* Oops, time skipped backwards. reset the number of samples done
             * with one update available since we (likely) just came back from
             * sleeping. */
            done = avail - device->UpdateSize;
        }

        if(avail-done < device->UpdateSize)
            al_nssleep(restTime);
        else while(avail-done >= device->UpdateSize)
        {

 /* There is a problematic portion here in that a pending resize call would
  * easily be starved here since this thread is usually created with high
  * priority, leading to the chance of the audio-drain rate saturating video
  * locking. The shmif_lock doesn't consider priority, so our best chances are
  * to have a separate tag for this with a known 4b magic for a side channel */
						if (sstruct && sstruct->magic == 0xfeedface){
							while(sstruct->resize_pending)
								sched_yield();
						}

            if (arcan_shmif_lock(acon)){
                aluMixData(device, acon->audb, device->UpdateSize);
                acon->abufused += device->UpdateSize * frame_sz;
                done += device->UpdateSize;
                arcan_shmif_signal(acon, SHMIF_SIGAUD | SHMIF_SIGBLK_NONE);
                arcan_shmif_unlock(acon);
            }
        }
    }

    return 0;
}


static ALCenum ALCarcanBackend_open(ALCarcanBackend *self, const ALCchar *name)
{
    ALCdevice *device;

    if(!name)
        name = arcanDevice;
    else if(strcmp(name, arcanDevice) != 0)
        return ALC_INVALID_VALUE;

    device = STATIC_CAST(ALCbackend, self)->mDevice;
    al_string_copy_cstr(&device->DeviceName, name);

    return ALC_NO_ERROR;
}

static void ALCarcanBackend_close(ALCarcanBackend* UNUSED(self))
{
}

static ALCboolean ALCarcanBackend_reset(ALCarcanBackend *self)
{
    ALCdevice *dev = STATIC_CAST(ALCbackend, self)->mDevice;
    struct arcan_shmif_cont* cont = arcan_shmif_primary(SHMIF_INPUT);
    if (!cont)
        return ALC_FALSE;

    dev->FmtChans = ALC_STEREO_SOFT;
    dev->FmtType = ALC_SHORT_SOFT;
    dev->Frequency = ARCAN_SHMIF_SAMPLERATE;

    cont->abufpos = 0;
    arcan_shmif_enqueue(cont, &(struct arcan_event){
        .ext.kind = ARCAN_EVENT(FLUSHAUD)
    });

    SetDefaultWFXChannelOrder(dev);

    return ALC_TRUE;
}

static ALCboolean ALCarcanBackend_start(ALCarcanBackend *self)
{
    self->killNow = 0;
    if(althrd_create(&self->thread, ALCarcanBackend_mixerProc, self) != althrd_success)
        return ALC_FALSE;
    return ALC_TRUE;
}

static void ALCarcanBackend_stop(ALCarcanBackend *self)
{
    int res;

    if(self->killNow)
        return;

    self->killNow = 1;
    althrd_join(self->thread, &res);
}


typedef struct ALCarcanBackendFactory {
    DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCarcanBackendFactory;
#define ALCARCANBACKENDFACTORY_INITIALIZER { { GET_VTABLE2(ALCarcanBackendFactory, ALCbackendFactory) } }

ALCbackendFactory *ALCarcanBackendFactory_getFactory(void);

static ALCboolean ALCarcanBackendFactory_init(ALCarcanBackendFactory *self);
static DECLARE_FORWARD(ALCarcanBackendFactory, ALCbackendFactory, void, deinit)
static ALCboolean ALCarcanBackendFactory_querySupport(ALCarcanBackendFactory *self, ALCbackend_Type type);
static void ALCarcanBackendFactory_probe(ALCarcanBackendFactory *self, enum DevProbe type);
static ALCbackend* ALCarcanBackendFactory_createBackend(ALCarcanBackendFactory *self, ALCdevice *device, ALCbackend_Type type);
DEFINE_ALCBACKENDFACTORY_VTABLE(ALCarcanBackendFactory);


ALCbackendFactory *ALCarcanBackendFactory_getFactory(void)
{
    static ALCarcanBackendFactory factory = ALCARCANBACKENDFACTORY_INITIALIZER;
    return STATIC_CAST(ALCbackendFactory, &factory);
}


static ALCboolean ALCarcanBackendFactory_init(ALCarcanBackendFactory* UNUSED(self))
{
    return ALC_TRUE;
}

static ALCboolean ALCarcanBackendFactory_querySupport(ALCarcanBackendFactory* UNUSED(self), ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
        return ALC_TRUE;
    return ALC_FALSE;
}

static void ALCarcanBackendFactory_probe(ALCarcanBackendFactory* UNUSED(self), enum DevProbe type)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
            AppendAllDevicesList(arcanDevice);
            break;
        case CAPTURE_DEVICE_PROBE:
            break;
    }
}

static ALCbackend* ALCarcanBackendFactory_createBackend(ALCarcanBackendFactory* UNUSED(self), ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCarcanBackend *backend;
        NEW_OBJ(backend, ALCarcanBackend)(device);
        if(!backend) return NULL;
        return STATIC_CAST(ALCbackend, backend);
    }

    return NULL;
}
