/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <inttypes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include "log.h"
#include "shl_hook.h"
#include "shl_misc.h"
#include "uterm.h"
#include "uterm_input.h"

#define LOG_SUBSYSTEM "input_uxkb"

int uxkb_desc_init(struct uterm_input *input,
		   const char *layout,
		   const char *variant,
		   const char *options)
{
	int ret;
	struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = "evdev",
		.layout = layout,
		.variant = variant,
		.options = options,
	};

	input->ctx = xkb_context_new(0);
	if (!input->ctx) {
		log_error("cannot create XKB context");
		return -ENOMEM;
	}

	input->keymap = xkb_map_new_from_names(input->ctx, &rmlvo, 0);
	if (!input->keymap) {
		log_warn("failed to create keymap (%s, %s, %s), "
			 "reverting to default US keymap",
			 layout, variant, options);

		rmlvo.layout = "us";
		rmlvo.variant = "";
		rmlvo.options = "";

		input->keymap = xkb_map_new_from_names(input->ctx, &rmlvo, 0);
		if (!input->keymap) {
			log_warn("failed to create XKB keymap");
			ret = -EFAULT;
			goto err_ctx;
		}
	}

	log_debug("new keyboard description (%s, %s, %s)",
		  layout, variant, options);
	return 0;

err_ctx:
	xkb_context_unref(input->ctx);
	return ret;
}

void uxkb_desc_destroy(struct uterm_input *input)
{
	xkb_map_unref(input->keymap);
	xkb_context_unref(input->ctx);
}

static void timer_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct uterm_input_dev *dev = data;

	dev->repeat_event.handled = false;
	shl_hook_call(dev->input->hook, dev->input, &dev->repeat_event);
}

int uxkb_dev_init(struct uterm_input_dev *dev)
{
	int ret;

	ret = ev_eloop_new_timer(dev->input->eloop, &dev->repeat_timer, NULL,
				 timer_event, dev);
	if (ret)
		return ret;

	dev->state = xkb_state_new(dev->input->keymap);
	if (!dev->state) {
		log_error("cannot create XKB state");
		ret = -ENOMEM;
		goto err_timer;
	}

	return 0;

err_timer:
	ev_eloop_rm_timer(dev->repeat_timer);
	return ret;
}

void uxkb_dev_destroy(struct uterm_input_dev *dev)
{
	xkb_state_unref(dev->state);
	ev_eloop_rm_timer(dev->repeat_timer);
}

#define EVDEV_KEYCODE_OFFSET 8
enum {
	KEY_RELEASED = 0,
	KEY_PRESSED = 1,
	KEY_REPEATED = 2,
};

int uxkb_dev_process(struct uterm_input_dev *dev,
		     uint16_t key_state, uint16_t code)
{
	struct xkb_state *state;
	struct xkb_keymap *keymap;
	xkb_keycode_t keycode;
	const xkb_keysym_t *keysyms;
	int num_keysyms, i;
	uint32_t *tmp;
	struct itimerspec spec;

	if (key_state == KEY_REPEATED)
		return -ENOKEY;

	state = dev->state;
	keymap = xkb_state_get_map(state);
	keycode = code + EVDEV_KEYCODE_OFFSET;

	num_keysyms = xkb_key_get_syms(state, keycode, &keysyms);

	if (key_state == KEY_PRESSED)
		xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
	else if (key_state == KEY_RELEASED)
		xkb_state_update_key(state, keycode, XKB_KEY_UP);

	if (num_keysyms <= 0)
		return -ENOKEY;

	if (dev->num_syms < num_keysyms) {
		tmp = realloc(dev->event.keysyms,
			      sizeof(uint32_t) * num_keysyms);
		if (!tmp) {
			log_warning("cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->event.keysyms = tmp;

		tmp = realloc(dev->event.codepoints,
			      sizeof(uint32_t) * num_keysyms);
		if (!tmp) {
			log_warning("cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->event.codepoints = tmp;

		tmp = realloc(dev->repeat_event.keysyms,
			      sizeof(uint32_t) * num_keysyms);
		if (!tmp) {
			log_warning("cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->repeat_event.keysyms = tmp;

		tmp = realloc(dev->repeat_event.codepoints,
			      sizeof(uint32_t) * num_keysyms);
		if (!tmp) {
			log_warning("cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->repeat_event.codepoints = tmp;

		dev->num_syms = num_keysyms;
	}

	dev->event.handled = false;
	dev->event.keycode = code;
	dev->event.ascii = shl_get_ascii(state, keycode, keysyms, num_keysyms);
	dev->event.mods = shl_get_xkb_mods(state);
	dev->event.num_syms = num_keysyms;
	memcpy(dev->event.keysyms, keysyms, sizeof(uint32_t) * num_keysyms);

	for (i = 0; i < num_keysyms; ++i) {
		dev->event.codepoints[i] = xkb_keysym_to_utf32(keysyms[i]);
		if (!dev->event.codepoints[i])
			dev->event.codepoints[i] = UTERM_INPUT_INVALID;
	}

	if (key_state == KEY_RELEASED &&
	    dev->repeat_event.num_syms == num_keysyms &&
	    !memcmp(dev->repeat_event.keysyms,
	            keysyms,
	            sizeof(uint32_t) * num_keysyms)) {
		ev_timer_update(dev->repeat_timer, NULL);
	} else if (key_state == KEY_PRESSED &&
		   xkb_key_repeats(keymap, keycode)) {
		dev->repeat_event.keycode = code;
		dev->repeat_event.ascii = dev->event.ascii;
		dev->repeat_event.mods = dev->event.mods;
		dev->repeat_event.num_syms = num_keysyms;

		for (i = 0; i < num_keysyms; ++i) {
			dev->repeat_event.keysyms[i] = dev->event.keysyms[i];
			dev->repeat_event.codepoints[i] =
						dev->event.codepoints[i];
		}

		spec.it_interval.tv_sec = 0;
		spec.it_interval.tv_nsec = dev->input->repeat_rate * 1000000;
		spec.it_value.tv_sec = 0;
		spec.it_value.tv_nsec = dev->input->repeat_delay * 1000000;
		ev_timer_update(dev->repeat_timer, &spec);
	}

	if (key_state == KEY_RELEASED)
		return -ENOKEY;

	shl_hook_call(dev->input->hook, dev->input, &dev->event);

	return 0;
}

/*
 * Call this when we regain control of the keyboard after losing it.
 * We don't reset the locked group, this should survive a VT switch, etc. The
 * locked modifiers are reset according to the keyboard LEDs.
 */
void uxkb_dev_reset(struct uterm_input_dev *dev, const unsigned long *ledbits)
{
	unsigned int i;
	struct xkb_state *state;
	static const struct {
		int led;
		const char *name;
	} led_names[] = {
		{ LED_NUML, XKB_LED_NAME_NUM },
		{ LED_CAPSL, XKB_LED_NAME_CAPS },
		{ LED_SCROLLL, XKB_LED_NAME_SCROLL },
	};

	/* TODO: Urghs, while the input device was closed we might have missed
	 * some events that affect internal state. As xkbcommon does not provide
	 * a way to reset the internal state, we simply recreate the state. This
	 * should have the same effect.
	 * It also has a bug that if the CTRL-Release event is skipped, then
	 * every further release will never perform a _real_ release. Kind of
	 * buggy so we should fix it upstream. */
	state = xkb_state_new(dev->input->keymap);
	if (!state) {
		log_warning("cannot recreate xkb-state");
		return;
	}
	xkb_state_unref(dev->state);
	dev->state = state;

	for (i = 0; i < sizeof(led_names) / sizeof(*led_names); i++) {
		if (!input_bit_is_set(ledbits, led_names[i].led))
			continue;

		/*
		 * TODO: Add support in xkbcommon for setting the led state,
		 * and updating the modifier state accordingly. E.g., something
		 * like this:
		 *	xkb_state_led_name_set_active(state, led_names[i].led);
		 */
	}
}
