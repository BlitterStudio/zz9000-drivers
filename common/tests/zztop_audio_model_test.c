/*
 * Host test for how ZZTop's audio editor model takes the firmware control
 * state (ZZTop/Core/audio_model.h): every reported field must replace the
 * CFG-seeded value. A live Separation edit of 54 once came back as the
 * saved 100 after ZZTop was reopened because the field was not installed.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

typedef int BOOL;
#include "audio_model.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

int main(void)
{
	struct audio_model model;
	struct audio_model live;

	memset(&model, 0, sizeof(model));
	memset(&live, 0, sizeof(live));
	/* CFG-seeded entry state. */
	model.active = 1;
	model.base_paula = 36;
	model.base_ax = 72;
	model.ceiling_paula = 48;
	model.ceiling_ax = 80;
	model.separation = 100;
	model.scenes[2].lpf = 16000;
	/* Live state: everything differs, separation was dragged to 54. */
	live.active = 4;
	live.base_paula = 20;
	live.base_ax = 40;
	live.ceiling_paula = 512;
	live.ceiling_ax = 1024;
	live.separation = 54;
	live.scenes[2].lpf = 1;

	audio_model_install_live(&model, &live);
	check(model.active == 4, "active scene installed");
	check(model.base_paula == 20 && model.base_ax == 40,
		"baseline installed");
	check(model.ceiling_paula == 512 && model.ceiling_ax == 1024,
		"ceilings installed");
	check(model.separation == 54, "live separation installed");
	check(model.scenes[2].lpf == 16000,
		"scenes keep their CFG-seeded values");

	if (failures)
		return 1;
	puts("zztop_audio_model_test: all checks passed");
	return 0;
}
