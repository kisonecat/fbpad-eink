#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <asm/types.h>
#include <sys/ioctl.h>
#include <asm/ioctl.h>
#include "fbink.h"
#include <errno.h>
#include "conf.h"
#include <string.h>

#include <linux/fb.h>
#include "mxcfb-kobo.h"
#include "refresh.h"

#include "draw.h"

const int isKoboMk7 = 0;
const int canHWInvert = 0;

#define ERRCODE(e) (-(e))
#define WARN(fmt, ...) ({ fprintf(stderr, "[FBInk] " fmt "!\n", ##__VA_ARGS__); })
#define LOG(fmt, ...)

#define FBFD_AUTO -1

// Kobo devices ([Mk3<->Mk6])
static int
    refresh_kobo(int                     fbfd,
		 const struct mxcfb_rect region,
		 uint32_t                waveform_mode,
		 uint32_t                update_mode,
		 bool                    is_nightmode,
		 uint32_t                marker)
{
	struct mxcfb_update_data_v1_ntx update = {
		.update_region = region,
		.waveform_mode = waveform_mode,
		.update_mode   = update_mode,
		.update_marker = marker,
		.temp          = TEMP_USE_AMBIENT,
		.flags         = (waveform_mode == WAVEFORM_MODE_REAGLD)
			     ? EPDC_FLAG_USE_AAD
			     : (waveform_mode == WAVEFORM_MODE_A2) ? EPDC_FLAG_FORCE_MONOCHROME : 0U,
		.alt_buffer_data = { 0U },
	};

	if (is_nightmode && canHWInvert) {
		update.flags |= EPDC_FLAG_ENABLE_INVERSION;
	}

	int rv;
	rv = ioctl(fbfd, MXCFB_SEND_UPDATE_V1_NTX, &update);

	if (rv < 0) {
		char        buf[256];
		const char* errstr = (char*)strerror_r(errno, buf, sizeof(buf));
		WARN("MXCFB_SEND_UPDATE_V1_NTX: %s", errstr);
		if (errno == EINVAL) {
			WARN("update_region={top=%u, left=%u, width=%u, height=%u}",
			     region.top,
			     region.left,
			     region.width,
			     region.height);
		}
		return ERRCODE(EXIT_FAILURE);
	}

	if (update_mode == UPDATE_MODE_FULL) {
		rv = ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE_V1, &marker);

		if (rv < 0) {
			char        buf[256];
			const char* errstr = (char*)strerror_r(errno, buf, sizeof(buf));
			WARN("MXCFB_WAIT_FOR_UPDATE_COMPLETE_V1: %s", errstr);
			return ERRCODE(EXIT_FAILURE);
		} else {
			// NOTE: Timeout is set to 10000ms
			LOG("Waited %ldms for completion of flashing update %u", (10000 - jiffies_to_ms(rv)), marker);
		}
	}

	return EXIT_SUCCESS;
}

// Kobo Mark 7 devices ([Mk7<->??)
static int
    refresh_kobo_mk7(int                     fbfd,
		     const struct mxcfb_rect region,
		     uint32_t                waveform_mode,
		     uint32_t                update_mode,
		     int                     dithering_mode,
		     bool                    is_nightmode,
		     uint32_t                marker)
{
	struct mxcfb_update_data_v2 update = {
		.update_region = region,
		.waveform_mode = waveform_mode,
		.update_mode   = update_mode,
		.update_marker = marker,
		.temp          = TEMP_USE_AMBIENT,
		.flags         = (waveform_mode == WAVEFORM_MODE_GLD16)
			     ? EPDC_FLAG_USE_REGAL
			     : (waveform_mode == WAVEFORM_MODE_A2) ? EPDC_FLAG_FORCE_MONOCHROME : 0U,
		.dither_mode = dithering_mode,
		.quant_bit   = (dithering_mode == EPDC_FLAG_USE_DITHERING_PASSTHROUGH)
				 ? 0
				 : (waveform_mode == WAVEFORM_MODE_A2 || waveform_mode == WAVEFORM_MODE_DU)
				       ? 1
				       : (waveform_mode == WAVEFORM_MODE_GC4) ? 3 : 7,
		.alt_buffer_data = { 0U },
	};

	if (is_nightmode && canHWInvert) {
		update.flags |= EPDC_FLAG_ENABLE_INVERSION;
	}

	int rv;
	rv = ioctl(fbfd, MXCFB_SEND_UPDATE_V2, &update);

	if (rv < 0) {
		char        buf[256];
		const char* errstr = (char*)strerror_r(errno, buf, sizeof(buf));
		WARN("MXCFB_SEND_UPDATE_V2: %s", errstr);
		if (errno == EINVAL) {
			WARN("update_region={top=%u, left=%u, width=%u, height=%u}",
			     region.top,
			     region.left,
			     region.width,
			     region.height);
		}
		return ERRCODE(EXIT_FAILURE);
	}

	if (update_mode == UPDATE_MODE_FULL) {
		struct mxcfb_update_marker_data update_marker = {
			.update_marker  = marker,
			.collision_test = 0U,
		};

		rv = ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE_V3, &update_marker);

		if (rv < 0) {
			char        buf[256];
			const char* errstr = (char*)strerror_r(errno, buf, sizeof(buf));
			WARN("MXCFB_WAIT_FOR_UPDATE_COMPLETE_V3: %s", errstr);
			return ERRCODE(EXIT_FAILURE);
		} else {
			// NOTE: Timeout is set to 5000ms
			LOG("Waited %ldms for completion of flashing update %u", (5000 - jiffies_to_ms(rv)), marker);
		}
	}

	return EXIT_SUCCESS;
}

// And another variant that'll try to get a nonblocking fd when using FBFD_AUTO
// NOTE: Only use this for functions that don't actually need to write to the fb, and only need an fd for ioctls!
//       Generally, those won't try to mmap the fb either ;).
static int
    open_fb_fd_nonblock(int* restrict fbfd, bool* restrict keep_fd)
{
	if (*fbfd == FBFD_AUTO) {
		// If we're opening a fd now, don't keep it around.
		*keep_fd = false;
		// We only need an fd for ioctl, hence O_NONBLOCK (as per open(2)).
		*fbfd = open(FBDEV, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (!*fbfd) {
			WARN("Error: cannot open framebuffer character device, aborting");
			return ERRCODE(EXIT_FAILURE);
		}
	}

	return EXIT_SUCCESS;
}

int
    refresh(int fbfd,
	    const struct mxcfb_rect region,
	    uint32_t waveform_mode,
	    int dithering_mode,
	    bool is_nightmode,
	    bool is_flashing,
	    bool no_refresh)
{
	// Were we asked to skip refreshes?
	if (no_refresh) {
		LOG("Skipping eInk refresh, as requested.");
		return EXIT_SUCCESS;
	}

	// NOTE: Discard bogus regions, they can cause a softlock on some devices.
	//       A 0x0 region is a no go on most devices, while a 1x1 region may only upset some Kindle models.
	//       Some devices even balk at 1xN or Nx1, so, catch that, too.
	if (region.width <= 1 || region.height <= 1) {
		WARN("Discarding bogus empty region (%ux%u) to avoid a softlock", region.width, region.height);
		return ERRCODE(EXIT_FAILURE);
	}

	// NOTE: There are also a number of hardware quirks (which got better on newer devices) related to region alignment,
	//       that the driver should already be taking care of...
	//       c.f., epdc_process_update @ mxc_epdc_fb.c or mxc_epdc_v2_fb.c
	//       If you see strays "unaligned" ... "Copying update before processing" entries in your dmesg, that's it.
	//       I'm hoping everything handles this sanely, because I really don't want to duplicate the driver's job...

	// NOTE: While we'd be perfect candidates for using A2 waveform mode, it's all kinds of fucked up on Kobos,
	//       and may lead to disappearing text or weird blending depending on the surrounding fb content...
	//       It only shows up properly when FULL, which isn't great...
        
	// NOTE: On the Forma, the (apparently randomly) broken A2 behavior is exacerbated if the FB is UR @ 8bpp...
	//       Which is intriguing, because that should make the driver's job easier (except maybe not on latest epdc v2 revs),
	//       c.f., epdc_submit_work_func @ drivers/video/fbdev/mxc/mxc_epdc_v2_fb.c
        
	// NOTE: And while we're on the fun quirks train: FULL never flashes w/ AUTO on (some?) Kobos,
	//       so request GC16 if we want a flash...
        
	// NOTE: FWIW, DU behaves properly when PARTIAL, but doesn't flash when FULL.
	//       Which somewhat tracks given AUTO's behavior on Kobos, as well as on Kindles.
	//       (i.e., DU or GC16 is most likely often what AUTO will land on).

	// So, handle this common switcheroo here...
	uint32_t wfm = (is_flashing && waveform_mode == WAVEFORM_MODE_AUTO) ? WAVEFORM_MODE_GC16 : waveform_mode;
	uint32_t upm = is_flashing ? UPDATE_MODE_FULL : UPDATE_MODE_PARTIAL;
	// We'll want to increment the marker on each subsequent calls (for API users)
	static uint32_t marker_counter = 0U;
	uint32_t marker = (uint32_t) getpid() + marker_counter;
	marker_counter++;

	// NOTE: Make sure update_marker is valid, an invalid marker *may* hang the kernel instead of failing gracefully,
	//       depending on the device/FW...
	if (marker == 0U) {
		marker = (70U + 66U + 73U + 78U + 75U);
	}

	if (isKoboMk7) {
		return refresh_kobo_mk7(fbfd, region, wfm, upm, dithering_mode, is_nightmode, marker);
	} else {
		return refresh_kobo(fbfd, region, wfm, upm, is_nightmode, marker);
	}
}

// Convert our public HW_DITHER_INDEX_T values to an appropriate mxcfb dithering mode constant
static int
    get_hwd_mode(uint8_t hw_dither_index)
{
	// NOTE: This hardware dithering (handled by the PxP) is only supported since EPDC v2!
	//       AFAICT, most of our eligible target devices only support PASSTHROUGH & ORDERED...
	//       (c.f., drivers/dma/pxp/pxp_dma_v3.c)
	int dither_algo = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;

	// Parse dithering algo...
	switch (hw_dither_index) {
		case HWD_PASSTHROUGH:
			dither_algo = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;
			break;
		case HWD_FLOYD_STEINBERG:
			dither_algo = EPDC_FLAG_USE_DITHERING_FLOYD_STEINBERG;
			break;
		case HWD_ATKINSON:
			dither_algo = EPDC_FLAG_USE_DITHERING_ATKINSON;
			break;
		case HWD_ORDERED:
			dither_algo = EPDC_FLAG_USE_DITHERING_ORDERED;
			break;
		case HWD_QUANT_ONLY:
			dither_algo = EPDC_FLAG_USE_DITHERING_QUANT_ONLY;
			break;
		default:
			LOG("Unknown (or unsupported) dithering mode '%s' @ index %hhu, defaulting to PASSTHROUGH",
			    hwd_to_string(hw_dither_index),
			    hw_dither_index);
			dither_algo = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;
			break;
	}

	return dither_algo;
}


// Convert our public WFM_MODE_INDEX_T values to an appropriate mxcfb waveform mode constant for the current device
static uint32_t
    get_wfm_mode(uint8_t wfm_mode_index)
{
	uint32_t waveform_mode = WAVEFORM_MODE_AUTO;

	// Parse waveform mode...
	switch (wfm_mode_index) {
		case WFM_AUTO:
			waveform_mode = WAVEFORM_MODE_AUTO;
			break;
		case WFM_DU:
			waveform_mode = WAVEFORM_MODE_DU;
			break;
		case WFM_GC16:
			waveform_mode = WAVEFORM_MODE_GC16;
			break;
		case WFM_GC4:
			waveform_mode = WAVEFORM_MODE_GC4;
			break;
		case WFM_A2:
			waveform_mode = WAVEFORM_MODE_A2;
			break;
		case WFM_GL16:
			waveform_mode = WAVEFORM_MODE_GL16;
			break;
		case WFM_REAGL:
			waveform_mode = WAVEFORM_MODE_REAGL;
			break;
		case WFM_REAGLD:
			waveform_mode = WAVEFORM_MODE_REAGLD;
			break;
		default:
			LOG("Unknown (or unsupported) waveform mode @ index %hhu, defaulting to AUTO",
			    wfm_mode_index);
			waveform_mode = WAVEFORM_MODE_AUTO;
			break;
	}

	return waveform_mode;
}

// Tweak the region to cover the full screen
static void
    fullscreen_region(struct mxcfb_rect* restrict region)
{
	region->top    = 0U;
	region->left   = 0U;
	region->width  = fb_cols();
	region->height = fb_rows();
}

// Small public wrapper around refresh(), without the caller having to depend on mxcfb headers
int
    fbink_refresh(int                fbfd,
		  uint32_t           region_top,
		  uint32_t           region_left,
		  uint32_t           region_width,
		  uint32_t           region_height,
		  uint8_t            dithering_mode)
{
	// Open the framebuffer if need be (nonblock, we'll only do ioctls)...
	bool keep_fd = true;
	if (open_fb_fd_nonblock(&fbfd, &keep_fd) != EXIT_SUCCESS) {
		return ERRCODE(EXIT_FAILURE);
	}

	// Same for the dithering mode, if we actually requested dithering...
	int region_dither = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;
	if (dithering_mode > 0U) {
		region_dither = get_hwd_mode(dithering_mode);
	} else {
		LOG("No hardware dithering requested");
	}
	// NOTE: Right now, we enforce quant_bit to what appears to be sane values depending on the waveform mode.

	struct mxcfb_rect region = {
		.top    = region_top,
		.left   = region_left,
		.width  = region_width,
		.height = region_height,
	};

	// If region is empty, do a full-screen refresh!
	if (region.top == 0U && region.left == 0U && region.width == 0U && region.height == 0U) {
		fullscreen_region(&region);
	}

	int ret;
	if (EXIT_SUCCESS != (ret = refresh(fbfd,
					   region,
					   WFM_AUTO, //get_wfm_mode(fbink_cfg->wfm_mode),
					   region_dither,
					   false,//fbink_cfg->is_nightmode,
					   false,//fbink_cfg->is_flashing,
					   false))) {
		WARN("Failed to refresh the screen");
	}

	if (!keep_fd) {
		close(fbfd);
	}

	return ret;
}
