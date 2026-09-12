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

#ifndef _RDPCLIENTCON_H
#define _RDPCLIENTCON_H

#include <xorg-server.h>
#include <xorgVersion.h>
#include <xf86.h>

#include "xup_client_info.h"

/* Session capability bits sent to the accel-assist helper as message type 3
   of the control batch. Must match XH_CAPS_* in xrdp's
   xrdp_accel_assist/xrdp_accel_assist.h -- as with the message type numbers
   themselves, the two repositories carry their own copy. */
#define XH_CAPS_AVC444 (1 << 0)
#define XH_CAPS_AVC444_V2 (1 << 1)

/* XORGXRDP_TIMING=1: where a frame's time goes on this side of the pipe.
   Capture is gated on the previous frame being acknowledged
   (rdpDeferredUpdateCallback returns early while rect_id > rect_id_ack), so
   the loop runs one frame deep no matter what xrdp's frames_in_flight
   allows, and the rate is one over the sum of the stages. */
struct rdp_timing
{
    int enabled;
    int count;
    int capture_total_ms;
    int capture_max_ms;
    int send_total_ms;
    int send_max_ms;
    int ack_total_ms;          /* send -> rect_id_ack, the lockstep gap */
    int ack_max_ms;
    /* The client-latency round trip xrdp measures and now sends with each
       acknowledgement. Unlike ack_total_ms above, which runs from our send
       to the acknowledgement and therefore contains our own capture
       interval, this excludes it. */
    int crtt_total_ms;
    int crtt_max_ms;
    int crtt_count;
    int blocked;               /* callbacks that returned early on the gate */
    CARD32 sent_ms;
    /* Send time per frame, indexed by rect_id. The acknowledgement names
       the frame it is for, and with more than one frame in flight that is
       not the frame we sent most recently, so timing it against sent_ms
       understates the round trip. */
#define RDP_SEND_TIME_SLOTS 64
    CARD32 send_time[RDP_SEND_TIME_SLOTS];
    int blit_total_ms;         /* the CopyArea loop */
    int sync_total_ms;         /* the 1x1 GetImage that drains the GPU */
    int blit_count;
    int force_drain;            /* XORGXRDP_CAPTURE_DRAIN=1 */
    /* Why the loop is idle between frames. idle_total_ms is the dead time
       from handing a frame off to starting the next capture; inflight_total
       sums rect_id - rect_id_ack at capture start (0 means we drained
       completely and gained nothing from a capture depth above 1);
       damage_starved counts frames that ended with an empty dirtyRegion,
       i.e. we then sat waiting for the application to draw. */
    int idle_total_ms;
    int capture_count;          /* frames actually sent, vs count = acks */
    int idle_max_ms;
    int inflight_total;
    int damage_starved;
    /* The MAX_CAPTURE_RECTS collapse in rdpCapRect: how often the dirty
       region is replaced by its bounding box, how many rects that threw
       away, and what it cost in area. waste is the bounding box area as a
       percentage of the area actually dirty, so 100 means the collapse was
       free and 3000 means it captured thirty times what it had to.

       Nothing else in the pipe can see this: xrdp receives
       REGION_NUM_RECTS() of the already-collapsed region, so from there on
       a collapse is indistinguishable from the X server having reported one
       large damage rect. */
    /* Every capture, not only the multi-rect ones: how many rects the dirty
       region held and how much of the monitor they covered. The collapse
       counters below only see frames with more than one rect, which made a
       region that arrives as a single full-screen rect invisible -- the case
       that actually matters, since it is what the client ends up copying. */
    int dirty_frames;
    int dirty_rects_total;
    int dirty_rects_max;
    int dirty_area_total;       /* percent of the monitor, summed */
    int dirty_area_max;
    int dirty_full_frames;      /* captures covering 90% or more */
    int collapse_considered;    /* frames with more than one dirty rect */
    int collapse_fired;
    int collapse_rects_total;   /* pre-collapse rect count, when it fired */
    int collapse_rects_max;
    int collapse_waste_total;
    int collapse_waste_max;
};

/* used in rdpGlyphs.c */
struct font_cache
{
    int offset;
    int baseline;
    int width;
    int height;
    int crc;
    int stamp;
};

struct rdpup_os_bitmap
{
    int used;
    PixmapPtr pixmap;
    rdpPixmapPtr priv;
    int stamp;
};

enum shared_memory_status {
    SHM_UNINITIALIZED = 0,
    SHM_RESIZING,
    SHM_ACTIVE_PENDING,
    SHM_RFX_ACTIVE_PENDING,
    SHM_H264_ACTIVE_PENDING,
    SHM_ACTIVE,
    SHM_RFX_ACTIVE,
    SHM_H264_ACTIVE
};

/* one of these for each client */
struct _rdpClientCon
{
    rdpPtr dev;

    int sck;
    int sckControlListener;
    int sckControl;
    struct stream *out_s;
    struct stream *in_s;

    int connected; /* boolean. Set to False when I/O fails */
    int begin; /* boolean */
    int count;
    struct rdpup_os_bitmap *osBitmaps;
    int maxOsBitmaps;
    int osBitmapStamp;
    int osBitmapAllocSize;
    int osBitmapNumUsed;
    int doComposite;
    int doGlyphCache;
    int canDoPixToPix;
    int doMultimon;

    int rdp_bpp; /* client depth */
    int rdp_Bpp;
    int rdp_Bpp_mask;
    int rdp_width;
    int rdp_height;
    int rdp_format; /* XRDP_a8r8g8b8, XRDP_r5g6b5, ... */
    int cap_left;
    int cap_top;
    int cap_width;
    int cap_height;
    int cap_stride_bytes;

    int rdpIndex; /* current os target */

    int conNumber;

    /* rdpGlyphs.c */
    struct font_cache font_cache[12][256];
    int font_stamp;

    struct xup_client_info client_info;
    struct rdp_timing timing;

    uint8_t *shmemptr;
    int shmemfd;
    int shmem_bytes;
    int shmem_lineBytes;
    RegionPtr shmRegion;
    int rect_id;
    int rect_id_ack;
    enum shared_memory_status shmemstatus;

    /* Two capture buffers per monitor, alternated so a frame can be captured
       while the helper still reads the previous one.

       rdpCapture copies only the damage boxes, so alternating buffers would
       leave neither one a complete picture -- each holding just the boxes
       that happened to land in it. The main view survives that, reading back
       exactly the rects written, but the AVC444 auxiliary view is rendered
       full-frame and samples the whole texture, so it would read stale
       pixels outside the current damage. That was visible as blocks of stale
       content whenever damage was scattered, most obviously when dragging a
       window.

       accelAssistPending[mon][buf] is therefore the damage this buffer has
       missed since it was last written. A capture into it copies the current
       damage unioned with that debt, which restores the invariant a single
       buffer had for free: whatever the helper reads, every pixel of it is
       current. The client is still told only about the current damage. */
    PixmapPtr accelAssistPixmaps[16][2];
    RegionPtr accelAssistPending[16][2];
    int accelAssistBuf[16];
    int capture_depth;         /* XORGXRDP_CAPTURE_DEPTH, 1 or 2 */

    OsTimerPtr updateTimer;
    CARD32 lastUpdateTime; /* millisecond timestamp */
    int updateScheduled; /* boolean */
    int updateRetries;

    /* Minimum spacing between captures for THIS connection. Seeded from
       client_info, then steered at run time when adaptive pacing is on.
       Per connection rather than per device because the right value is a
       property of the client: measured on one host, a native client answers
       in 11-16 ms while a browser client on the same session and the same
       content answers in 222-289 ms. One device-wide constant has to be
       tuned for the slower of the two. */
    CARD32 msFrameInterval;
    int pace_enabled;          /* XORGXRDP_ADAPTIVE_PACE */
    int pace_min_ms;           /* XORGXRDP_PACE_MIN_MS */
    int pace_max_ms;           /* XORGXRDP_PACE_MAX_MS */
    int pace_rtt_ms;           /* smoothed client rtt, the control signal */
    int pace_samples;          /* acks seen, until the average is warm */
    int pace_good_run;         /* consecutive acks the client kept up on */

    RegionPtr dirtyRegion;

    int num_rfx_crcs_alloc[16];
    uint64_t *rfx_crcs[16];
    int send_key_frame[16];

    /* true = skip drawing */
    int suppress_output;

    int use_accel_assist;
    int accel_assist_pid;

    struct _rdpClientCon *next;
    struct _rdpClientCon *prev;
};

extern _X_EXPORT int
rdpClientConBeginUpdate(rdpPtr dev, rdpClientCon *clientCon);
extern _X_EXPORT int
rdpClientConEndUpdate(rdpPtr dev, rdpClientCon *clientCon);
extern _X_EXPORT int
rdpClientConSetFgcolor(rdpPtr dev, rdpClientCon *clientCon, int fgcolor);
extern _X_EXPORT int
rdpClientConFillRect(rdpPtr dev, rdpClientCon *clientCon,
                     short x, short y, int cx, int cy);
extern _X_EXPORT int
rdpClientConCheck(ScreenPtr pScreen);
extern _X_EXPORT int
rdpClientConInit(rdpPtr dev);
extern _X_EXPORT int
rdpClientConDeinit(rdpPtr dev);

extern _X_EXPORT int
rdpClientConDeleteOsSurface(rdpPtr dev, rdpClientCon *clientCon, int rdpindex);

extern _X_EXPORT int
rdpClientConRemoveOsBitmap(rdpPtr dev, rdpClientCon *clientCon, int rdpindex);

extern _X_EXPORT void
rdpClientConScheduleDeferredUpdate(rdpPtr dev);
extern _X_EXPORT int
rdpClientConCheckDirtyScreen(rdpPtr dev, rdpClientCon *clientCon);
extern _X_EXPORT int
rdpClientConAddDirtyScreenReg(rdpPtr dev, rdpClientCon *clientCon,
                              RegionPtr reg);
extern _X_EXPORT int
rdpClientConAddDirtyScreenBox(rdpPtr dev, rdpClientCon *clientCon,
                              BoxPtr box);
extern _X_EXPORT int
rdpClientConAddDirtyScreen(rdpPtr dev, rdpClientCon *clientCon,
                           int x, int y, int cx, int cy);
extern _X_EXPORT void
rdpClientConGetScreenImageRect(rdpPtr dev, rdpClientCon *clientCon,
                               struct image_data *id);
extern _X_EXPORT int
rdpClientConAddAllReg(rdpPtr dev, RegionPtr reg, DrawablePtr pDrawable);
extern _X_EXPORT int
rdpClientConAddAllBox(rdpPtr dev, BoxPtr box, DrawablePtr pDrawable);
extern _X_EXPORT int
rdpClientConSetCursorSystem(rdpPtr dev, rdpClientCon *clientCon,
                            int pointer_type);
extern _X_EXPORT int
rdpClientConMoveCursor(rdpPtr dev, rdpClientCon *clientCon, int x, int y);
extern _X_EXPORT int
rdpClientConSetCursor(rdpPtr dev, rdpClientCon *clientCon,
                      short x, short y, uint8_t *cur_data, uint8_t *cur_mask);
extern _X_EXPORT int
rdpClientConSetCursorEx(rdpPtr dev, rdpClientCon *clientCon,
                        short x, short y, uint8_t *cur_data,
                        uint8_t *cur_mask, int bpp);
extern _X_EXPORT int
rdpClientConSetCursorShmFd(rdpPtr dev, rdpClientCon *clientCon,
                           short x, short y,
                           uint8_t *cur_data, uint8_t *cur_mask, int bpp,
                           int width, int height);

#endif
