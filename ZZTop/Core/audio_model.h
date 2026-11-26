/*
 * ZZTop audio editor model and the rule for taking firmware state into it.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_AUDIO_MODEL_H
#define ZZTOP_AUDIO_MODEL_H

#include <exec/types.h>

#include "audio.h"

struct audio_model {
	struct audio_scene scenes[AUDIO_SCENE_COUNT];
	UWORD active;
	UWORD base_paula;
	UWORD base_ax;
	UWORD ceiling_paula;
	UWORD ceiling_ax;
	UWORD separation;
};

/*
 * Installs the fields the firmware control state reports. Scenes have no
 * firmware readback and stay as seeded from ZZ9000.CFG. Every state field
 * must be copied here: one that is missed silently keeps its saved CFG
 * value, which is how a live separation edit once reappeared as 100 after
 * ZZTop was reopened.
 */
static inline void audio_model_install_live(struct audio_model *model,
	const struct audio_model *live)
{
	model->active = live->active;
	model->base_paula = live->base_paula;
	model->base_ax = live->base_ax;
	model->ceiling_paula = live->ceiling_paula;
	model->ceiling_ax = live->ceiling_ax;
	model->separation = live->separation;
}

#endif /* ZZTOP_AUDIO_MODEL_H */
