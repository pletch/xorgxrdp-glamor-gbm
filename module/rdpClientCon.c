/*
Copyright 2005-2017 Jay Sorg

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Client connection to xrdp

*/

#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* this should be before all X11 .h files */
#include <xorg-server.h>
#include <xorgVersion.h>

/* all driver need this */
#include <xf86.h>
#include <xf86_OSproc.h>

#include "rdp.h"
#include "rdpDraw.h"
#include "rdpClientCon.h"
#include "rdpMisc.h"
#include "rdpInput.h"
#include "rdpReg.h"
#include "rdpCapture.h"

#if defined(XORGXRDP_LRANDR)
#include "rdpRandR.h"
#include "rdpLRandR.h"
#include "rdpRandRGrid.h"
#else
#include "rdpRandR.h"
#include "rdpRandRGrid.h"
#endif

#define LTOUI32(_in) ((unsigned int)(_in))

#define USE_MAX_OS_BYTES 1
#define MAX_OS_BYTES (16 * 1024 * 1024)

#define MIN_MS_TO_WAIT_FOR_MORE_UPDATES 4

/*
0 GXclear,        0
1 GXnor,          DPon
2 GXandInverted,  DPna
3 GXcopyInverted, Pn
4 GXandReverse,   PDna
5 GXinvert,       Dn
6 GXxor,          DPx
7 GXnand,         DPan
8 GXand,          DPa
9 GXequiv,        DPxn
a GXnoop,         D
b GXorInverted,   DPno
c GXcopy,         P
d GXorReverse,    PDno
e GXor,           DPo
f GXset           1
*/

static int g_rdp_opcodes[16] =
{
    0x00, /* GXclear        0x0 0 */
    0x88, /* GXand          0x1 src AND dst */
    0x44, /* GXandReverse   0x2 src AND NOT dst */
    0xcc, /* GXcopy         0x3 src */
    0x22, /* GXandInverted  0x4 NOT src AND dst */
    0xaa, /* GXnoop         0x5 dst */
    0x66, /* GXxor          0x6 src XOR dst */
    0xee, /* GXor           0x7 src OR dst */
    0x11, /* GXnor          0x8 NOT src AND NOT dst */
    0x99, /* GXequiv        0x9 NOT src XOR dst */
    0x55, /* GXinvert       0xa NOT dst */
    0xdd, /* GXorReverse    0xb src OR NOT dst */
    0x33, /* GXcopyInverted 0xc NOT src */
    0xbb, /* GXorInverted   0xd NOT src OR dst */
    0x77, /* GXnand         0xe NOT src OR NOT dst */
    0xff  /* GXset          0xf 1 */
};

static int
rdpClientConDisconnect(rdpPtr dev, rdpClientCon *clientCon);
static CARD32
rdpDeferredIdleDisconnectCallback(OsTimerPtr timer, CARD32 now, pointer arg);
static void
rdpScheduleDeferredUpdate(rdpClientCon *clientCon);
static void
rdpClientConProcessClientInfoMonitors(rdpPtr dev, rdpClientCon *clientCon);
static int
rdpSendMemoryAllocationComplete(rdpPtr dev, rdpClientCon *clientCon);
static int
rdpSendAccelAssistMonitors(rdpPtr dev, rdpClientCon *clientCon);

#if XORG_VERSION_CURRENT < XORG_VERSION_NUMERIC(1, 18, 5, 0, 0)

/******************************************************************************/
static int
rdpClientConAddEnabledDevice(ScreenPtr pScreen, int fd)
{
    AddEnabledDevice(fd);
    return 0;
}

/******************************************************************************/
static int
rdpClientConRemoveEnabledDevice(int fd)
{
    RemoveEnabledDevice(fd);
    return 0;
}

#else

/******************************************************************************/
static void
rdpClientConNotifyFdProcPtr(int fd, int ready, void *data)
{
    ScreenPtr pScreen = (ScreenPtr) data;
    rdpClientConCheck(pScreen);
}

/******************************************************************************/
static int
rdpClientConAddEnabledDevice(ScreenPtr pScreen, int fd)
{
    SetNotifyFd(fd, rdpClientConNotifyFdProcPtr, X_NOTIFY_READ, pScreen);
    return 0;
}

/******************************************************************************/
static int
rdpClientConRemoveEnabledDevice(int fd)
{
    RemoveNotifyFd(fd);
    return 0;
}

#endif

/******************************************************************************/
static void
rdpAddClientConToDev(rdpPtr dev, rdpClientCon *clientCon)
{
    clientCon->next = NULL;
    clientCon->prev = dev->clientConTail;

    if (dev->clientConTail == NULL)
    {
        LOG(LOG_LEVEL_INFO, "rdpAddClientConToDev: adding first clientCon %p",
            clientCon);
        dev->clientConHead = clientCon;
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "rdpAddClientConToDev: adding clientCon %p",
            clientCon);
        dev->clientConTail->next = clientCon;
    }
    dev->clientConTail = clientCon;
}

/******************************************************************************/
static void
rdpRemoveClientConFromDev(rdpPtr dev, rdpClientCon *clientCon)
{
    LOG(LOG_LEVEL_INFO, "rdpRemoveClientConFromDev: removing clientCon %p",
        clientCon);

    if (clientCon->prev == NULL)
    {
        /* first in list */
        dev->clientConHead = clientCon->next;
    }
    else
    {
        clientCon->prev->next = clientCon->next;
    }

    if (clientCon->next == NULL)
    {
        /* last in list */
        dev->clientConTail = clientCon->prev;
    }
    else
    {
        clientCon->next->prev = clientCon->prev;
    }
}

/******************************************************************************/
static int
rdpClientConGotConnection(ScreenPtr pScreen, rdpPtr dev)
{
    rdpClientCon *clientCon;
    int new_sck;

    LOG(LOG_LEVEL_TRACE, "rdpClientConGotConnection:");
    clientCon = g_new0(rdpClientCon, 1);
    clientCon->shmemstatus = SHM_UNINITIALIZED;
    clientCon->updateRetries = 0;
    clientCon->dev = dev;
    clientCon->shmemfd = -1;

    dev->last_event_time_ms = GetTimeInMillis();
    dev->do_dirty_ons = 1;

    make_stream(clientCon->in_s);
    init_stream(clientCon->in_s, 8192);
    make_stream(clientCon->out_s);
    init_stream(clientCon->out_s, 8192 * 4 + 100);

    new_sck = g_sck_accept(dev->listen_sck);
    if (new_sck == -1)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: g_sck_accept failed");
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: g_sck_accept ok new_sck %d",
            new_sck);
        clientCon->sck = new_sck;
        g_sck_set_non_blocking(clientCon->sck);
        g_sck_tcp_set_no_delay(clientCon->sck); /* only works if TCP */
        clientCon->connected = TRUE;
        clientCon->begin = FALSE;
        dev->conNumber++;
        clientCon->conNumber = dev->conNumber;
        rdpClientConAddEnabledDevice(pScreen, clientCon->sck);
    }

#if 1
    if (dev->clientConTail != NULL)
    {
        /* Only allow one client at a time */
        LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: "
            "marking only clientCon %p for disconnect",
            dev->clientConTail);
        dev->clientConTail->connected = FALSE;
    }
#endif

    /* reset disconnectTimer if reconnection */
    if (dev->disconnect_scheduled)
    {
        if (dev->disconnectTimer != NULL)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConGotConnection: disengaging disconnect timer");
            TimerCancel(dev->disconnectTimer);
            TimerFree(dev->disconnectTimer);
            dev->disconnectTimer = NULL;
        }
        dev->disconnect_scheduled = FALSE;
    }

    /* set idle timer to disconnect */
    if (dev->idle_disconnect_timeout_s > 0)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: "
            "engaging idle timer, timeout [%d] sec",
            dev->idle_disconnect_timeout_s);
        dev->idleDisconnectTimer = TimerSet(dev->idleDisconnectTimer, 0, dev->idle_disconnect_timeout_s * 1000,
                                            rdpDeferredIdleDisconnectCallback, dev);
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: "
            "idle_disconnect_timeout set to non-positive value, "
            "idle timer turned off");
    }

    rdpAddClientConToDev(dev, clientCon);

    clientCon->dirtyRegion = rdpRegionCreate(NullBox, 0);
    clientCon->shmRegion = rdpRegionCreate(NullBox, 0);

    return 0;
}

/******************************************************************************/
static CARD32
rdpDeferredDisconnectCallback(OsTimerPtr timer, CARD32 now, pointer arg)
{
    (void) timer;
    (void) now;
    (void) arg;

    /* time's up, kill the session */
    LOG(LOG_LEVEL_INFO,
        "rdpDeferredDisconnectCallback: disconnect timeout exceeded, exiting");
    kill(getpid(), SIGTERM);

    /* not reached */
    return 0;
}

/*****************************************************************************/
static CARD32
rdpDeferredIdleDisconnectCallback(OsTimerPtr timer, CARD32 now, pointer arg)
{
    LOG(LOG_LEVEL_TRACE, "rdpDeferredIdleDisconnectCallback:");

    rdpPtr dev;

    dev = (rdpPtr) arg;

    CARD32 millis_since_last_event;

    /* how many millis was the last event ago? */
    millis_since_last_event = now - dev->last_event_time_ms;

    /* we MUST compare to equal otherwise we could restart the idle timer with 0! */
    if (millis_since_last_event >= (dev->idle_disconnect_timeout_s * 1000))
    {
        LOG(LOG_LEVEL_INFO, "rdpDeferredIdleDisconnectCallback: "
            "session has been idle for %d seconds, disconnecting",
            dev->idle_disconnect_timeout_s);

        /* disconnect all clients */
        while (dev->clientConHead != NULL)
        {
            rdpClientConDisconnect(dev, dev->clientConHead);
        }

        LOG(LOG_LEVEL_INFO, "rdpDeferredIdleDisconnectCallback: "
            "disconnected idle session");

        TimerCancel(dev->idleDisconnectTimer);
        TimerFree(dev->idleDisconnectTimer);
        dev->idleDisconnectTimer = NULL;
        LOG(LOG_LEVEL_INFO, "rdpDeferredIdleDisconnectCallback: "
            "idle timer disengaged");
        return 0;
    }

    /* restart the idle timer with last_event + idle timeout */
    dev->idleDisconnectTimer = TimerSet(dev->idleDisconnectTimer, 0, (dev->idle_disconnect_timeout_s * 1000) - millis_since_last_event,
                                        rdpDeferredIdleDisconnectCallback, dev);
    return 0;
}

/*****************************************************************************/
static int
rdpShutdownAccelAssist(rdpPtr dev, rdpClientCon *clientCon) {
    ScreenPtr pScreen;
    PixmapPtr pPixmap;
    int index;
    int exit_code = 0;

    LOG(LOG_LEVEL_TRACE, "rdpShutdownAccelAssist:");
    if (clientCon->accel_assist_pid <= 0)
    {
        return 0;
    }
    if (waitpid(clientCon->accel_assist_pid, &exit_code, WNOHANG) == 0)
    {
        /* still running */
        kill(clientCon->accel_assist_pid, SIGTERM);
        waitpid(clientCon->accel_assist_pid, &exit_code, 0);
    }
    pScreen = clientCon->dev->pScreen;
    for (index = 0; index < 16; index++)
    {
        int buf;

        for (buf = 0; buf < 2; buf++)
        {
            pPixmap = clientCon->accelAssistPixmaps[index][buf];
            if (pPixmap != NULL)
            {
                pScreen->DestroyPixmap(pPixmap);
                clientCon->accelAssistPixmaps[index][buf] = NULL;
            }
            if (clientCon->accelAssistPending[index][buf] != NULL)
            {
                rdpRegionDestroy(clientCon->accelAssistPending[index][buf]);
                clientCon->accelAssistPending[index][buf] = NULL;
            }
        }
    }
    clientCon->accel_assist_pid = -1;
    clientCon->use_accel_assist = 0;
    return exit_code;
}

/******************************************************************************/
/* Is AVC444 in effect for this connection?

   Normally this is whatever the client negotiated, which xrdp worked out
   from the confirmed EGFX capability set and passed down in
   client_info.gfx_avc444. XRDP_ACCEL_AVC444 overrides it, tri-state:

     unset  follow what the client negotiated
     "0"    force AVC444 off, whatever the client supports
     other  force AVC444 on

   The accel-assist helper applies the same rule to the same variable, so
   the codec id asked for here and the encoder the helper builds cannot
   disagree. An off switch has to exist now that negotiation can turn
   AVC444 on by itself, and it cannot be a presence test, or "=0" -- the
   obvious spelling -- would turn it on. */
static Bool
rdpClientConAvc444(rdpClientCon *clientCon)
{
    const char *env = getenv("XRDP_ACCEL_AVC444");

    if (env != NULL)
    {
        return strcmp(env, "0") != 0;
    }
    return clientCon->client_info.gfx_avc444 != 0;
}

/******************************************************************************/
/* Is the v2 chroma layout in effect?

   client_info.gfx_avc444 carries the level xrdp derived from the confirmed
   capability set: 2 means the v2 layout, 1 means hold to v1. There is no
   capability flag for this in MS-RDPEGFX, so the version is all xrdp has to
   go on and the choice is finally the server's. XRDP_ACCEL_AVC444_V2
   overrides, tri-state like XRDP_ACCEL_AVC444 above. */
static Bool
rdpClientConAvc444V2(rdpClientCon *clientCon)
{
    const char *env = getenv("XRDP_ACCEL_AVC444_V2");

    if (env != NULL)
    {
        return strcmp(env, "0") != 0;
    }
    /* An xrdp that predates the level-carrying gfx_avc444 sets it to 1 for
       any AVC444-capable client, so treating "not 2" as "v1" silently
       downgrades a whole session on a mixed build. Only hold to v1 when the
       peer actually said 1 while knowing about levels; a plain 1 from an
       older peer keeps the previous default, which was v2. */
    if (clientCon->client_info.version >= XUP_CLIENT_INFO_LEVEL_AVC444_VERSION)
    {
        return clientCon->client_info.gfx_avc444 >= 2;
    }
    return clientCon->client_info.gfx_avc444 != 0;
}

/******************************************************************************/
static Bool
rdpClientConUseAccelAssist(rdpPtr dev, rdpClientCon *clientCon)
{
    const char *xrdp_use_accel_assist = getenv("XRDP_USE_ACCEL_ASSIST");

    if (xrdp_use_accel_assist == NULL)
    {
        return FALSE;
    }
    if (strcmp(xrdp_use_accel_assist, "0") == 0)
    {
        return FALSE;
    }
    if (strcmp(xrdp_use_accel_assist, "1") == 0)
    {
        return ((dev->nvidia || dev->glamor) &&
                ((clientCon->client_info.capture_code == CC_SUF_A2) ||
                 (clientCon->client_info.capture_code == CC_GFX_A2)));
    }
    return 0;
}

/*****************************************************************************/
static int
rdpClientConDisconnect(rdpPtr dev, rdpClientCon *clientCon)
{
    int index;

    LOG(LOG_LEVEL_TRACE, "rdpClientConDisconnect:");

    if (dev->idleDisconnectTimer != NULL && dev->idle_disconnect_timeout_s > 0)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConDisconnect: "
            "disconnected, idle timer disengaged");
        TimerCancel(dev->idleDisconnectTimer);
        TimerFree(dev->idleDisconnectTimer);
        dev->idleDisconnectTimer = NULL;
    }

    rdpClientConRemoveEnabledDevice(clientCon->sck);
    g_sck_close(clientCon->sck);
    if (clientCon->maxOsBitmaps > 0)
    {
        for (index = 0; index < clientCon->maxOsBitmaps; index++)
        {
            if (clientCon->osBitmaps[index].used)
            {
                if (clientCon->osBitmaps[index].priv != NULL)
                {
                    clientCon->osBitmaps[index].priv->status = 0;
                }
            }
        }
    }
    free(clientCon->osBitmaps);

    rdpRemoveClientConFromDev(dev, clientCon);

    rdpRegionDestroy(clientCon->dirtyRegion);
    rdpRegionDestroy(clientCon->shmRegion);
    if (clientCon->updateTimer != NULL)
    {
        TimerCancel(clientCon->updateTimer);
        TimerFree(clientCon->updateTimer);
    }
    free_stream(clientCon->out_s);
    free_stream(clientCon->in_s);
    if (clientCon->shmemptr != NULL)
    {
        g_free_unmap_fd(clientCon->shmemptr,
                        clientCon->shmemfd,
                        clientCon->shmem_bytes);
    }
    if (clientCon->use_accel_assist)
    {
        rdpShutdownAccelAssist(dev, clientCon);
    }
    free(clientCon);

    /* kill session after disconnect ? */
    if (dev->do_kill_disconnected)
    {
        /* kill immediate or after timeout? */
        if (dev->disconnect_timeout_s == 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConDisconnect: disconnected, immediate session kill");
            kill(getpid(), SIGTERM);
        }
        else if (dev->disconnect_scheduled == FALSE)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConDisconnect: "
                "engaging disconnect timer, exit after %d seconds",
                dev->disconnect_timeout_s);
            dev->disconnectTimer = TimerSet(dev->disconnectTimer, 0,
                                            dev->disconnect_timeout_s * 1000,
                                            rdpDeferredDisconnectCallback, dev);
            dev->disconnect_scheduled = TRUE;
        }
    }

    return 0;
}

/*****************************************************************************/
/* returns error */
static int
rdpClientConSend(rdpPtr dev, rdpClientCon *clientCon, const char *data, int len)
{
    int sent;
    int retries = 0;

    LOG(LOG_LEVEL_TRACE, "rdpClientConSend - sending %d bytes", len);

    if (!clientCon->connected)
    {
        return 1;
    }

    while (len > 0)
    {
        sent = g_sck_send(clientCon->sck, data, len, 0);

        if (sent == -1)
        {
            if (g_sck_last_error_would_block(clientCon->sck))
            {
                // Just because we couldn't after 100 retries
                // does not mean we're disconnected.
                if (retries > 100)
                {
                    return 0;
                }
                ++retries;
                g_sleep(1);
            }
            else
            {
                LOG(LOG_LEVEL_INFO,
                    "rdpClientConSend: g_tcp_send failed(returned -1)");
                clientCon->connected = FALSE;
                return 1;
            }
        }
        else if (sent == 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConSend: g_tcp_send failed(returned zero)");
            clientCon->connected = FALSE;
            return 1;
        }
        else
        {
            data += sent;
            len -= sent;
        }
    }

    return 0;
}

/******************************************************************************/
static int
rdpClientConSendMsg(rdpPtr dev, rdpClientCon *clientCon)
{
    int len;
    int rv;
    struct stream *s;

    rv = 1;
    s = clientCon->out_s;
    if (s != NULL)
    {
        len = (int) (s->end - s->data);

        if (len > s->size)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConSendMsg: overrun error len, %d "
                "stream size %d, client count %d",
                len, s->size, clientCon->count);
        }

        s_pop_layer(s, iso_hdr);
        out_uint16_le(s, 3);
        out_uint16_le(s, clientCon->count);
        out_uint32_le(s, len - 8);
        rv = rdpClientConSend(dev, clientCon, s->data, len);
    }

    if (rv != 0)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConSendMsg: error in rdpup_send_msg");
    }

    return rv;
}

/******************************************************************************/
static int
rdpClientConSendPending(rdpPtr dev, rdpClientCon *clientCon)
{
    int rv;

    rv = 0;
    if (clientCon->connected && clientCon->begin)
    {
        out_uint16_le(clientCon->out_s, 2); /* XR_SERVER_END_UPDATE */
        out_uint16_le(clientCon->out_s, 4); /* size */
        clientCon->count++;
        s_mark_end(clientCon->out_s);
        if (rdpClientConSendMsg(dev, clientCon) != 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConSendPending: rdpClientConSendMsg failed");
            rv = 1;
        }
    }
    clientCon->count = 0;
    clientCon->begin = FALSE;
    return rv;
}

/******************************************************************************/
/* returns error */
static int
rdpClientConRecv(rdpPtr dev, rdpClientCon *clientCon, char *data, int len)
{
    int rcvd;

    if (!clientCon->connected)
    {
        return 1;
    }

    while (len > 0)
    {
        rcvd = g_sck_recv(clientCon->sck, data, len, 0);

        if (rcvd == -1)
        {
            if (g_sck_last_error_would_block(clientCon->sck))
            {
                g_sleep(1);
            }
            else
            {
                LOG(LOG_LEVEL_INFO,
                    "rdpClientConRecv: g_sck_recv failed(returned -1)");
                clientCon->connected = FALSE;
                return 1;
            }
        }
        else if (rcvd == 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConRecv: g_sck_recv failed(returned 0)");
            clientCon->connected = FALSE;
            return 1;
        }
        else
        {
            data += rcvd;
            len -= rcvd;
        }
    }

    return 0;
}

/******************************************************************************/
static int
rdpClientConRecvMsg(rdpPtr dev, rdpClientCon *clientCon)
{
    int len;
    int rv;
    struct stream *s;

    rv = 1;

    s = clientCon->in_s;
    if (s != 0)
    {
        init_stream(s, 4);
        rv = rdpClientConRecv(dev, clientCon, s->data, 4);

        if (rv == 0)
        {
            s->end = s->data + 4;
            in_uint32_le(s, len);

            if (len > 3)
            {
                init_stream(s, len);
                rv = rdpClientConRecv(dev, clientCon, s->data, len - 4);
                if (rv == 0)
                {
                    s->end = s->data + len;
                }
            }
        }
    }

    if (rv != 0)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConRecvMsg: error");
    }

    return rv;
}

/******************************************************************************/
static int
rdpClientConSendCaps(rdpPtr dev, rdpClientCon *clientCon)
{
    struct stream *ls;
    int len;
    int rv;
    int cap_count;

    make_stream(ls);
    init_stream(ls, 8192);
    s_push_layer(ls, iso_hdr, 8);

    cap_count = 0;

#if 0
    out_uint16_le(ls, 0);
    out_uint16_le(ls, 4);
    cap_count++;

    out_uint16_le(ls, 1);
    out_uint16_le(ls, 4);
    cap_count++;
#endif

    out_uint16_le(ls, 100);   /* Version capability */
    out_uint16_le(ls, 2 + 2 + 4);
    out_uint32_le(ls, XUP_CLIENT_INFO_CURRENT_VERSION);
    cap_count++;

    s_mark_end(ls);
    len = (int)(ls->end - ls->data);
    s_pop_layer(ls, iso_hdr);
    out_uint16_le(ls, 2); /* caps */
    out_uint16_le(ls, cap_count); /* num caps */
    out_uint32_le(ls, len - 8); /* caps len after header */

    rv = rdpClientConSend(dev, clientCon, ls->data, len);

    if (rv != 0)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConSendCaps: rdpup_send failed");
    }

    free_stream(ls);
    return rv;
}

/******************************************************************************/
static int
rdpClientConProcessMsgVersion(rdpPtr dev, rdpClientCon *clientCon,
                              int param1, int param2, int param3, int param4)
{
    LOG(LOG_LEVEL_INFO, "rdpClientConProcessMsgVersion: version %d %d %d %d",
        param1, param2, param3, param4);

    if ((param1 > 0) || (param2 > 0) || (param3 > 0) || (param4 > 0))
    {
        rdpClientConSendCaps(dev, clientCon);
    }

    return 0;
}

/**************************************************************************//**
 * Allocate shared memory
 *
 * This memory is shared with the xup driver in xrdp which avoids a lot
 * of unnecessary copying
 *
 * @param clientCon Client connection
 * @param bytes Size of area to attach
 */
static void
rdpClientConAllocateSharedMemory(rdpClientCon *clientCon, int bytes)
{
    void *shmemptr;
    int shmemfd;

    if (clientCon->shmemptr != NULL && clientCon->shmem_bytes == bytes)
    {
        LOG(LOG_LEVEL_INFO,
            "rdpClientConAllocateSharedMemory: reusing shmemfd %d",
            clientCon->shmemfd);
        return;
    }
    if (clientCon->shmemptr != NULL)
    {
        g_free_unmap_fd(clientCon->shmemptr,
                        clientCon->shmemfd,
                        clientCon->shmem_bytes);
        clientCon->shmemptr = NULL;
        clientCon->shmemfd = -1;
        clientCon->shmem_bytes = 0;
    }
    if (g_alloc_shm_map_fd(&shmemptr, &shmemfd, bytes) != 0)
    {
        FatalError("rdpClientConAllocateSharedMemory:"
                   " g_alloc_shm_map_fd failed");
    }
    clientCon->shmemptr = shmemptr;
    clientCon->shmemfd = shmemfd;
    clientCon->shmem_bytes = bytes;
    LOG(LOG_LEVEL_INFO,
        "rdpClientConAllocateSharedMemory: shmemfd %d shmemptr %p "
        "bytes %d",
        clientCon->shmemfd, clientCon->shmemptr,
        clientCon->shmem_bytes);
}

/******************************************************************************/
static enum shared_memory_status
convertSharedMemoryStatusToActive(enum shared_memory_status status)
{
    switch (status)
    {
        case SHM_ACTIVE_PENDING:
            return SHM_ACTIVE;
        case SHM_RFX_ACTIVE_PENDING:
            return SHM_RFX_ACTIVE;
        case SHM_H264_ACTIVE_PENDING:
            return SHM_H264_ACTIVE;
        default:
            return status;
    }
}

/******************************************************************************/
/**
 * Resizes all memory areas following a change in client geometry or
 * capture format.
 *
 * Call this when any of the following are changed:-
 * - clientCon->client_info.display_sizes.session_width
 * - clientCon->client_info.display_sizes.session_height
 * - clientCon->client_info.capture_code
 * - clientCon->client_info.capture_format
 *
 * All the remaining memory and capture parameters are adjusted
 */
static void
rdpClientConResizeAllMemoryAreas(rdpPtr dev, rdpClientCon *clientCon)
{
    int bytes;
    int width = clientCon->client_info.display_sizes.session_width;
    int height = clientCon->client_info.display_sizes.session_height;

    enum shared_memory_status shmemstatus;

    LOG(LOG_LEVEL_TRACE, "rdpClientConResizeAllMemoryAreas:");

    // Update the rdp size from the client size
    clientCon->rdp_width = width;
    clientCon->rdp_height = height;

    /* Set the capture parameters */
    switch(clientCon->client_info.capture_code)
    {
        case CC_SUF_RFX: /* RFX */
        case CC_GFX_PRO:
            LOG(LOG_LEVEL_INFO,
                "rdpClientConProcessMsgClientInfo: got RFX capture");
            /* RFX capture needs fixed-size rectangles */
            clientCon->cap_width = RDPALIGN(width, XRDP_RFX_ALIGN);
            clientCon->cap_height = RDPALIGN(height, XRDP_RFX_ALIGN);
            LOG(LOG_LEVEL_INFO, "  cap_width %d cap_height %d",
                clientCon->cap_width, clientCon->cap_height);

            bytes = clientCon->cap_width * clientCon->cap_height *
                    clientCon->rdp_Bpp;

            clientCon->shmem_lineBytes = clientCon->rdp_Bpp * clientCon->cap_width;
            clientCon->cap_stride_bytes = clientCon->cap_width * 4;
            shmemstatus = SHM_RFX_ACTIVE_PENDING;

            dev->msFrameInterval = clientCon->client_info.rfx_frame_interval;
            clientCon->msFrameInterval = dev->msFrameInterval;
            break;
        case CC_SUF_A2: /* H264 */
        case CC_GFX_A2:
            LOG(LOG_LEVEL_INFO,
                "rdpClientConProcessMsgClientInfo: got H264 capture");
            clientCon->cap_width = width;
            clientCon->cap_height = height;

            bytes = clientCon->cap_width * clientCon->cap_height * 2;

            clientCon->shmem_lineBytes = clientCon->rdp_Bpp * clientCon->cap_width;
            clientCon->cap_stride_bytes = clientCon->cap_width * 4;
            shmemstatus = SHM_H264_ACTIVE_PENDING;

            dev->msFrameInterval = clientCon->client_info.h264_frame_interval;
            clientCon->msFrameInterval = dev->msFrameInterval;
            break;
        default:
            LOG(LOG_LEVEL_INFO,
                "rdpClientConProcessMsgClientInfo: got normal capture");
            clientCon->cap_width = width;
            clientCon->cap_height = height;

            bytes = width * height * clientCon->rdp_Bpp;

            clientCon->shmem_lineBytes = clientCon->rdp_Bpp * clientCon->cap_width;
            clientCon->cap_stride_bytes = clientCon->cap_width * clientCon->rdp_Bpp;
            shmemstatus = SHM_ACTIVE_PENDING;

            dev->msFrameInterval = clientCon->client_info.normal_frame_interval;
            clientCon->msFrameInterval = dev->msFrameInterval;
            break;
    }

    LOG(LOG_LEVEL_INFO,
        "    msFrameInterval %ld", (long)dev->msFrameInterval);
    {
        const char *ap = getenv("XORGXRDP_ADAPTIVE_PACE");
        const char *pmin = getenv("XORGXRDP_PACE_MIN_MS");
        const char *pmax = getenv("XORGXRDP_PACE_MAX_MS");

        const char *cd = getenv("XORGXRDP_CAPTURE_DEPTH");

        /* One by default. Two lets the next frame be captured while the
           previous is still in flight; safe now that each buffer is given
           the damage it missed as well as the current frame's, so the
           full-frame AVC444 auxiliary pass never reads a stale pixel. */
        clientCon->capture_depth = (cd != NULL) ? atoi(cd) : 1;
        if (clientCon->capture_depth < 1)
        {
            clientCon->capture_depth = 1;
        }
        if (clientCon->capture_depth > 2)
        {
            clientCon->capture_depth = 2;
        }
        clientCon->pace_enabled = (ap != NULL && atoi(ap) != 0);
        /* The floor defaults to 20 ms because the RandR mode xorgxrdp
           advertises is 50 Hz (rdpRandR.c), so a vsync-aware application
           cannot paint faster than that -- probing below the source's own
           rate only spends capture attempts that find nothing changed. */
        clientCon->pace_min_ms = (pmin != NULL) ? atoi(pmin) : 20;
        clientCon->pace_max_ms = (pmax != NULL) ? atoi(pmax) : 100;
        if (clientCon->pace_min_ms < 1)
        {
            clientCon->pace_min_ms = 1;
        }
        if (clientCon->pace_max_ms < clientCon->pace_min_ms)
        {
            clientCon->pace_max_ms = clientCon->pace_min_ms;
        }
        if (clientCon->pace_enabled)
        {
            LOG(LOG_LEVEL_INFO, "    capture depth %d",
            clientCon->capture_depth);
        LOG(LOG_LEVEL_INFO, "    adaptive pacing on, interval %d-%d ms",
                clientCon->pace_min_ms, clientCon->pace_max_ms);
        }
    }
    rdpClientConAllocateSharedMemory(clientCon, bytes);

    if (clientCon->client_info.capture_format != 0)
    {
        clientCon->rdp_format = clientCon->client_info.capture_format;
        switch (clientCon->rdp_format)
        {
            case XRDP_a8r8g8b8:
            case XRDP_a8b8g8r8:
                clientCon->cap_stride_bytes = clientCon->cap_width * 4;
                break;
            case XRDP_r5g6b5:
            case XRDP_a1r5g5b5:
                clientCon->cap_stride_bytes = clientCon->cap_width * 2;
                break;
            default:
                clientCon->cap_stride_bytes = clientCon->cap_width * 1;
                break;
        }
    }
    else
    {
        int bpp = clientCon->client_info.bpp;
        if (bpp < 15)
        {
            clientCon->rdp_format = XRDP_r3g3b2;
        }
        else if (bpp == 15)
        {
            clientCon->rdp_format = XRDP_a1r5g5b5;
        }
        else if (bpp == 16)
        {
            clientCon->rdp_format = XRDP_r5g6b5;
        }
        else if (bpp > 16)
        {
            clientCon->rdp_format = XRDP_a8r8g8b8;
        }
    }

    if (clientCon->shmRegion != 0)
    {
        rdpRegionDestroy(clientCon->shmRegion);
    }
    clientCon->shmRegion = rdpRegionCreate(NullBox, 0);

    if ((dev->width != width) || (dev->height != height))
    {
        /* Set the device size, regardless of the 'allow_screen_resize'
         * setting */
        ScrnInfoPtr pScrn = xf86Screens[dev->pScreen->myNum];
        int mmwidth = PixelToMM(width, pScrn->xDpi);
        int mmheight = PixelToMM(height, pScrn->yDpi);
        int ok;
        dev->allow_screen_resize = 1;
        ok = RRScreenSizeSet(dev->pScreen, width, height, mmwidth, mmheight);
        dev->allow_screen_resize = 0;
        LOG(LOG_LEVEL_INFO,
            "rdpClientConResizeAllMemoryAreas: RRScreenSizeSet ok=[%d]", ok);
    }

    rdpCaptureResetState(clientCon);

    if (clientCon->shmemstatus == SHM_UNINITIALIZED
       || clientCon->shmemstatus == SHM_RESIZING)
    {
        clientCon->shmemstatus
            = convertSharedMemoryStatusToActive(shmemstatus);
    }
}

/******************************************************************************/
static int
rdpClientConProcessMonitorUpdateMsg(rdpPtr dev, rdpClientCon *clientCon,
                                    int width, int height, int num_monitors,
                                    struct monitor_info monitors[])
{
    int i;
    LOG(LOG_LEVEL_INFO, "rdpClientConProcessMonitorUpdateMsg: (%dx%d) #%d",
        width, height, num_monitors);

    // Update the client_info we have
    clientCon->client_info.display_sizes.monitorCount = num_monitors;
    for (i = 0; i < num_monitors; ++i)
    {
        clientCon->client_info.display_sizes.minfo[i] = monitors[i];
        clientCon->client_info.display_sizes.minfo_wm[i] = monitors[i];
    }
    clientCon->client_info.display_sizes.session_width = width;
    clientCon->client_info.display_sizes.session_height = height;

    rdpClientConResizeAllMemoryAreas(dev, clientCon);
    rdpClientConProcessClientInfoMonitors(dev, clientCon);

    /* Tell xrdp we're done */
    rdpClientConAddDirtyScreen(dev, clientCon, 0, 0, width, height);
    rdpSendMemoryAllocationComplete(dev, clientCon);

    if (clientCon->use_accel_assist)
    {
        rdpSendAccelAssistMonitors(dev, clientCon);
    }

    return 0;
}

/******************************************************************************/
static int
rdpClientConProcessMsgClientInput(rdpPtr dev, rdpClientCon *clientCon)
{
    struct stream *s;
    int msg;
    int param1;
    int param2;
    int param3;
    int param4;
    int x;
    int y;
    int cx;
    int cy;

    s = clientCon->in_s;
    in_uint32_le(s, msg);
    in_uint32_le(s, param1);
    in_uint32_le(s, param2);
    in_uint32_le(s, param3);
    in_uint32_le(s, param4);

    LOG(LOG_LEVEL_TRACE,
        "rdpClientConProcessMsgClientInput: msg %d param1 %d param2 %d "
        "param3 %d param4 %d", msg, param1, param2, param3, param4);

    if (msg < 100)
    {
        rdpInputKeyboardEvent(dev, msg, param1, param2, param3, param4);
    }
    else if (msg < 200)
    {
        rdpInputMouseEvent(dev, msg, param1, param2, param3, param4);
    }
    else if (msg == 200) /* invalidate */
    {
        x = (param1 >> 16) & 0xffff;
        y = param1 & 0xffff;
        cx = (param2 >> 16) & 0xffff;
        cy = param2 & 0xffff;
        LOG(LOG_LEVEL_INFO,
            "rdpClientConProcessMsgClientInput: invalidate x %d y %d "
            "cx %d cy %d", x, y, cx, cy);
        rdpClientConAddDirtyScreen(dev, clientCon, x, y, cx, cy);
    }
    else if (msg == 300) /* resize desktop */
    {
        LOG(LOG_LEVEL_INFO,
            "rdpClientConProcessMsgClientInput: obsolete msg %d", msg);
    }
    else if (msg == 301) /* version */
    {
        rdpClientConProcessMsgVersion(dev, clientCon,
                                      param1, param2, param3, param4);
    }
    else if (msg == 302) /* monitor update */
    {
        if (param3 > 0 && param3 < CLIENT_MONITOR_DATA_MAXIMUM_MONITORS)
        {
            struct monitor_info monitors[CLIENT_MONITOR_DATA_MAXIMUM_MONITORS];
            in_uint8a(s, monitors, param3 * sizeof(monitors[0]));

            rdpClientConProcessMonitorUpdateMsg(dev, clientCon,
                                                param1, param2, param3,
                                                monitors);
        }
        else
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConProcessMsgClientInput: bad monitor count %d",
                param3);
        }
    }
    else
    {
        LOG(LOG_LEVEL_INFO,
            "rdpClientConProcessMsgClientInput: unknown msg %d", msg);
    }

    return 0;
}

/******************************************************************************/
static int
rdpStartAccelAssist(rdpPtr dev, rdpClientCon *clientCon)
{
    char text[64];
    int spair[2];
    int index;

    // Accel assist is already running, don't attempt to initialize it again.
    if (clientCon->accel_assist_pid > 0)
    {
        return 0;
    }

    socketpair(AF_UNIX, SOCK_STREAM, 0, spair);

    clientCon->accel_assist_pid = fork();
    if (clientCon->accel_assist_pid == -1)
    {
        /* error */
        close(spair[0]);
        close(spair[1]);
    }
    else if (clientCon->accel_assist_pid == 0)
    {
        /* child */
        for (index = 0; index < 256; index++)
        {
            if ((index != clientCon->sck) && (index != spair[0]))
            {
                close(index);
            }
        }
        open("/dev/null", O_RDWR);
        open("/dev/null", O_RDWR);
        open("/dev/null", O_RDWR);
#ifdef HAS_DIX_GET_DISPLAY_NAME
        snprintf(text, 63, ":%s", dixGetDisplayName(&dev->pScreen));
#else
        snprintf(text, 63, ":%s", display);
#endif
        text[63] = 0;
        setenv("DISPLAY", text, 1);
        snprintf(text, 63, "%d", spair[0]);
        text[63] = 0;
        setenv("XORGXRDP_XORG_FD", text, 1);
        snprintf(text, 63, "%d", clientCon->sck);
        text[63] = 0;
        setenv("XORGXRDP_XRDP_FD", text, 1);
        snprintf(text, 63, "%s/xrdp-accel-assist", XRDP_LIBEXEC_PATH);
        text[63] = 0;
        execlp(text, text, "-d", (void *) 0);
        exit(0);
    }
    else
    {
        /* parent */
        LOG(LOG_LEVEL_INFO, "rdpStartAccelAssist: started accel assist pid %d",
            clientCon->accel_assist_pid);
        rdpClientConRemoveEnabledDevice(clientCon->sck);
        close(clientCon->sck);
        close(spair[0]);
        clientCon->sck = spair[1];
        g_sck_set_non_blocking(clientCon->sck);
        rdpClientConAddEnabledDevice(dev->pScreen, clientCon->sck);
    }
    return 0;
}

/******************************************************************************/
static int
rdpSendAccelAssistMonitors(rdpPtr dev, rdpClientCon *clientCon)
{
    int index;
    int len;
    int rv;
    int width;
    int height;
    int caps;
    const int layer_size = 8;

    LOG(LOG_LEVEL_INFO, "rdpSendAccelAssistMonitors: monitorCount %d",
        dev->monitorCount);
    rdpClientConSendPending(dev, clientCon);
    init_stream(clientCon->out_s, 0);
    s_push_layer(clientCon->out_s, iso_hdr, layer_size);
    out_uint16_le(clientCon->out_s, 1); /* clear monitors */
    out_uint16_le(clientCon->out_s, 4); /* size */
    clientCon->count++;

    /* Session capabilities, ahead of the pixmap creates below because the
       helper's encoder geometry depends on them: the two AVC444 views are
       pictures of one H.264 sequence and must share a 16-aligned height,
       which is fixed when the pixmap is created. An older helper skips this
       message on its size field and simply never enables AVC444.

       Sub-message ids in the type-100 batch are shared with xrdp, because
       accel-assist forwards the whole batch on after processing it and
       xrdp's lib_mod_process_message parses the same bytes. 1 is clear
       monitors, 2 is add monitor, 3 is memory allocation complete (which
       xrdp acts on -- reusing it here fed a capability word to
       server_monitor_resize_done as a width and height and crashed xrdp),
       so this is 4. Both parsers skip ids they do not know using the size
       field. */
    caps = 0;
    if (rdpClientConAvc444(clientCon))
    {
        caps |= XH_CAPS_AVC444;
        if (rdpClientConAvc444V2(clientCon))
        {
            caps |= XH_CAPS_AVC444_V2;
        }
    }
    out_uint16_le(clientCon->out_s, 4);  /* session capabilities */
    out_uint16_le(clientCon->out_s, 8);  /* size */
    out_uint32_le(clientCon->out_s, caps);
    clientCon->count++;
    LOG(LOG_LEVEL_INFO, "rdpSendAccelAssistMonitors: capabilities 0x%8.8x "
        "(AVC444 %s)", caps,
        (caps & XH_CAPS_AVC444)
        ? ((caps & XH_CAPS_AVC444_V2) ? "v2 negotiated" : "v1 negotiated")
        : "not negotiated");
    if (dev->monitorCount < 1)
    {
        width = dev->width;
        height = dev->height;
        out_uint16_le(clientCon->out_s, 2);
        out_uint16_le(clientCon->out_s, 20); /* size */
        out_uint16_le(clientCon->out_s, width);
        out_uint16_le(clientCon->out_s, height);
        out_uint32_le(clientCon->out_s, 0xDEADBEEF);
        out_uint32_le(clientCon->out_s, clientCon->conNumber);
        out_uint32_le(clientCon->out_s, 0);
        clientCon->count++;
    }
    else
    {
        for (index = 0; index < dev->monitorCount; index++)
        {
            width = dev->minfo[index].right - dev->minfo[index].left + 1;
            height = dev->minfo[index].bottom - dev->minfo[index].top + 1;
            out_uint16_le(clientCon->out_s, 2);
            out_uint16_le(clientCon->out_s, 20); /* size */
            out_uint16_le(clientCon->out_s, width);
            out_uint16_le(clientCon->out_s, height);
            out_uint32_le(clientCon->out_s, 0xDEADBEEF);
            out_uint32_le(clientCon->out_s, clientCon->conNumber);
            out_uint32_le(clientCon->out_s, index);
            clientCon->count++;
        }
    }
    s_mark_end(clientCon->out_s);
    len = (int) (clientCon->out_s->end - clientCon->out_s->data);
    s_pop_layer(clientCon->out_s, iso_hdr);
    out_uint16_le(clientCon->out_s, 100);
    out_uint16_le(clientCon->out_s, clientCon->count);
    out_uint32_le(clientCon->out_s, len - layer_size);
    rv = rdpClientConSend(dev, clientCon, clientCon->out_s->data, len);
    return rv;
}

int
rdpClientConPreCheck(rdpPtr dev, rdpClientCon *clientCon, int in_size);

/******************************************************************************/
static int
rdpSendMemoryAllocationComplete(rdpPtr dev, rdpClientCon *clientCon)
{
    int len;
    int rv;
    int width = dev->width;
    int height = dev->height;
    int alignment = 0;
    const int layer_size = 8;

    switch (clientCon->client_info.capture_code)
    {
        case CC_SUF_RFX:
        case CC_GFX_PRO:
            alignment = XRDP_RFX_ALIGN;
            break;
        case CC_SUF_A2:
        case CC_GFX_A2:
            alignment = XRDP_H264_ALIGN;
            break;
        default:
            break;
    }
    if (alignment != 0)
    {
        width = RDPALIGN(dev->width, alignment);
        height = RDPALIGN(dev->height, alignment);
    }

    rdpClientConSendPending(dev, clientCon);
    init_stream(clientCon->out_s, 0);
    s_push_layer(clientCon->out_s, iso_hdr, layer_size);
    clientCon->count++;
    out_uint16_le(clientCon->out_s, 3); /* code: memory allocation complete */
    out_uint16_le(clientCon->out_s, 8); /* size */
    out_uint16_le(clientCon->out_s, width);
    out_uint16_le(clientCon->out_s, height);
    s_mark_end(clientCon->out_s);
    len = (int) (clientCon->out_s->end - clientCon->out_s->data);
    s_pop_layer(clientCon->out_s, iso_hdr);
    out_uint16_le(clientCon->out_s, 100); /* Metadata message to xrdp (or if using accel assist, signal) */
    out_uint16_le(clientCon->out_s, clientCon->count);
    out_uint32_le(clientCon->out_s, len - layer_size);
    rv = rdpClientConSend(dev, clientCon, clientCon->out_s->data, len);
    return rv;
}

/******************************************************************************/
/**
 * Process the monitors in the client_info
 * @param dev RDP device
 * @param clientCon Client connection
 */
static void
rdpClientConProcessClientInfoMonitors(rdpPtr dev, rdpClientCon *clientCon)
{
    int index;
    BoxRec box;
    if (clientCon->client_info.display_sizes.monitorCount > 0)
    {
        LOG(LOG_LEVEL_INFO, "  client can do multimon");
        LOG(LOG_LEVEL_INFO, "  client monitor data, monitorCount=%d",
            clientCon->client_info.display_sizes.monitorCount);
        clientCon->doMultimon = 1;
        dev->doMultimon = 1;
        memcpy(dev->minfo, clientCon->client_info.display_sizes.minfo, sizeof(dev->minfo));
        dev->monitorCount = clientCon->client_info.display_sizes.monitorCount;

        box.x1 = dev->minfo[0].left;
        box.y1 = dev->minfo[0].top;
        box.x2 = dev->minfo[0].right;
        box.y2 = dev->minfo[0].bottom;
        /* adjust monitor info so it's not negative */
        for (index = 1; index < dev->monitorCount; index++)
        {
            box.x1 = min(box.x1, dev->minfo[index].left);
            box.y1 = min(box.y1, dev->minfo[index].top);
            box.x2 = max(box.x2, dev->minfo[index].right);
            box.y2 = max(box.y2, dev->minfo[index].bottom);
        }
        for (index = 0; index < dev->monitorCount; index++)
        {
            dev->minfo[index].left -= box.x1;
            dev->minfo[index].top -= box.y1;
            dev->minfo[index].right -= box.x1;
            dev->minfo[index].bottom -= box.y1;
            LOG(LOG_LEVEL_INFO, "    left %d top %d right %d bottom %d",
                dev->minfo[index].left,
                dev->minfo[index].top,
                dev->minfo[index].right,
                dev->minfo[index].bottom);
        }
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "  client can not do multimon");
        clientCon->doMultimon = 0;
        dev->doMultimon = 0;
        dev->monitorCount = 0;
    }
#if defined(XORGXRDP_LRANDR)
    if (dev->nvidia && dev->nvidia_grid)
    {
        rdpRandRGridSetRdpOutputs(dev);
    }
    else if (dev->nvidia)
    {
        rdpLRRSetRdpOutputs(dev);
    }
    else
    {
        rdpRRSetRdpOutputs(dev);
        RRTellChanged(dev->pScreen);
    }
#else
    if (dev->nvidia && dev->nvidia_grid)
    {
        rdpRandRGridSetRdpOutputs(dev);
    }
    else
    {
        rdpRRSetRdpOutputs(dev);
        RRTellChanged(dev->pScreen);
    }
#endif
}

/******************************************************************************/
static int
rdpClientConProcessMsgClientInfo(rdpPtr dev, rdpClientCon *clientCon)
{
    struct stream *s;
    int bytes;
    int i1;

    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsgClientInfo:");
    s = clientCon->in_s;
    in_uint32_le(s, bytes);
    if (bytes > sizeof(clientCon->client_info))
    {
        bytes = sizeof(clientCon->client_info);
    }
    memcpy(&(clientCon->client_info), s->p - 4, bytes);
    clientCon->client_info.size = bytes;

    // This shouldn't happen - xrdp should check the version we send it
    // before sending client info.
    if (clientCon->client_info.version != XUP_CLIENT_INFO_CURRENT_VERSION)
    {
        LOG(LOG_LEVEL_INFO, "expected xrdp client_info version %d, got %d",
            XUP_CLIENT_INFO_CURRENT_VERSION,
            clientCon->client_info.version);
        FatalError("Incompatible xrdp version detected  - please recompile");
    }

    LOG(LOG_LEVEL_INFO, "  got client info bytes %d", bytes);
    LOG(LOG_LEVEL_INFO, "  jpeg support %d", clientCon->client_info.jpeg);
    i1 = clientCon->client_info.offscreen_support_level;
    LOG(LOG_LEVEL_INFO, "  offscreen support %d", i1);
    i1 = clientCon->client_info.offscreen_cache_size;
    LOG(LOG_LEVEL_INFO, "  offscreen size %d", i1);
    i1 = clientCon->client_info.offscreen_cache_entries;
    LOG(LOG_LEVEL_INFO, "  offscreen entries %d", i1);

    /* Monitor info */
    int bpp = clientCon->client_info.bpp;
    if (bpp < 15)
    {
        clientCon->rdp_Bpp = 1;
        clientCon->rdp_Bpp_mask = 0xff;
    }
    else if (bpp == 15)
    {
        clientCon->rdp_Bpp = 2;
        clientCon->rdp_Bpp_mask = 0x7fff;
    }
    else if (bpp == 16)
    {
        clientCon->rdp_Bpp = 2;
        clientCon->rdp_Bpp_mask = 0xffff;
    }
    else if (bpp > 16)
    {
        clientCon->rdp_Bpp = 4;
        clientCon->rdp_Bpp_mask = 0xffffff;
    }

    rdpClientConResizeAllMemoryAreas(dev, clientCon);
    rdpClientConProcessClientInfoMonitors(dev, clientCon);

    if (clientCon->client_info.offscreen_support_level > 0)
    {
        if (clientCon->client_info.offscreen_cache_entries > 0)
        {
            clientCon->maxOsBitmaps = clientCon->client_info.offscreen_cache_entries;
            free(clientCon->osBitmaps);
            clientCon->osBitmaps = g_new0(struct rdpup_os_bitmap,
                                          clientCon->maxOsBitmaps);
        }
    }

    if (clientCon->client_info.orders[0x1b])   /* 27 NEG_GLYPH_INDEX_INDEX */
    {
        LOG(LOG_LEVEL_INFO, "  client supports glyph cache but server disabled");
        //clientCon->doGlyphCache = 1;
    }
    if (clientCon->client_info.order_flags_ex & 0x100)
    {
        clientCon->doComposite = 1;
    }
    if (clientCon->doGlyphCache)
    {
        LOG(LOG_LEVEL_INFO, "  using glyph cache");
    }
    if (clientCon->doComposite)
    {
        LOG(LOG_LEVEL_INFO, "  using client composite");
    }
    LOG(LOG_LEVEL_TRACE, "order_flags_ex 0x%x", clientCon->client_info.order_flags_ex);
    if (clientCon->client_info.offscreen_cache_entries == 2000)
    {
        LOG(LOG_LEVEL_INFO, "  client can do offscreen to offscreen blits");
        clientCon->canDoPixToPix = 1;
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "  client can not do offscreen to offscreen blits");
        clientCon->canDoPixToPix = 0;
    }
    if (clientCon->client_info.pointer_flags & 1)
    {
        LOG(LOG_LEVEL_INFO, "  client can do new(color) cursor");
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "  client can not do new(color) cursor");
    }

    /* rdpLoadLayout */
    rdpInputKeyboardEvent(dev, 18, (long)(&(clientCon->client_info)),
                          0, 0, 0);

    rdpSendMemoryAllocationComplete(dev, clientCon);
    rdpClientConAddDirtyScreen(dev, clientCon, 0, 0, clientCon->rdp_width,
                               clientCon->rdp_height);

    /* currently only nvenc and h264 is supported */
    {
        const char *t = getenv("XORGXRDP_TIMING");
        const char *d = getenv("XORGXRDP_CAPTURE_DRAIN");
        clientCon->timing.enabled = (t != NULL && strcmp(t, "0") != 0);
        clientCon->timing.force_drain = (d != NULL && strcmp(d, "0") != 0);
        if (clientCon->timing.force_drain)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: capture will "
                "drain the GPU rather than flush");
        }
        if (clientCon->timing.enabled)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConGotConnection: frame timing on");
        }
    }
    if (rdpClientConUseAccelAssist(dev, clientCon))
    {
        clientCon->use_accel_assist = 1;
        rdpStartAccelAssist(dev, clientCon);
        rdpSendAccelAssistMonitors(dev, clientCon);
    }

    return 0;
}

/******************************************************************************/
static int
rdpClientConProcessMsgClientRegion(rdpPtr dev, rdpClientCon *clientCon)
{
    struct stream *s;
    int flags;
    int x;
    int y;
    int cx;
    int cy;
    RegionRec reg;
    BoxRec box;

    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsgClientRegion:");
    s = clientCon->in_s;

    in_uint32_le(s, flags);
    in_uint32_le(s, clientCon->rect_id_ack);
    in_uint32_le(s, x);
    in_uint32_le(s, y);
    in_uint32_le(s, cx);
    in_uint32_le(s, cy);
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConProcessMsgClientRegion: %d %d %d %d flags 0x%8.8x",
        x, y, cx, cy, flags);
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConProcessMsgClientRegion: rect_id %d rect_id_ack %d",
        clientCon->rect_id, clientCon->rect_id_ack);

    box.x1 = x;
    box.y1 = y;
    box.x2 = box.x1 + cx;
    box.y2 = box.y1 + cy;

    rdpRegionInit(&reg, &box, 0);
    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsgClientRegion: %d %d %d %d",
        box.x1, box.y1, box.x2, box.y2);
    rdpRegionSubtract(clientCon->shmRegion, clientCon->shmRegion, &reg);
    rdpRegionUninit(&reg);
    rdpScheduleDeferredUpdate(clientCon);
    return 0;
}

/******************************************************************************/
/* Adaptive capture pacing, steered by the client's own round trip.

   msFrameInterval is the minimum spacing between captures. Fixed, it has to
   be tuned to the slowest client that will connect and then throttles every
   other client sharing the session. Measured on one host at 2688x1488, same
   session and same content: a native client answered in 11-16 ms while a
   browser client answered in 222-289 ms. No single constant serves both.

   The signal is the round trip xrdp measures from its own send to the
   client's acknowledgement, sent down with each ack. It must be that figure
   and not the send-to-ack gap this file can measure locally: the local one
   runs from OUR send, and we do not send again until msFrameInterval has
   elapsed, so it contains the very interval being set. An earlier version of
   this loop used it and wound itself up to the ceiling -- raising the
   interval raised the signal that decides whether to raise it. Verified
   against both clients before this was rebuilt: the local gap read 18-20 ms
   while the true client latency ranged 1-13, and under load the true figure
   moved 20x where the local one moved 4x.

   Steer towards the smoothed round trip: a client answering in N ms can
   accept a frame every N ms. Move a quarter of the way each time, in both
   directions, so recovering costs what backing off did. Back off on the
   first bad sample, take rate back only after a sustained good run. */
static void
rdpClientConPaceUpdate(rdpClientCon *clientCon, int rtt)
{
    int interval;
    int target;

    if (!clientCon->pace_enabled || rtt <= 0)
    {
        return;
    }
    /* EWMA with a quarter weight on the new sample. Single frames are noisy
       -- a scene change or a pause in the client shows up as one large
       round trip -- and reacting to those would make the interval jitter. */
    if (clientCon->pace_rtt_ms == 0)
    {
        clientCon->pace_rtt_ms = rtt;
    }
    else
    {
        /* Outliers are discarded rather than scaled down. Measured on a
           browser client, the round trip sits in the teens with isolated
           frames at 100-300 and occasionally over 1000 -- a scene change, or
           the client's own scheduler. Averaging those in holds the interval
           far above what the client needs.

           An earlier version clamped a sample to twice the current average
           instead. That ratchets: the clamp is relative to a value it is
           itself inflating, so each admitted spike raises the ceiling for the
           next one and a burst walks the average up geometrically. The
           interval then oscillated between the floor and 68 ms while the true
           round trip stayed inside a 10-54 ms band.

           A sample more than four times the average is treated as an event
           rather than a measurement and dropped. A real slowdown persists,
           and once four in a row have been dropped the average is plainly
           wrong, so it is reseeded from the current sample. */
        if (rtt > clientCon->pace_rtt_ms * 4 && clientCon->pace_rtt_ms > 0)
        {
            /* Too far above the average to be a measurement of the steady
               state -- a scene change, or the client's own scheduler. Damped
               rather than discarded: an outright reject means a genuine
               slowdown is invisible while it is happening. Measured on a
               browser client the average sat near 16 ms and real degradation
               ran to 65-77, which is inside the rejected band, so the
               interval stayed at 17 ms while the round trip was 65. A
               sixteenth of the way still lets sustained elevation register
               within a few frames, while a lone spike barely moves it. */
            clientCon->pace_rtt_ms += (rtt - clientCon->pace_rtt_ms) / 16;
        }
        else
        {
            clientCon->pace_rtt_ms += (rtt - clientCon->pace_rtt_ms) / 4;
        }
    }
    /* Let the average fill before steering by it. The first frames of a
       session carry the initial full-screen update, whose round trip is not
       representative of anything. */
    if (clientCon->pace_samples < 32)
    {
        clientCon->pace_samples++;
        return;
    }
    interval = (int) clientCon->msFrameInterval;
    target = clientCon->pace_rtt_ms;
    if (target < clientCon->pace_min_ms)
    {
        target = clientCon->pace_min_ms;
    }
    if (target > clientCon->pace_max_ms)
    {
        target = clientCon->pace_max_ms;
    }
    if (target > interval)
    {
        /* Behind: give ground at once. */
        clientCon->msFrameInterval = interval + (target - interval + 3) / 4;
        clientCon->pace_good_run = 0;
    }
    else if (target < interval)
    {
        /* Keeping up: take ground only after a sustained run, so a brief
           quiet patch does not pull the interval into a region that
           thrashes. Probing down is an experiment, not a target -- a client
           answering in 13 ms at 30 fps is not evidence it can serve 60 --
           so if the rate proves too high the round trip grows and the branch
           above gives it straight back. */
        clientCon->pace_good_run++;
        /* Four, not sixteen. With a spike in most hundred-frame windows a
           sixteen-ack run rarely completed, so the interval descended a few
           milliseconds per window and never reached the floor -- it spent
           the session climbing back from spikes. */
        if (clientCon->pace_good_run >= 4)
        {
            clientCon->pace_good_run = 0;
            clientCon->msFrameInterval = interval - (interval - target + 3) / 4;
        }
    }
    if ((int) clientCon->msFrameInterval != interval)
    {
        LOG(LOG_LEVEL_DEBUG, "rdpClientConPaceUpdate: interval %d -> %d ms "
            "(smoothed client rtt %d ms)", interval,
            (int) clientCon->msFrameInterval, clientCon->pace_rtt_ms);
    }
}

/******************************************************************************/
static int
rdpClientConProcessMsgClientRegionEx(rdpPtr dev, rdpClientCon *clientCon)
{
    struct stream *s;
    int flags;

    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsgClientRegionEx:");
    s = clientCon->in_s;

    in_uint32_le(s, flags);
    in_uint32_le(s, clientCon->rect_id_ack);
    if (clientCon->rect_id_ack == INT_MAX)
    {
        // Client just wishes to ack all in-flight frames
        clientCon->rect_id_ack = clientCon->rect_id;
    }
    /* Round trip from xrdp's send to the client's acknowledgement, appended
       to this message by newer xrdp builds. Absent from an older peer, hence
       the remaining-bytes check. Zero means unknown.

       Worth the plumbing because it is the only latency figure here that does
       not contain the capture interval: an attempt to pace capture by the
       send-to-ack gap measured locally fed the interval back into its own
       input and settled against the ceiling. */
    if (s->end - s->p >= 4)
    {
        int crtt;

        in_uint32_le(s, crtt);
        rdpClientConPaceUpdate(clientCon, crtt);
        if (crtt > 0)
        {
            clientCon->timing.crtt_total_ms += crtt;
            clientCon->timing.crtt_count++;
            if (crtt > clientCon->timing.crtt_max_ms)
            {
                clientCon->timing.crtt_max_ms = crtt;
            }
        }
    }
    if (clientCon->timing.enabled && clientCon->timing.sent_ms != 0)
    {
        CARD32 sent = clientCon->timing.send_time[clientCon->rect_id_ack %
                                                  RDP_SEND_TIME_SLOTS];
        int gap;

        if (sent == 0)
        {
            sent = clientCon->timing.sent_ms;
        }
        gap = (int) (GetTimeInMillis() - sent);

        clientCon->timing.ack_total_ms += gap;
        if (gap > clientCon->timing.ack_max_ms)
        {
            clientCon->timing.ack_max_ms = gap;
        }
        clientCon->timing.count++;
        if (clientCon->timing.count >= 100)
        {
            struct rdp_timing *t = &clientCon->timing;

            LOG(LOG_LEVEL_INFO, "xorgxrdp frame timing over %d frames: "
                "capture mean %d ms max %d, handoff mean %d ms max %d, "
                "send->ack mean %d ms max %d, blocked callbacks %d, "
                "capture split: blit mean %d ms, gpu-drain mean %d ms, "
                "idle mean %d ms max %d, inflight %d/100, sent %d, "
                "no-op callbacks %d, client rtt mean %d ms max %d, "
                "interval %d ms",
                t->count,
                t->capture_total_ms / t->count, t->capture_max_ms,
                t->send_total_ms / t->count, t->send_max_ms,
                t->ack_total_ms / t->count, t->ack_max_ms,
                t->blocked,
                t->blit_count ? t->blit_total_ms / t->blit_count : 0,
                t->blit_count ? t->sync_total_ms / t->blit_count : 0,
                t->capture_count ? t->idle_total_ms / t->capture_count : 0,
                t->idle_max_ms,
                t->capture_count
                    ? t->inflight_total * 100 / t->capture_count : 0,
                t->capture_count, t->damage_starved,
                t->crtt_count ? t->crtt_total_ms / t->crtt_count : 0,
                t->crtt_max_ms, (int) clientCon->msFrameInterval);
            /* Separate line: it answers a different question, and it is only
               interesting while the collapse is actually firing. */
            LOG(LOG_LEVEL_INFO, "xorgxrdp dirty region: %d captures, rects "
                "mean %d max %d, monitor covered mean %d%% max %d%%, "
                "%d captures covered 90%% or more",
                t->dirty_frames,
                t->dirty_frames ? t->dirty_rects_total / t->dirty_frames : 0,
                t->dirty_rects_max,
                t->dirty_frames ? t->dirty_area_total / t->dirty_frames : 0,
                t->dirty_area_max, t->dirty_full_frames);
            t->dirty_frames = 0; t->dirty_rects_total = 0;
            t->dirty_rects_max = 0; t->dirty_area_total = 0;
            t->dirty_area_max = 0; t->dirty_full_frames = 0;
            LOG(LOG_LEVEL_INFO, "xorgxrdp dirty region collapse: fired %d of "
                "%d multi-rect frames, rects discarded mean %d max %d, "
                "bounding box vs dirty area mean %d%% max %d%%",
                t->collapse_fired, t->collapse_considered,
                t->collapse_fired
                    ? t->collapse_rects_total / t->collapse_fired : 0,
                t->collapse_rects_max,
                t->collapse_fired
                    ? t->collapse_waste_total / t->collapse_fired : 0,
                t->collapse_waste_max);
            t->collapse_considered = 0; t->collapse_fired = 0;
            t->collapse_rects_total = 0; t->collapse_rects_max = 0;
            t->collapse_waste_total = 0; t->collapse_waste_max = 0;
            t->count = 0;
            t->capture_total_ms = 0; t->capture_max_ms = 0;
            t->send_total_ms = 0; t->send_max_ms = 0;
            t->ack_total_ms = 0; t->ack_max_ms = 0;
            t->blocked = 0;
            t->blit_total_ms = 0;
            t->sync_total_ms = 0; t->blit_count = 0;
            t->idle_total_ms = 0; t->idle_max_ms = 0;
            t->inflight_total = 0; t->damage_starved = 0;
            t->capture_count = 0;
            t->crtt_total_ms = 0; t->crtt_max_ms = 0;
            t->crtt_count = 0;
        }
    }
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConProcessMsgClientRegionEx: flags 0x%8.8x", flags);
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConProcessMsgClientRegionEx: rect_id %d "
        "rect_id_ack %d", clientCon->rect_id, clientCon->rect_id_ack);
    rdpScheduleDeferredUpdate(clientCon);
    return 0;
}

/******************************************************************************/
static int
rdpClientConProcessMsgClientSuppressOutput(rdpPtr dev, rdpClientCon *clientCon)
{
    int suppress;
    int left;
    int top;
    int right;
    int bottom;
    struct stream *s;

    s = clientCon->in_s;
    in_uint32_le(s, suppress);
    in_uint32_le(s, left);
    in_uint32_le(s, top);
    in_uint32_le(s, right);
    in_uint32_le(s, bottom);
    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsgClientSuppressOutput: "
        "suppress %d left %d top %d right %d bottom %d",
        suppress, left, top, right, bottom);
    clientCon->suppress_output = suppress;
    if (suppress == 0)
    {
        rdpClientConAddDirtyScreen(dev, clientCon, left, top,
                                   right - left, bottom - top);
    }
    return 0;
}

/******************************************************************************/
static int
rdpClientConProcessMsg(rdpPtr dev, rdpClientCon *clientCon)
{
    int msg_type;
    struct stream *s;

    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsg:");
    s = clientCon->in_s;
    in_uint16_le(s, msg_type);
    LOG(LOG_LEVEL_TRACE, "rdpClientConProcessMsg: msg_type %d", msg_type);
    switch (msg_type)
    {
        case 103: /* client input */
            rdpClientConProcessMsgClientInput(dev, clientCon);
            break;
        case 104: /* client info */
            rdpClientConProcessMsgClientInfo(dev, clientCon);
            break;
        case 105: /* client region */
            rdpClientConProcessMsgClientRegion(dev, clientCon);
            break;
        case 106: /* client region ex */
            rdpClientConProcessMsgClientRegionEx(dev, clientCon);
            break;
        case 108: /* client suppress output */
            rdpClientConProcessMsgClientSuppressOutput(dev, clientCon);
            break;
        default:
            LOG(LOG_LEVEL_INFO, "rdpClientConProcessMsg: unknown msg_type %d",
                msg_type);
            break;
    }

    return 0;
}

/******************************************************************************/
static int
rdpClientConGotData(ScreenPtr pScreen, rdpPtr dev, rdpClientCon *clientCon)
{
    int rv;

    LOG(LOG_LEVEL_TRACE, "rdpClientConGotData:");

    rv = rdpClientConRecvMsg(dev, clientCon);
    if (rv == 0)
    {
        rv = rdpClientConProcessMsg(dev, clientCon);
    }

    return rv;
}

/******************************************************************************/
static int
rdpClientConGotControlConnection(ScreenPtr pScreen, rdpPtr dev,
                                 rdpClientCon *clientCon)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConGotControlConnection:");
    return 0;
}

/******************************************************************************/
static int
rdpClientConGotControlData(ScreenPtr pScreen, rdpPtr dev,
                           rdpClientCon *clientCon)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConGotControlData:");
    return 0;
}

/******************************************************************************/
int
rdpClientConCheck(ScreenPtr pScreen)
{
    rdpPtr dev;
    rdpClientCon *clientCon;
    rdpClientCon *nextCon;
    fd_set rfds;
    struct timeval time;
    int max;
    int sel;
    int count;
    char buf[8];

    LOG(LOG_LEVEL_TRACE, "rdpClientConCheck:");
    dev = rdpGetDevFromScreen(pScreen);
    time.tv_sec = 0;
    time.tv_usec = 0;
    FD_ZERO(&rfds);
    count = 0;
    max = 0;

    if (dev->disconnect_sck > 0)
    {
        count++;
        FD_SET(LTOUI32(dev->disconnect_sck), &rfds);
        max = RDPMAX(dev->disconnect_sck, max);
    }

    if (dev->listen_sck > 0)
    {
        count++;
        FD_SET(LTOUI32(dev->listen_sck), &rfds);
        max = RDPMAX(dev->listen_sck, max);
    }
    clientCon = dev->clientConHead;
    while (clientCon != NULL)
    {
        if (!clientCon->connected)
        {
            /* I/O error on this client - remove it */
            nextCon = clientCon->next;
            rdpClientConDisconnect(dev, clientCon);
            clientCon = nextCon;
            continue;
        }

        if (clientCon->sck > 0)
        {
            count++;
            FD_SET(LTOUI32(clientCon->sck), &rfds);
            max = RDPMAX(clientCon->sck, max);
        }
        if (clientCon->sckControl > 0)
        {
            count++;
            FD_SET(LTOUI32(clientCon->sckControl), &rfds);
            max = RDPMAX(clientCon->sckControl, max);
        }
        if (clientCon->sckControlListener > 0)
        {
            count++;
            FD_SET(LTOUI32(clientCon->sckControlListener), &rfds);
            max = RDPMAX(clientCon->sckControlListener, max);
        }
        clientCon = clientCon->next;
    }
    if (count < 1)
    {
        sel = 0;
    }
    else
    {
        sel = select(max + 1, &rfds, 0, 0, &time);
    }
    if (sel < 1)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConCheck: no select");
        return 0;
    }

    if (dev->listen_sck > 0)
    {
        if (FD_ISSET(LTOUI32(dev->listen_sck), &rfds))
        {
            rdpClientConGotConnection(pScreen, dev);
        }
    }

    if (dev->disconnect_sck > 0)
    {
        if (FD_ISSET(LTOUI32(dev->disconnect_sck), &rfds))
        {

            if (g_sck_recv(dev->disconnect_sck, buf, sizeof(buf), 0))
            {
                LOG(LOG_LEVEL_INFO,
                    "rdpClientConCheck: got disconnection request");

                /* disconnect all clients */
                while (dev->clientConHead != NULL)
                {
                    rdpClientConDisconnect(dev, dev->clientConHead);
                }
            }
        }
    }

    for (clientCon = dev->clientConHead;
            clientCon != NULL;
            clientCon = clientCon->next)
    {
        if (clientCon->sck > 0)
        {
            if (FD_ISSET(LTOUI32(clientCon->sck), &rfds))
            {
                if (rdpClientConGotData(pScreen, dev, clientCon) != 0)
                {
                    LOG(LOG_LEVEL_INFO,
                        "rdpClientConCheck: rdpClientConGotData failed");
                    continue; /* skip other socket checks for this clientCon */
                }
            }
        }
        if (clientCon->sckControlListener > 0)
        {
            if (FD_ISSET(LTOUI32(clientCon->sckControlListener), &rfds))
            {
                if (rdpClientConGotControlConnection(pScreen, dev, clientCon) != 0)
                {
                    LOG(LOG_LEVEL_INFO, "rdpClientConCheck: "
                        "rdpClientConGotControlConnection failed");
                    continue;
                }
            }
        }
        if (clientCon->sckControl > 0)
        {
            if (FD_ISSET(LTOUI32(clientCon->sckControl), &rfds))
            {
                if (rdpClientConGotControlData(pScreen, dev, clientCon) != 0)
                {
                    LOG(LOG_LEVEL_INFO, "rdpClientConCheck: "
                        "rdpClientConGotControlData failed");
                    continue;
                }
            }
        }
    }
    return 0;
}

/******************************************************************************/
/**
 * Sets up a socket name string
 *
 * @param sockname Destination buffer
 * @param sockname_len Length of above
 * @param env_name Environment variable for Unqualified socket name
 * @param default_format Default format if no env_name, with '%s'
 *                       for display name
 * @param displaystr Pointer to display name
 */
void set_sock_name(char *sockname, unsigned int sockname_len,
                   const char *env_name,
                   const char *default_format, const char *displaystr)
{
    const char *socket_dir = g_socket_dir();
    unsigned int len;

    // Add the path plus a '/' to the output buffer
    len = g_snprintf(sockname, sockname_len, "%s/", socket_dir);
    if ((unsigned int)len < sockname_len)
    {
        sockname += len;
        sockname_len -= len;

        const char *env_val = getenv(env_name);
        if (env_val == NULL || env_val[0] == '\0')
        {
            (void)g_snprintf(sockname, sockname_len,
                             default_format, displaystr);
        }
        else
        {
            (void)g_snprintf(sockname, sockname_len, "%s", env_val);
        }
    }
}

/******************************************************************************/
int
rdpClientConInit(rdpPtr dev)
{
    int i;
    char *ptext;
    char *endptr = NULL;
    const char *socket_dir;

    socket_dir = g_socket_dir();
    if (!g_directory_exist(socket_dir))
    {
        if (!g_create_dir(socket_dir))
        {
            if (!g_directory_exist(socket_dir))
            {
                LOG(LOG_LEVEL_INFO,
                    "rdpClientConInit: g_create_dir(%s) failed", socket_dir);
                return 0;
            }
        }
        g_chmod_hex(socket_dir, 0x1777);
    }

    // Later versions of the X server removed the global display variable
    // and replaced it with a getter function
#ifdef HAS_DIX_GET_DISPLAY_NAME
    const char *display = dixGetDisplayName(&dev->pScreen);
    if (display == NULL)
    {
        FatalError("rdpClientConInit: Can't get display from DIX layer");
    }
#endif

    // Check display is numeric
    errno = 0;
    i = (int)strtol(display, &endptr, 10);
    if (errno != 0 || display == endptr || *endptr != 0)
    {
        FatalError("rdpClientConInit: can not run at non-integer display");
    }

    set_sock_name(dev->uds_data, sizeof(dev->uds_data),
                  "XRDP_X11RDP_SOCKET", "xrdp_display_X11-%s", display);

    if (dev->listen_sck == 0)
    {
        unlink(dev->uds_data);
        dev->listen_sck = g_sck_local_socket_stream();
        if (g_sck_local_bind(dev->listen_sck, dev->uds_data) != 0)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConInit: g_tcp_local_bind failed");
            return 1;
        }
        g_sck_listen(dev->listen_sck);
        g_chmod_hex(dev->uds_data, 0x0660);
        rdpClientConAddEnabledDevice(dev->pScreen, dev->listen_sck);
    }

    set_sock_name(dev->disconnect_uds, sizeof(dev->disconnect_uds),
                  "XRDP_DISCONNECT_SOCKET",
                  "xrdp_disconnect_display_X11-%s", display);

    if (dev->disconnect_sck == 0)
    {
        unlink(dev->disconnect_uds);
        dev->disconnect_sck = g_sck_local_socket_dgram();
        if (g_sck_local_bind(dev->disconnect_sck, dev->disconnect_uds) != 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConInit: g_tcp_local_bind failed at %s:%d",
                __FILE__, __LINE__);
            return 1;
        }
        g_sck_listen(dev->disconnect_sck);
        g_chmod_hex(dev->disconnect_uds, 0x0660);
        rdpClientConAddEnabledDevice(dev->pScreen, dev->disconnect_sck);
    }

    /* disconnect idle */
    ptext = getenv("XRDP_SESMAN_MAX_IDLE_TIME");
    if (ptext != 0)
    {
        i = atoi(ptext);
        if (i > 0)
        {
            dev->idle_disconnect_timeout_s = i;
        }

    }
    LOG(LOG_LEVEL_INFO,
        "rdpClientConInit: disconnect idle session after [%d] sec",
        dev->idle_disconnect_timeout_s);

    /* kill disconnected */
    ptext = getenv("XRDP_SESMAN_KILL_DISCONNECTED");
    if (ptext != 0)
    {
        i = atoi(ptext);
        if (i == 0)
        {
            dev->do_kill_disconnected = 0;
        }
        else
        {
            dev->do_kill_disconnected = 1;
        }
    }

    /* kill timeout (0 == immediate kill) */
    ptext = getenv("XRDP_SESMAN_MAX_DISC_TIME");
    if (ptext != 0)
    {
        i = atoi(ptext);
        if (i >= 0)
        {
            dev->disconnect_timeout_s = i;
        }
    }

    LOG(LOG_LEVEL_INFO,
        "rdpClientConInit: kill disconnected [%d] timeout [%d] sec",
        dev->do_kill_disconnected, dev->disconnect_timeout_s);


    return 0;
}

/******************************************************************************/
int
rdpClientConDeinit(rdpPtr dev)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConDeinit:");

    while (dev->clientConTail != NULL)
    {
        LOG(LOG_LEVEL_INFO, "rdpClientConDeinit: disconnecting clientCon");
        rdpClientConDisconnect(dev, dev->clientConTail);
    }

    if (dev->listen_sck != 0)
    {
        rdpClientConRemoveEnabledDevice(dev->listen_sck);
        g_sck_close(dev->listen_sck);
        LOG(LOG_LEVEL_INFO, "rdpClientConDeinit: deleting file %s", dev->uds_data);
        if (unlink(dev->uds_data) < 0)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConDeinit: failed to delete %s (%s)",
                dev->uds_data, strerror(errno));
        }
    }

    if (dev->disconnect_sck != 0)
    {
        rdpClientConRemoveEnabledDevice(dev->disconnect_sck);
        g_sck_close(dev->disconnect_sck);
        LOG(LOG_LEVEL_INFO, "rdpClientConDeinit: deleting file %s",
            dev->disconnect_uds);
        if (unlink(dev->disconnect_uds) < 0)
        {
            LOG(LOG_LEVEL_INFO,
                "rdpClientConDeinit: failed to delete %s (%s)",
                dev->disconnect_uds, strerror(errno));
        }
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConBeginUpdate(rdpPtr dev, rdpClientCon *clientCon)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConBeginUpdate:");

    if (clientCon->begin)
    {
        return 0;
    }
    init_stream(clientCon->out_s, 0);
    s_push_layer(clientCon->out_s, iso_hdr, 8);
    out_uint16_le(clientCon->out_s, 1); /* begin update */
    out_uint16_le(clientCon->out_s, 4); /* size */
    clientCon->begin = TRUE;
    clientCon->count = 1;

    return 0;
}

/******************************************************************************/
int
rdpClientConEndUpdate(rdpPtr dev, rdpClientCon *clientCon)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConEndUpdate");

    if (clientCon->connected && clientCon->begin)
    {
        if (dev->do_dirty_ons)
        {
            /* in this mode, end update is only called in check dirty */
            rdpClientConSendPending(dev, clientCon);
        }
        else
        {
            rdpClientConScheduleDeferredUpdate(dev);
        }
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConPreCheck(rdpPtr dev, rdpClientCon *clientCon, int in_size)
{
    int rv;

    rv = 0;
    if (clientCon->begin == FALSE)
    {
        rdpClientConBeginUpdate(dev, clientCon);
    }

    if ((clientCon->out_s->p - clientCon->out_s->data) >
        (clientCon->out_s->size - (in_size + 20)))
    {
        s_mark_end(clientCon->out_s);
        if (rdpClientConSendMsg(dev, clientCon) != 0)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConPreCheck: rdpup_send_msg failed");
            rv = 1;
        }
        clientCon->count = 0;
        init_stream(clientCon->out_s, 0);
        s_push_layer(clientCon->out_s, iso_hdr, 8);
    }

    return rv;
}

/******************************************************************************/
int
rdpClientConFillRect(rdpPtr dev, rdpClientCon *clientCon,
                     short x, short y, int cx, int cy)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConFillRect:");
        rdpClientConPreCheck(dev, clientCon, 12);
        out_uint16_le(clientCon->out_s, 3); /* fill rect */
        out_uint16_le(clientCon->out_s, 12); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint16_le(clientCon->out_s, cx);
        out_uint16_le(clientCon->out_s, cy);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConScreenBlt(rdpPtr dev, rdpClientCon *clientCon,
                      short x, short y, int cx, int cy, short srcx, short srcy)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConScreenBlt: x %d y %d cx %d cy %d "
            "srcx %d srcy %d",
            x, y, cx, cy, srcx, srcy);
        rdpClientConPreCheck(dev, clientCon, 16);
        out_uint16_le(clientCon->out_s, 4); /* screen blt */
        out_uint16_le(clientCon->out_s, 16); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint16_le(clientCon->out_s, cx);
        out_uint16_le(clientCon->out_s, cy);
        out_uint16_le(clientCon->out_s, srcx);
        out_uint16_le(clientCon->out_s, srcy);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetClip(rdpPtr dev, rdpClientCon *clientCon,
                    short x, short y, int cx, int cy)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetClip:");
        rdpClientConPreCheck(dev, clientCon, 12);
        out_uint16_le(clientCon->out_s, 10); /* set clip */
        out_uint16_le(clientCon->out_s, 12); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint16_le(clientCon->out_s, cx);
        out_uint16_le(clientCon->out_s, cy);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConResetClip(rdpPtr dev, rdpClientCon *clientCon)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConResetClip:");
        rdpClientConPreCheck(dev, clientCon, 4);
        out_uint16_le(clientCon->out_s, 11); /* reset clip */
        out_uint16_le(clientCon->out_s, 4); /* size */
        clientCon->count++;
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConConvertPixel(rdpPtr dev, rdpClientCon *clientCon, int in_pixel)
{
    int red;
    int green;
    int blue;
    int rv;

    rv = 0;

    if (dev->depth == 24)
    {
        if (clientCon->rdp_bpp == 24)
        {
            rv = in_pixel;
            SPLITCOLOR32(red, green, blue, rv);
            rv = COLOR24(red, green, blue);
        }
        else if (clientCon->rdp_bpp == 16)
        {
            rv = in_pixel;
            SPLITCOLOR32(red, green, blue, rv);
            rv = COLOR16(red, green, blue);
        }
        else if (clientCon->rdp_bpp == 15)
        {
            rv = in_pixel;
            SPLITCOLOR32(red, green, blue, rv);
            rv = COLOR15(red, green, blue);
        }
        else if (clientCon->rdp_bpp == 8)
        {
            rv = in_pixel;
            SPLITCOLOR32(red, green, blue, rv);
            rv = COLOR8(red, green, blue);
        }
    }
    else if (dev->depth == clientCon->rdp_bpp)
    {
        return in_pixel;
    }

    return rv;
}

/******************************************************************************/
int
rdpClientConConvertPixels(rdpPtr dev, rdpClientCon *clientCon,
                          const void *src, void *dst, int num_pixels)
{
    uint32_t pixel;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    const uint32_t *src32;
    uint32_t *dst32;
    uint16_t *dst16;
    uint8_t *dst8;
    int index;

    if (dev->depth == clientCon->rdp_bpp)
    {
        memcpy(dst, src, num_pixels * dev->Bpp);
        return 0;
    }

    if (dev->depth == 24)
    {
        src32 = (const uint32_t *) src;

        if (clientCon->rdp_bpp == 24)
        {
            dst32 = (uint32_t *) dst;

            for (index = 0; index < num_pixels; index++)
            {
                pixel = *src32;
                *dst32 = pixel;
                dst32++;
                src32++;
            }
        }
        else if (clientCon->rdp_bpp == 16)
        {
            dst16 = (uint16_t *) dst;

            for (index = 0; index < num_pixels; index++)
            {
                pixel = *src32;
                SPLITCOLOR32(red, green, blue, pixel);
                pixel = COLOR16(red, green, blue);
                *dst16 = pixel;
                dst16++;
                src32++;
            }
        }
        else if (clientCon->rdp_bpp == 15)
        {
            dst16 = (uint16_t *) dst;

            for (index = 0; index < num_pixels; index++)
            {
                pixel = *src32;
                SPLITCOLOR32(red, green, blue, pixel);
                pixel = COLOR15(red, green, blue);
                *dst16 = pixel;
                dst16++;
                src32++;
            }
        }
        else if (clientCon->rdp_bpp == 8)
        {
            dst8 = (uint8_t *) dst;

            for (index = 0; index < num_pixels; index++)
            {
                pixel = *src32;
                SPLITCOLOR32(red, green, blue, pixel);
                pixel = COLOR8(red, green, blue);
                *dst8 = pixel;
                dst8++;
                src32++;
            }
        }
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConAlphaPixels(const void *src, void *dst, int num_pixels)
{
    const uint32_t *src32;
    uint8_t *dst8;
    int index;

    src32 = (const uint32_t *) src;
    dst8 = (uint8_t *) dst;
    for (index = 0; index < num_pixels; index++)
    {
        *dst8 = (*src32) >> 24;
        dst8++;
        src32++;
    }
    return 0;
}

/******************************************************************************/
int
rdpClientConSetFgcolor(rdpPtr dev, rdpClientCon *clientCon, int fgcolor)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetFgcolor:");
        rdpClientConPreCheck(dev, clientCon, 8);
        out_uint16_le(clientCon->out_s, 12); /* set fgcolor */
        out_uint16_le(clientCon->out_s, 8); /* size */
        clientCon->count++;
        fgcolor = fgcolor & dev->Bpp_mask;
        fgcolor = rdpClientConConvertPixel(dev, clientCon, fgcolor) &
                  clientCon->rdp_Bpp_mask;
        out_uint32_le(clientCon->out_s, fgcolor);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetBgcolor(rdpPtr dev, rdpClientCon *clientCon, int bgcolor)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetBgcolor:");
        rdpClientConPreCheck(dev, clientCon, 8);
        out_uint16_le(clientCon->out_s, 13); /* set bg color */
        out_uint16_le(clientCon->out_s, 8); /* size */
        clientCon->count++;
        bgcolor = bgcolor & dev->Bpp_mask;
        bgcolor = rdpClientConConvertPixel(dev, clientCon, bgcolor) &
                  clientCon->rdp_Bpp_mask;
        out_uint32_le(clientCon->out_s, bgcolor);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetOpcode(rdpPtr dev, rdpClientCon *clientCon, int opcode)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetOpcode:");
        rdpClientConPreCheck(dev, clientCon, 6);
        out_uint16_le(clientCon->out_s, 14); /* set opcode */
        out_uint16_le(clientCon->out_s, 6); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, g_rdp_opcodes[opcode & 0xf]);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetPen(rdpPtr dev, rdpClientCon *clientCon, int style, int width)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetPen:");
        rdpClientConPreCheck(dev, clientCon, 8);
        out_uint16_le(clientCon->out_s, 17); /* set pen */
        out_uint16_le(clientCon->out_s, 8); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, style);
        out_uint16_le(clientCon->out_s, width);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConDrawLine(rdpPtr dev, rdpClientCon *clientCon,
                     short x1, short y1, short x2, short y2)
{
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConDrawLine:");
        rdpClientConPreCheck(dev, clientCon, 12);
        out_uint16_le(clientCon->out_s, 18); /* draw line */
        out_uint16_le(clientCon->out_s, 12); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, x1);
        out_uint16_le(clientCon->out_s, y1);
        out_uint16_le(clientCon->out_s, x2);
        out_uint16_le(clientCon->out_s, y2);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetCursorSystem(rdpPtr dev, rdpClientCon *clientCon,
                            int pointer_type)
{
    int size;

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetCursor:");
        size = 2 + 2 + 4;
        rdpClientConPreCheck(dev, clientCon, size);
        out_uint16_le(clientCon->out_s, 65); /* set cursor system */
        out_uint16_le(clientCon->out_s, size); /* size */
        clientCon->count++;
        out_uint32_le(clientCon->out_s, pointer_type);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConMoveCursor(rdpPtr dev, rdpClientCon *clientCon, int x, int y)
{
    int size;

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetCursor:");
        size = 2 + 2 + 2 + 2;
        rdpClientConPreCheck(dev, clientCon, size);
        out_uint16_le(clientCon->out_s, 66); /* move cursor */
        out_uint16_le(clientCon->out_s, size); /* size */
        clientCon->count++;
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetCursor(rdpPtr dev, rdpClientCon *clientCon,
                      short x, short y, uint8_t *cur_data, uint8_t *cur_mask)
{
    int size;

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetCursor:");
        size = 8 + 32 * (32 * 3) + 32 * (32 / 8);
        rdpClientConPreCheck(dev, clientCon, size);
        out_uint16_le(clientCon->out_s, 19); /* set cursor */
        out_uint16_le(clientCon->out_s, size); /* size */
        clientCon->count++;
        x = RDPMAX(0, x);
        x = RDPMIN(31, x);
        y = RDPMAX(0, y);
        y = RDPMIN(31, y);
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint8a(clientCon->out_s, cur_data, 32 * (32 * 3));
        out_uint8a(clientCon->out_s, cur_mask, 32 * (32 / 8));
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetCursorEx(rdpPtr dev, rdpClientCon *clientCon,
                        short x, short y, uint8_t *cur_data,
                        uint8_t *cur_mask, int bpp)
{
    int size;
    int Bpp;

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetCursorEx:");
        Bpp = (bpp == 0) ? 3 : (bpp + 7) / 8;
        size = 10 + 32 * (32 * Bpp) + 32 * (32 / 8);
        rdpClientConPreCheck(dev, clientCon, size);
        out_uint16_le(clientCon->out_s, 51); /* set cursor ex */
        out_uint16_le(clientCon->out_s, size); /* size */
        clientCon->count++;
        x = RDPMAX(0, x);
        x = RDPMIN(31, x);
        y = RDPMAX(0, y);
        y = RDPMIN(31, y);
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint16_le(clientCon->out_s, bpp);
        out_uint8a(clientCon->out_s, cur_data, 32 * (32 * Bpp));
        out_uint8a(clientCon->out_s, cur_mask, 32 * (32 / 8));
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConSetCursorShmFd(rdpPtr dev, rdpClientCon *clientCon,
                           short x, short y,
                           uint8_t *cur_data, uint8_t *cur_mask, int bpp,
                           int width, int height)
{
    int size;
    int Bpp;
    int fd = -1;
    int rv = 0;
    void *addr = NULL;
    uint8_t *shmemptr;
    size_t shmsize;

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSetCursorShm:");
        Bpp = (bpp == 0) ? 3 : (bpp + 7) / 8;
        shmsize = width * height * Bpp + width * height / 8;
        if (g_alloc_shm_map_fd(&addr, &fd, shmsize) != 0)
        {
            FatalError("rdpClientConSetCursorShmFd: g_alloc_shm_map_fd failed");
        }
        shmemptr = (uint8_t *)addr;
        size = 14;
        rdpClientConPreCheck(dev, clientCon, size);
        out_uint16_le(clientCon->out_s, 63); /* set cursor shmfd */
        out_uint16_le(clientCon->out_s, size); /* size */
        clientCon->count++;
        x = max(0, x);
        x = min(width - 1, x);
        y = max(0, y);
        y = min(height - 1, y);
        out_uint16_le(clientCon->out_s, x);
        out_uint16_le(clientCon->out_s, y);
        out_uint16_le(clientCon->out_s, bpp);
        out_uint16_le(clientCon->out_s, width);
        out_uint16_le(clientCon->out_s, height);
        memcpy(shmemptr, cur_data, width * height * Bpp);
        memcpy(shmemptr + width * height * Bpp, cur_mask, width * height / 8);
        rdpClientConSendPending(clientCon->dev, clientCon);
        rv = g_sck_send_fd_set(clientCon->sck, "int", 4, &fd, 1);
        LOG(LOG_LEVEL_TRACE,
            "rdpClientConSetCursorShmFd: g_sck_send_fd_set rv %d", rv);
        g_free_unmap_fd(shmemptr, fd, shmsize);
    }
    return rv;
}

/******************************************************************************/
int
rdpClientConCreateOsSurface(rdpPtr dev, rdpClientCon *clientCon,
                            int rdpindex, int width, int height)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConCreateOsSurface:");

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE,
            "rdpClientConCreateOsSurface: width %d height %d", width, height);
        rdpClientConPreCheck(dev, clientCon, 12);
        out_uint16_le(clientCon->out_s, 20);
        out_uint16_le(clientCon->out_s, 12);
        clientCon->count++;
        out_uint32_le(clientCon->out_s, rdpindex);
        out_uint16_le(clientCon->out_s, width);
        out_uint16_le(clientCon->out_s, height);
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConCreateOsSurfaceBpp(rdpPtr dev, rdpClientCon *clientCon,
                               int rdpindex, int width, int height, int bpp)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConCreateOsSurfaceBpp:");
    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE,
            "rdpClientConCreateOsSurfaceBpp: width %d height %d "
            "bpp %d", width, height, bpp);
        rdpClientConPreCheck(dev, clientCon, 13);
        out_uint16_le(clientCon->out_s, 31);
        out_uint16_le(clientCon->out_s, 13);
        clientCon->count++;
        out_uint32_le(clientCon->out_s, rdpindex);
        out_uint16_le(clientCon->out_s, width);
        out_uint16_le(clientCon->out_s, height);
        out_uint8(clientCon->out_s, bpp);
    }
    return 0;
}

/******************************************************************************/
int
rdpClientConSwitchOsSurface(rdpPtr dev, rdpClientCon *clientCon, int rdpindex)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConSwitchOsSurface:");

    if (clientCon->connected)
    {
        if (clientCon->rdpIndex == rdpindex)
        {
            return 0;
        }

        clientCon->rdpIndex = rdpindex;
        LOG(LOG_LEVEL_TRACE,
            "rdpClientConSwitchOsSurface: rdpindex %d", rdpindex);
        /* switch surface */
        rdpClientConPreCheck(dev, clientCon, 8);
        out_uint16_le(clientCon->out_s, 21);
        out_uint16_le(clientCon->out_s, 8);
        out_uint32_le(clientCon->out_s, rdpindex);
        clientCon->count++;
    }

    return 0;
}

/******************************************************************************/
int
rdpClientConDeleteOsSurface(rdpPtr dev, rdpClientCon *clientCon, int rdpindex)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConDeleteOsSurface: rdpindex %d", rdpindex);

    if (clientCon->connected)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConDeleteOsSurface: rdpindex %d", rdpindex);
        rdpClientConPreCheck(dev, clientCon, 8);
        out_uint16_le(clientCon->out_s, 22);
        out_uint16_le(clientCon->out_s, 8);
        clientCon->count++;
        out_uint32_le(clientCon->out_s, rdpindex);
    }

    return 0;
}

/*****************************************************************************/
/* returns -1 on error */
int
rdpClientConAddOsBitmap(rdpPtr dev, rdpClientCon *clientCon,
                        PixmapPtr pixmap, rdpPixmapPtr priv)
{
    int index;
    int rv;
    int oldest;
    int oldest_index;
    int this_bytes;

    LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap:");
    if (clientCon->connected == FALSE)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: test error 1");
        return -1;
    }

    if (clientCon->osBitmaps == NULL)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: test error 2");
        return -1;
    }

    this_bytes = pixmap->devKind * pixmap->drawable.height;
    if (this_bytes > MAX_OS_BYTES)
    {
        LOG(LOG_LEVEL_TRACE,
            "rdpClientConAddOsBitmap: error, too big this_bytes %d "
            "width %d height %d", this_bytes,
            pixmap->drawable.height, pixmap->drawable.height);
        return -1;
    }

    oldest = INT_MAX;
    oldest_index = -1;
    rv = -1;
    index = 0;

    while (index < clientCon->maxOsBitmaps)
    {
        if (clientCon->osBitmaps[index].used == FALSE)
        {
            clientCon->osBitmaps[index].used = TRUE;
            clientCon->osBitmaps[index].pixmap = pixmap;
            clientCon->osBitmaps[index].priv = priv;
            clientCon->osBitmaps[index].stamp = clientCon->osBitmapStamp;
            clientCon->osBitmapStamp++;
            clientCon->osBitmapNumUsed++;
            rv = index;
            break;
        }
        else
        {
            if (clientCon->osBitmaps[index].stamp < oldest)
            {
                oldest = clientCon->osBitmaps[index].stamp;
                oldest_index = index;
            }
        }
        index++;
    }

    if (rv == -1)
    {
        if (oldest_index == -1)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConAddOsBitmap: error");
        }
        else
        {
            LOG(LOG_LEVEL_TRACE,
                "rdpClientConAddOsBitmap: too many pixmaps removing "
                "oldest_index %d", oldest_index);
            rdpClientConRemoveOsBitmap(dev, clientCon, oldest_index);
            rdpClientConDeleteOsSurface(dev, clientCon, oldest_index);
            clientCon->osBitmaps[oldest_index].used = TRUE;
            clientCon->osBitmaps[oldest_index].pixmap = pixmap;
            clientCon->osBitmaps[oldest_index].priv = priv;
            clientCon->osBitmaps[oldest_index].stamp = clientCon->osBitmapStamp;
            clientCon->osBitmapStamp++;
            clientCon->osBitmapNumUsed++;
            rv = oldest_index;
        }
    }

    if (rv < 0)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: test error 3");
        return rv;
    }

    clientCon->osBitmapAllocSize += this_bytes;
    LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: this_bytes %d "
        "clientCon->osBitmapAllocSize %d",
        this_bytes, clientCon->osBitmapAllocSize);
#if USE_MAX_OS_BYTES
    while (clientCon->osBitmapAllocSize > MAX_OS_BYTES)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: must delete "
            "clientCon->osBitmapNumUsed %d",
            clientCon->osBitmapNumUsed);
        /* find oldest */
        oldest = INT_MAX;
        oldest_index = -1;
        index = 0;
        while (index < clientCon->maxOsBitmaps)
        {
            if (clientCon->osBitmaps[index].used &&
                (clientCon->osBitmaps[index].stamp < oldest))
            {
                oldest = clientCon->osBitmaps[index].stamp;
                oldest_index = index;
            }
            index++;
        }
        if (oldest_index == -1)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConAddOsBitmap: error 1");
            break;
        }
        if (oldest_index == rv)
        {
            LOG(LOG_LEVEL_INFO, "rdpClientConAddOsBitmap: error 2");
            break;
        }
        rdpClientConRemoveOsBitmap(dev, clientCon, oldest_index);
        rdpClientConDeleteOsSurface(dev, clientCon, oldest_index);
    }
#endif
    LOG(LOG_LEVEL_TRACE, "rdpClientConAddOsBitmap: new bitmap index %d", rv);
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConAddOsBitmap: clientCon->osBitmapNumUsed %d "
        "clientCon->osBitmapStamp 0x%8.8x",
        clientCon->osBitmapNumUsed, clientCon->osBitmapStamp);
    return rv;
}

/*****************************************************************************/
int
rdpClientConRemoveOsBitmap(rdpPtr dev, rdpClientCon *clientCon, int rdpindex)
{
    PixmapPtr pixmap;
    rdpPixmapPtr priv;
    int this_bytes;

    if (clientCon->osBitmaps == NULL)
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConRemoveOsBitmap: test error 1");
        return 1;
    }

    if ((rdpindex < 0) || (rdpindex >= clientCon->maxOsBitmaps))
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConRemoveOsBitmap: test error 2");
        return 1;
    }

    LOG(LOG_LEVEL_TRACE, "rdpClientConRemoveOsBitmap: index %d stamp %d",
        rdpindex, clientCon->osBitmaps[rdpindex].stamp);

    if (clientCon->osBitmaps[rdpindex].used)
    {
        pixmap = clientCon->osBitmaps[rdpindex].pixmap;
        priv = clientCon->osBitmaps[rdpindex].priv;
        rdpDrawItemRemoveAll(dev, priv);
        this_bytes = pixmap->devKind * pixmap->drawable.height;
        clientCon->osBitmapAllocSize -= this_bytes;
        LOG(LOG_LEVEL_TRACE, "rdpClientConRemoveOsBitmap: this_bytes %d "
            "clientCon->osBitmapAllocSize %d", this_bytes,
            clientCon->osBitmapAllocSize);
        clientCon->osBitmaps[rdpindex].used = 0;
        clientCon->osBitmaps[rdpindex].pixmap = 0;
        clientCon->osBitmaps[rdpindex].priv = 0;
        clientCon->osBitmapNumUsed--;
        priv->status = 0;
        priv->con_number = 0;
        priv->use_count = 0;
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "rdpup_remove_os_bitmap: error");
    }

    LOG(LOG_LEVEL_TRACE, "rdpup_remove_os_bitmap: clientCon->osBitmapNumUsed %d",
        clientCon->osBitmapNumUsed);
    return 0;
}

/*****************************************************************************/
int
rdpClientConUpdateOsUse(rdpPtr dev, rdpClientCon *clientCon, int rdpindex)
{
    if (clientCon->osBitmaps == NULL)
    {
        return 1;
    }

    if ((rdpindex < 0) || (rdpindex >= clientCon->maxOsBitmaps))
    {
        LOG(LOG_LEVEL_ERROR, "rdpClientConUpdateOsUse: bad index %d",
            rdpindex);
        return 1;
    }

    LOG(LOG_LEVEL_TRACE, "rdpClientConUpdateOsUse: index %d stamp %d",
        rdpindex, clientCon->osBitmaps[rdpindex].stamp);

    if (clientCon->osBitmaps[rdpindex].used)
    {
        clientCon->osBitmaps[rdpindex].stamp = clientCon->osBitmapStamp;
        clientCon->osBitmapStamp++;
    }
    else
    {
        LOG(LOG_LEVEL_INFO,
            "rdpClientConUpdateOsUse: error rdpindex %d", rdpindex);
    }

    return 0;
}

/******************************************************************************/
static CARD32
rdpClientConDeferredUpdateCallback(OsTimerPtr timer, CARD32 now, pointer arg)
{
    rdpPtr dev;
    rdpClientCon *clientCon;

    LOG(LOG_LEVEL_TRACE, "rdpClientConDeferredUpdateCallback");

    dev = (rdpPtr) arg;
    clientCon = dev->clientConHead;
    while (clientCon != NULL)
    {
        if (dev->do_dirty_ons)
        {
            rdpClientConCheckDirtyScreen(dev, clientCon);
        }
        else
        {
            rdpClientConSendPending(dev, clientCon);
        }
        clientCon = clientCon->next;
    }
    dev->sendUpdateScheduled = FALSE;
    return 0;
}

/******************************************************************************/
void
rdpClientConScheduleDeferredUpdate(rdpPtr dev)
{
    if (dev->sendUpdateScheduled == FALSE)
    {
        dev->sendUpdateScheduled = TRUE;
        dev->sendUpdateTimer =
                TimerSet(dev->sendUpdateTimer, 0, dev->msFrameInterval,
                         rdpClientConDeferredUpdateCallback, dev);
    }
}

/******************************************************************************/
int
rdpClientConCheckDirtyScreen(rdpPtr dev, rdpClientCon *clientCon)
{
    return 0;
}

/******************************************************************************/
static int
out_rects_dr(struct stream *s,
             BoxPtr rects_d, int num_rects_d,
             BoxPtr rects_c, int num_rects_c)
{
    int index;
    BoxRec box;
    short x;
    short y;
    short cx;
    short cy;

    out_uint16_le(s, num_rects_d);
    for (index = 0; index < num_rects_d; index++)
    {
        box = rects_d[index];
        x = box.x1;
        y = box.y1;
        cx = box.x2 - box.x1;
        cy = box.y2 - box.y1;
        out_uint16_le(s, x);
        out_uint16_le(s, y);
        out_uint16_le(s, cx);
        out_uint16_le(s, cy);
        LOG(LOG_LEVEL_TRACE,
            "out_rects_dr: rects_d index %d x %d y %d cx %d cy %d",
            index, x, y, cx, cy);
    }
    out_uint16_le(s, num_rects_c);
    for (index = 0; index < num_rects_c; index++)
    {
        box = rects_c[index];
        x = box.x1;
        y = box.y1;
        cx = box.x2 - box.x1;
        cy = box.y2 - box.y1;
        out_uint16_le(s, x);
        out_uint16_le(s, y);
        out_uint16_le(s, cx);
        out_uint16_le(s, cy);
        LOG(LOG_LEVEL_TRACE,
            "out_rects_dr: rects_c index %d x %d y %d cx %d cy %d",
            index, x, y, cx, cy);
    }
    return 0;
}

/******************************************************************************/
static int
rdpClientConSendPaintRectShmFd(rdpPtr dev, rdpClientCon *clientCon,
                               struct image_data *id,
                               RegionPtr dirtyReg,
                               BoxPtr copyRects, int numCopyRects)
{
    int size;
    int num_rects_d;
    int num_rects_c;
    struct stream *s;
    enum xrdp_capture_code capture_code;
    int start_frame_bytes;
    int wiretosurface1_bytes;
    int wiretosurface2_bytes;
    int end_frame_bytes;
    int surface_id;

    LOG(LOG_LEVEL_TRACE, "rdpClientConSendPaintRectShmFd:");
    LOG(LOG_LEVEL_TRACE,
        "rdpClientConSendPaintRectShmFd: cap_left %d cap_top %d "
        "cap_width %d cap_height %d",
        clientCon->cap_left, clientCon->cap_top,
        clientCon->cap_width, clientCon->cap_height);
    LOG(LOG_LEVEL_TRACE, "rdpClientConSendPaintRectShmFd: id->flags 0x%8.8X "
        "id->left %d id->top %d id->width %d id->height %d",
        id->flags, id->left, id->top, id->width, id->height);

    capture_code = clientCon->client_info.capture_code;
    LOG(LOG_LEVEL_TRACE, "rdpClientConSendPaintRectShmFd: capture_code %d",
        capture_code);

    num_rects_d = REGION_NUM_RECTS(dirtyReg);
    num_rects_c = numCopyRects;
    if ((num_rects_c < 1) || (num_rects_d < 1))
    {
        LOG(LOG_LEVEL_TRACE, "rdpClientConSendPaintRectShmFd: nothing to send");
        return 0;
    }

    rdpClientConBeginUpdate(dev, clientCon);

    if (capture_code < CC_GFX_PRO)
    {
        /* non gfx */
        size = 2 + 2 + 2 + num_rects_d * 8 + 2 + num_rects_c * 8;
        size += 4 + 4 + 4 + 4 + 2 + 2 + 2 + 2;
        rdpClientConPreCheck(dev, clientCon, size);

        s = clientCon->out_s;
        out_uint16_le(s, 64);
        out_uint16_le(s, size);
        clientCon->count++;

        out_rects_dr(s, REGION_RECTS(dirtyReg), num_rects_d,
                     copyRects, num_rects_c);

        out_uint32_le(s, id->flags);
        ++clientCon->rect_id;
        out_uint32_le(s, clientCon->rect_id);
        out_uint32_le(s, id->shmem_bytes);
        out_uint32_le(s, id->shmem_offset);
        if (capture_code == CC_SUF_RFX) /* rfx */
        {
            out_uint16_le(s, id->left);
            out_uint16_le(s, id->top);
            out_uint16_le(s, id->width);
            out_uint16_le(s, id->height);
        }
        else
        {
            out_uint16_le(s, 0);
            out_uint16_le(s, 0);
            out_uint16_le(s, clientCon->cap_width);
            out_uint16_le(s, clientCon->cap_height);
        }
        rdpClientConSendPending(clientCon->dev, clientCon);
        g_sck_send_fd_set(clientCon->sck, "int", 4, &(id->shmem_fd), 1);
    }
    else if (capture_code == CC_GFX_PRO) /* gfx pro rfx */
    {
        start_frame_bytes = 8 + 8;
        wiretosurface2_bytes = 8 + 13 +
                               2 + num_rects_d * 8 +
                               2 + num_rects_c * 8 +
                               8;
        end_frame_bytes = 8 + 4;

        size = 2 + 2;                   /* header */
        size += 4;                      /* message 62 cmd_bytes */
        size += start_frame_bytes;      /* start frame message */
        size += wiretosurface2_bytes;   /* frame message */
        size += end_frame_bytes;        /* end frame message */
        size += 4;                      /* message 62 data_bytes */

        rdpClientConPreCheck(dev, clientCon, size);
        s = clientCon->out_s;
        out_uint16_le(s, 62);
        out_uint16_le(s, size);
        clientCon->count++;

        out_uint32_le(s, start_frame_bytes +
                        wiretosurface2_bytes +
                        end_frame_bytes); /* total of cmd_bytes */

        ++clientCon->rect_id;

        /* XR_RDPGFX_CMDID_STARTFRAME */
        out_uint16_le(s, 0x000B);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, start_frame_bytes);    /* cmd_bytes */
        out_uint32_le(s, clientCon->rect_id);   /* frame_id */
        out_uint32_le(s, 0);                    /* time_stamp */

        surface_id = (id->flags >> 28) & 0xF;
        /* XR_RDPGFX_CMDID_WIRETOSURFACE_2 */
        out_uint16_le(s, 0x0002);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, wiretosurface2_bytes); /* cmd_bytes */
        out_uint16_le(s, surface_id);           /* surface_id */
        out_uint16_le(s, 0x0009);               /* codec_id */
        out_uint32_le(s, 0);                    /* codec_context_id */
        out_uint8(s, 0x20);                     /* pixel_format */

        out_uint32_le(s, id->flags);            /* flags */

        out_rects_dr(s, REGION_RECTS(dirtyReg), num_rects_d,
                     copyRects, num_rects_c);

        out_uint16_le(s, id->left);
        out_uint16_le(s, id->top);
        out_uint16_le(s, id->width);
        out_uint16_le(s, id->height);

        /* XR_RDPGFX_CMDID_ENDFRAME */
        out_uint16_le(s, 0x000C);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, end_frame_bytes);      /* cmd_bytes */
        out_uint32_le(s, clientCon->rect_id);   /* frame_id */

        if ((id->shmem_bytes > 0) && ((id->flags & 1) == 0))
        {
            out_uint32_le(s, id->shmem_bytes);  /* shmem_bytes */
            rdpClientConSendPending(clientCon->dev, clientCon);
            g_sck_send_fd_set(clientCon->sck, "int", 4, &(id->shmem_fd), 1);
        }
        else
        {
            out_uint32_le(s, 0);                /* shmem_bytes */
        }
    }
    else if (capture_code == CC_GFX_A2) /* gfx h264 */
    {
        start_frame_bytes = 8 + 8;
        wiretosurface1_bytes = 8 + 9 +
                               2 + num_rects_d * 8 +
                               2 + num_rects_c * 8 +
                               8;
        end_frame_bytes = 8 + 4;

        size = 2 + 2;                   /* header */
        size += 4;                      /* message 62 cmd_bytes */
        size += start_frame_bytes;      /* start frame message */
        size += wiretosurface1_bytes;   /* frame message */
        size += end_frame_bytes;        /* end frame message */
        size += 4;                      /* message 62 data_bytes */

        rdpClientConPreCheck(dev, clientCon, size);
        s = clientCon->out_s;
        out_uint16_le(s, 62);
        out_uint16_le(s, size);
        clientCon->count++;

        out_uint32_le(s, start_frame_bytes +
                        wiretosurface1_bytes +
                        end_frame_bytes); /* total of cmd_bytes */

        ++clientCon->rect_id;

        /* XR_RDPGFX_CMDID_STARTFRAME */
        out_uint16_le(s, 0x000B);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, start_frame_bytes);    /* cmd_bytes */
        out_uint32_le(s, clientCon->rect_id);   /* frame_id */
        out_uint32_le(s, 0);                    /* time_stamp */

        surface_id = (id->flags >> 28) & 0xF;
        /* XR_RDPGFX_CMDID_WIRETOSURFACE_1 */
        out_uint16_le(s, 0x0001);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, wiretosurface1_bytes); /* cmd_bytes */
        out_uint16_le(s, surface_id);           /* surface_id */
        /* AVC444 (0x000E) when the client negotiated it, else AVC420
           (0x000B). The accel-assist helper was told the same thing by the
           capability message, so its encoder is already sized for whichever
           we ask for here. */
        out_uint16_le(s, clientCon->use_accel_assist &&
                         rdpClientConAvc444(clientCon)
                         ? 0x000E : 0x000B);    /* codec_id */
        out_uint8(s, 0x20);                     /* pixel_format */

        out_uint32_le(s, id->flags);            /* flags */

        out_rects_dr(s, REGION_RECTS(dirtyReg), num_rects_d,
                     copyRects, num_rects_c);

        out_uint16_le(s, id->left);
        out_uint16_le(s, id->top);
        out_uint16_le(s, id->width);
        out_uint16_le(s, id->height);

        /* XR_RDPGFX_CMDID_ENDFRAME */
        out_uint16_le(s, 0x000C);
        out_uint16_le(s, 0);                    /* flags */
        out_uint32_le(s, end_frame_bytes);      /* cmd_bytes */
        out_uint32_le(s, clientCon->rect_id);   /* frame_id */

        if ((id->shmem_bytes > 0) && ((id->flags & 1) == 0))
        {
            out_uint32_le(s, id->shmem_bytes);  /* shmem_bytes */
            rdpClientConSendPending(clientCon->dev, clientCon);
            g_sck_send_fd_set(clientCon->sck, "int", 4, &(id->shmem_fd), 1);
        }
        else
        {
            out_uint32_le(s, 0);                /* shmem_bytes */
        }
    }

    rdpClientConEndUpdate(dev, clientCon);

    return 0;
}

/******************************************************************************/
/* this is called to capture a rect from the screen, if in a multi monitor
   session, this will get called for each monitor
   after the capture, it sends the info to xrdp
   returns error */
static int
rdpCapRect(rdpClientCon *clientCon, BoxPtr cap_rect, int mon,
           struct image_data *id)
{
    RegionPtr cap_dirty;
    RegionPtr cap_dirty_save;
    BoxPtr rects;
    BoxRec rect;
    int num_rects;

    cap_dirty = rdpRegionCreate(cap_rect, 0);
    LOG(LOG_LEVEL_TRACE, "rdpCapRect: cap_rect x1 %d y1 %d x2 %d y2 %d",
        cap_rect->x1, cap_rect->y1, cap_rect->x2, cap_rect->y2);
    rdpRegionIntersect(cap_dirty, cap_dirty, clientCon->dirtyRegion);
    num_rects = REGION_NUM_RECTS(cap_dirty);
    if (num_rects > 0)
    {
        /* Decide whether to replace the dirty region with its bounding box.

           The historical rule is a cap on the rect count, on the assumption
           that capture cost scales with it. On the accel-assist path it does
           not: the per-rect work is a CopyArea blit in rdpCopyBoxList, and
           the measurement recorded there puts the whole blit loop at 0 ms
           against 10-19 ms for the GPU drain it no longer does. What the
           collapse does cost is area -- the bounding box is by definition at
           least the area actually dirty, and usually much more -- and that
           area is paid again by the accel-assist shader pass, by the
           metablock, and by the client's copy out of the decoded frame.

           So count is a poor proxy for the trade being made. XORGXRDP_COLLAPSE
           selects the rule:

             count  (default)  collapse above XORGXRDP_COLLAPSE_MAX_RECTS,
                               which defaults to MAX_CAPTURE_RECTS -- the
                               behaviour that shipped
             area              collapse only when it is nearly free, i.e. when
                               the bounding box is within
                               XORGXRDP_COLLAPSE_RATIO percent of the area
                               actually dirty (default 200, so at most twice).
                               A hard cap still applies, far higher, to bound
                               the per-rect costs downstream
             never             never collapse, for A/B

           Sum of rect areas is exact here: pixman region rects are disjoint
           by construction, so it is the union area, not an overcount. */
        static int collapse_mode = -1;   /* 0 count, 1 area, 2 never */
        static int collapse_ratio_pct;
        static int collapse_max_rects;
        BoxPtr dirty_rects;
        long long union_area;
        long long extents_area;
        int waste_pct;
        int index;
        int collapse;

        if (collapse_mode < 0)
        {
            const char *m = getenv("XORGXRDP_COLLAPSE");
            const char *r = getenv("XORGXRDP_COLLAPSE_RATIO");
            const char *c = getenv("XORGXRDP_COLLAPSE_MAX_RECTS");

            collapse_mode = 0;
            if (m != NULL && strcmp(m, "area") == 0)
            {
                collapse_mode = 1;
            }
            else if (m != NULL && strcmp(m, "never") == 0)
            {
                collapse_mode = 2;
            }
            collapse_ratio_pct = (r != NULL) ? atoi(r) : 200;
            if (collapse_ratio_pct < 100)
            {
                collapse_ratio_pct = 100;
            }
            collapse_max_rects = (c != NULL) ? atoi(c)
                                 : (collapse_mode == 1 ? 256
                                                       : MAX_CAPTURE_RECTS);
            if (collapse_max_rects < 1)
            {
                collapse_max_rects = 1;
            }
            LOG(LOG_LEVEL_INFO, "rdpCapRect: dirty region collapse mode %s, "
                "ratio %d%%, max rects %d",
                collapse_mode == 1 ? "area" :
                (collapse_mode == 2 ? "never" : "count"),
                collapse_ratio_pct, collapse_max_rects);
        }

        rect = *rdpRegionExtents(cap_dirty);
        extents_area = (long long) (rect.x2 - rect.x1) *
                       (rect.y2 - rect.y1);
        union_area = 0;
        dirty_rects = REGION_RECTS(cap_dirty);
        for (index = 0; index < num_rects; index++)
        {
            union_area += (long long) (dirty_rects[index].x2 -
                                       dirty_rects[index].x1) *
                          (dirty_rects[index].y2 - dirty_rects[index].y1);
        }
        waste_pct = (union_area > 0)
                    ? (int) (extents_area * 100 / union_area) : 100;

        if (num_rects < 2)
        {
            /* Nothing to collapse; the measurement above still applies. */
            collapse = 0;
        }
        else
        {
            switch (collapse_mode)
            {
                case 1:
                    collapse = (num_rects > collapse_max_rects) ||
                               (waste_pct <= collapse_ratio_pct);
                    break;
                case 2:
                    collapse = 0;
                    break;
                default:
                    collapse = (num_rects > collapse_max_rects);
                    break;
            }
        }

        if (clientCon->timing.enabled)
        {
            struct rdp_timing *t = &clientCon->timing;
            long long mon_area = (long long) (cap_rect->x2 - cap_rect->x1) *
                                 (cap_rect->y2 - cap_rect->y1);
            int area_pct = (mon_area > 0)
                           ? (int) (union_area * 100 / mon_area) : 0;

            t->dirty_frames++;
            t->dirty_rects_total += num_rects;
            if (num_rects > t->dirty_rects_max)
            {
                t->dirty_rects_max = num_rects;
            }
            t->dirty_area_total += area_pct;
            if (area_pct > t->dirty_area_max)
            {
                t->dirty_area_max = area_pct;
            }
            if (area_pct >= 90)
            {
                t->dirty_full_frames++;
            }
        }
        if (clientCon->timing.enabled && (num_rects > 1))
        {
            struct rdp_timing *t = &clientCon->timing;

            t->collapse_considered++;
            if (collapse)
            {
                t->collapse_fired++;
                t->collapse_rects_total += num_rects;
                if (num_rects > t->collapse_rects_max)
                {
                    t->collapse_rects_max = num_rects;
                }
                t->collapse_waste_total += waste_pct;
                if (waste_pct > t->collapse_waste_max)
                {
                    t->collapse_waste_max = waste_pct;
                }
            }
        }

        if (collapse)
        {
            /* the dirty region is too complex, just get a rect that
               covers the whole region */
            rdpRegionDestroy(cap_dirty);
            cap_dirty = rdpRegionCreate(&rect, 0);
            num_rects = REGION_NUM_RECTS(cap_dirty);
        }
    }
    /* make a copy of cap_dirty because it may get altered */
    cap_dirty_save = rdpRegionCreate(NullBox, 0);
    rdpRegionCopy(cap_dirty_save, cap_dirty);
    if (num_rects > 0)
    {
        rects = 0;
        num_rects = 0;
        LOG(LOG_LEVEL_TRACE, "rdpCapRect: capture_code %d",
            clientCon->client_info.capture_code);
        CARD32 t0 = clientCon->timing.enabled ? GetTimeInMillis() : 0;
        if (rdpCapture(clientCon, cap_dirty, &rects, &num_rects, id))
        {
            CARD32 t1 = clientCon->timing.enabled ? GetTimeInMillis() : 0;
            LOG(LOG_LEVEL_TRACE, "rdpCapRect: num_rects %d", num_rects);
            if (clientCon->send_key_frame[mon])
            {
                clientCon->send_key_frame[mon] = 0;
                id->flags = (enum xrdp_encoder_flags)
                            ((int)id->flags | KEY_FRAME_REQUESTED);
            }
            rdpClientConSendPaintRectShmFd(clientCon->dev, clientCon, id,
                                           cap_dirty, rects, num_rects);
            if (clientCon->timing.enabled)
            {
                CARD32 t2 = GetTimeInMillis();
                int cap = (int) (t1 - t0);
                int snd = (int) (t2 - t1);

                clientCon->timing.capture_total_ms += cap;
                clientCon->timing.send_total_ms += snd;
                if (cap > clientCon->timing.capture_max_ms)
                {
                    clientCon->timing.capture_max_ms = cap;
                }
                if (snd > clientCon->timing.send_max_ms)
                {
                    clientCon->timing.send_max_ms = snd;
                }
                /* Dead time since the previous frame went out: the part
                   of the period no stage accounts for. Measured here, not
                   where the callback commits to a capture, because the ack
                   handler schedules a callback unconditionally and most of
                   those find nothing dirty and send nothing. */
                if (clientCon->timing.sent_ms != 0)
                {
                    int idle = (int) (t0 - clientCon->timing.sent_ms);

                    clientCon->timing.idle_total_ms += idle;
                    if (idle > clientCon->timing.idle_max_ms)
                    {
                        clientCon->timing.idle_max_ms = idle;
                    }
                }
                clientCon->timing.inflight_total +=
                    clientCon->rect_id - clientCon->rect_id_ack;
                clientCon->timing.capture_count++;
                clientCon->timing.sent_ms = t2;
                clientCon->timing.send_time[clientCon->rect_id %
                                            RDP_SEND_TIME_SLOTS] = t2;
            }
            free(rects);
        }
        else
        {
            LOG(LOG_LEVEL_INFO, "rdpCapRect: rdpCapture failed");
        }
    }
    rdpRegionSubtract(clientCon->dirtyRegion, clientCon->dirtyRegion,
                      cap_dirty_save);
    rdpRegionDestroy(cap_dirty);
    rdpRegionDestroy(cap_dirty_save);
    return 0;
}

/******************************************************************************/
static CARD32
rdpDeferredUpdateCallback(OsTimerPtr timer, CARD32 now, pointer arg)
{
    rdpClientCon *clientCon = (rdpClientCon *)arg;
    struct image_data id;
    int index;
    int monitor_index;
    int monitor_count;
    int depth;
    BoxRec cap_rect;

    LOG(LOG_LEVEL_TRACE, "rdpDeferredUpdateCallback:");
    clientCon->updateScheduled = FALSE;
    if (clientCon->suppress_output)
    {
        LOG(LOG_LEVEL_TRACE, "rdpDeferredUpdateCallback: suppress_output set");
        return 0;
    }
    if (clientCon->shmemstatus == SHM_UNINITIALIZED || clientCon->shmemstatus == SHM_RESIZING) {
        LOG(LOG_LEVEL_TRACE,
            "rdpDeferredUpdateCallback: clientCon->shmemstatus "
            "is not valid for capture operations: %d"
            " reschedule rect_id %d rect_id_ack %d",
            clientCon->shmemstatus, clientCon->rect_id, clientCon->rect_id_ack);
        return 0;
    }
    depth = 1;
    if (clientCon->use_accel_assist &&
        clientCon->accelAssistPixmaps[0][1] != NULL)
    {
        depth = clientCon->capture_depth;
    }
    if ((clientCon->rect_id - clientCon->rect_id_ack >= depth) ||
        /* do not allow captures until we have the client_info */
        clientCon->client_info.size == 0)
    {
        clientCon->timing.blocked++;
        return 0;
    }
    clientCon->lastUpdateTime = now;
    LOG(LOG_LEVEL_TRACE, "rdpDeferredUpdateCallback: sending");
    clientCon->updateRetries = 0;
    if (clientCon->dev->monitorCount < 1)
    {
        cap_rect.x1 = 0;
        cap_rect.y1 = 0;
        cap_rect.x2 = clientCon->rdp_width;
        cap_rect.y2 = clientCon->rdp_height;
        rdpClientConGetScreenImageRect(clientCon->dev, clientCon, &id);
        id.left = cap_rect.x1;
        id.top = cap_rect.y1;
        id.width = cap_rect.x2 - cap_rect.x1;
        id.height = cap_rect.y2 - cap_rect.y1;
        rdpCapRect(clientCon, &cap_rect, 0, &id);
    }
    else
    {
        monitor_index = 0;
        monitor_count = clientCon->dev->monitorCount;
        while (monitor_index < monitor_count)
        {
            // Did we get anything from the last monitor?
            if (clientCon->rect_id - clientCon->rect_id_ack >= depth)
            {
                LOG(LOG_LEVEL_TRACE,
                    "rdpDeferredUpdateCallback: reschedule rect_id %d "
                    "rect_id_ack %d",
                    clientCon->rect_id, clientCon->rect_id_ack);
                break;
            }
            // Offset the monitor index by the rectangle ID so we start
            // the monitor scan on a different monitor each time.
            index = (clientCon->rect_id + monitor_index) % monitor_count;
            cap_rect.x1 = clientCon->dev->minfo[index].left;
            cap_rect.y1 = clientCon->dev->minfo[index].top;
            cap_rect.x2 = clientCon->dev->minfo[index].right + 1;
            cap_rect.y2 = clientCon->dev->minfo[index].bottom + 1;
            rdpClientConGetScreenImageRect(clientCon->dev, clientCon, &id);
            id.left = cap_rect.x1;
            id.top = cap_rect.y1;
            id.width = cap_rect.x2 - cap_rect.x1;
            id.height = cap_rect.y2 - cap_rect.y1;
            id.flags = (index & 0xF) << 28;
            rdpCapRect(clientCon, &cap_rect, index, &id);
            monitor_index++;
        }
        if (monitor_index == monitor_count)
        {
            /* gone through all monitors, nothing changed */
            rdpRegionDestroy(clientCon->dirtyRegion);
            clientCon->dirtyRegion = rdpRegionCreate(NullBox, 0);
        }
    }
    if (rdpRegionNotEmpty(clientCon->dirtyRegion))
    {
        rdpScheduleDeferredUpdate(clientCon);
    }
    else if (clientCon->timing.enabled)
    {
        clientCon->timing.damage_starved++;
    }

    return 0;
}


/******************************************************************************/
static void
rdpScheduleDeferredUpdate(rdpClientCon *clientCon)
{
    uint32_t curTime;
    uint32_t msToWait;
    uint32_t minNextUpdateTime;

    if (clientCon->updateScheduled)
    {
        return;
    }
    curTime = (uint32_t) GetTimeInMillis();
    /* use two separate delays in order to limit the update rate and wait a bit
       for more changes before sending an update. Always waiting the longer
       delay would introduce unnecessarily much latency. */
    msToWait = MIN_MS_TO_WAIT_FOR_MORE_UPDATES;
    minNextUpdateTime = clientCon->lastUpdateTime +
                        clientCon->msFrameInterval;
    /* the first check is to gracefully handle the infrequent case of
       the time wrapping around */
    if(clientCon->lastUpdateTime < curTime &&
        minNextUpdateTime > curTime + msToWait)
    {
        msToWait = minNextUpdateTime - curTime;
    }

    clientCon->updateTimer = TimerSet(clientCon->updateTimer, 0,
                                      (CARD32) msToWait,
                                      rdpDeferredUpdateCallback,
                                      clientCon);
    clientCon->updateScheduled = TRUE;
    ++clientCon->updateRetries;
}

/******************************************************************************/
int
rdpClientConAddDirtyScreenReg(rdpPtr dev, rdpClientCon *clientCon,
                              RegionPtr reg)
{
    LOG(LOG_LEVEL_TRACE, "rdpClientConAddDirtyScreenReg:");
    rdpRegionUnion(clientCon->dirtyRegion, clientCon->dirtyRegion, reg);
    rdpScheduleDeferredUpdate(clientCon);
    return 0;
}

/******************************************************************************/
int
rdpClientConAddDirtyScreenBox(rdpPtr dev, rdpClientCon *clientCon,
                              BoxPtr box)
{
    RegionPtr reg;

    reg = rdpRegionCreate(box, 0);
    rdpClientConAddDirtyScreenReg(dev, clientCon, reg);
    rdpRegionDestroy(reg);
    return 0;
}

/******************************************************************************/
int
rdpClientConAddDirtyScreen(rdpPtr dev, rdpClientCon *clientCon,
                           int x, int y, int cx, int cy)
{
    BoxRec box;

    box.x1 = x;
    box.y1 = y;
    box.x2 = box.x1 + cx;
    box.y2 = box.y1 + cy;
    rdpClientConAddDirtyScreenBox(dev, clientCon, &box);
    return 0;
}

/******************************************************************************/
void
rdpClientConGetScreenImageRect(rdpPtr dev, rdpClientCon *clientCon,
                               struct image_data *id)
{
    id->left = 0;
    id->top = 0;
    id->width = dev->width;
    id->height = dev->height;
    id->bpp = clientCon->rdp_bpp;
    id->Bpp = clientCon->rdp_Bpp;
    id->lineBytes = dev->paddedWidthInBytes;
    id->flags = 0;
    id->pixels = dev->pfbMemory;
    id->shmem_pixels = clientCon->shmemptr;
    id->shmem_fd = clientCon->shmemfd;
    id->shmem_bytes = clientCon->shmem_bytes;
    id->shmem_offset = 0;
    id->shmem_lineBytes = clientCon->shmem_lineBytes;
}

/******************************************************************************/
int
rdpClientConAddAllReg(rdpPtr dev, RegionPtr reg, DrawablePtr pDrawable)
{
    rdpClientCon *clientCon;
    Bool drw_is_vis;

    drw_is_vis = XRDP_DRAWABLE_IS_VISIBLE(dev, pDrawable);
    if (!drw_is_vis)
    {
        return 0;
    }
    clientCon = dev->clientConHead;
    while (clientCon != NULL)
    {
        rdpClientConAddDirtyScreenReg(dev, clientCon, reg);
        clientCon = clientCon->next;
    }
    return 0;
}

/******************************************************************************/
int
rdpClientConAddAllBox(rdpPtr dev, BoxPtr box, DrawablePtr pDrawable)
{
    rdpClientCon *clientCon;
    Bool drw_is_vis;

    drw_is_vis = XRDP_DRAWABLE_IS_VISIBLE(dev, pDrawable);
    if (!drw_is_vis)
    {
        return 0;
    }
    clientCon = dev->clientConHead;
    while (clientCon != NULL)
    {
        rdpClientConAddDirtyScreenBox(dev, clientCon, box);
        clientCon = clientCon->next;
    }
    return 0;
}
