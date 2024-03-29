/*
 * Copyright © 2006 Eric Anholt
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef _INTEL_DVO_H
#define _INTEL_DVO_H

#include <linux/i2c.h>
#include "drmP.h"
#include "drm.h"
#include "drm_crtc.h"
#include "intel_drv.h"

struct intel_dvo_device {
	char *name;
	int type;
	/* DVOA/B/C output register */
	u32 dvo_reg;
	/* GPIO register used for i2c bus to control this device */
	u32 gpio;
	int slave_addr;
	struct i2c_adapter *i2c_bus;

	const struct intel_dvo_dev_ops *dev_ops;
	void *dev_priv;

	struct drm_display_mode *panel_fixed_mode;
	bool panel_wants_dither;
};

struct intel_dvo_dev_ops {
	/*
	 * Initialize the device at startup time.
	 * Returns NULL if the device does not exist.
	 */
	bool (*init)(struct intel_dvo_device *dvo,
		     struct i2c_adapter *i2cbus);

	/*
	 * Called to allow the output a chance to create properties after the
	 * RandR objects have been created.
	 */
	void (*create_resources)(struct intel_dvo_device *dvo);

	/*
	 * Turn on/off output or set intermediate power levels if available.
	 *
	 * Unsupported intermediate modes drop to the lower power setting.
	 * If the  mode is DPMSModeOff, the output must be disabled,
	 * as the DPLL may be disabled afterwards.
	 */
	void (*dpms)(struct intel_dvo_device *dvo, int mode);

	/*
	 * Callback for testing a video mode for a given output.
	 *
	 * This function should only check for cases where a mode can't
	 * be supported on the output specifically, and not represent
	 * generic CRTC limitations.
	 *
	 * \return MODE_OK if the mode is valid, or another MODE_* otherwise.
	 */
	int (*mode_valid)(struct intel_dvo_device *dvo,
			  struct drm_display_mode *mode);

	/*
	 * Callback to adjust the mode to be set in the CRTC.
	 *
	 * This allows an output to adjust the clock or even the entire set of
	 * timings, which is used for panels with fixed timings or for
	 * buses with clock limitations.
	 */
	bool (*mode_fixup)(struct intel_dvo_device *dvo,
			   struct drm_display_mode *mode,
			   struct drm_display_mode *adjusted_mode);

	/*
	 * Callback for preparing mode changes on an output
	 */
	void (*prepare)(struct intel_dvo_device *dvo);

	/*
	 * Callback for committing mode changes on an output
	 */
	void (*commit)(struct intel_dvo_device *dvo);

	/*
	 * Callback for setting up a video mode after fixups have been made.
	 *
	 * This is only called while the output is disabled.  The dpms callback
	 * must be all that's necessary for the output, to turn the output on
	 * after this function is called.
	 */
	void (*mode_set)(struct intel_dvo_device *dvo,
			 struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode);

	/*
	 * Probe for a connected output, and return detect_status.
	 */
	enum drm_connector_status (*detect)(struct intel_dvo_device *dvo);

	/**
	 * Query the device for the modes it provides.
	 *
	 * This function may also update MonInfo, mm_width, and mm_height.
	 *
	 * \return singly-linked list of modes or NULL if no modes found.
	 */
	struct drm_display_mode *(* const get_modes)(struct intel_dvo_device *dvo);

	/**
	 * Clean up driver-specific bits of the output
	 */
	void (* const destroy) (struct intel_dvo_device *dvo);

	/**
	 * Debugging hook to dump device registers to log file
	 */
	void (* const dump_regs)(struct intel_dvo_device *dvo);
};

extern const struct intel_dvo_dev_ops sil164_ops;
extern const struct intel_dvo_dev_ops ch7xxx_ops;
extern const struct intel_dvo_dev_ops ivch_ops;
extern const struct intel_dvo_dev_ops tfp410_ops;
extern const struct intel_dvo_dev_ops ch7017_ops;

#endif /* _INTEL_DVO_H */
