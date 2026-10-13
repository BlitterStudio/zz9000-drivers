/*
 * ZZTop System page: boot-time card settings and their ENV overrides.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <gadgets/chooser.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <gadgets/string.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <proto/checkbox.h>
#include <proto/chooser.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/space.h>
#include <proto/string.h>

#include <stdio.h>
#include <string.h>

#include "zztop.h"
#include "board.h"
#include "settings.h"
#include "ui.h"

#define P PAGE_SYSTEM

enum {
	G_INT = 1,
	G_INT_USE_ENV,
	G_INT_REMOVE_ENV,
	G_MAC_BUILTIN,
	G_MAC,
	G_MAC_USE_ENV,
	G_MAC_REMOVE_ENV,
	G_HDF_ENABLE,
	G_HDF,
	G_OFFSCREEN,
	G_OVERLAY,
	G_FAST_RAM
};

static const char *const int_labels[] = { "INT6 (default)", "INT2", NULL };
static struct List *int_choices;
static Object *g_int, *g_int_use, *g_int_remove;
static Object *g_mac_builtin, *g_mac, *g_mac_use, *g_mac_remove;
static Object *g_hdf_enable, *g_hdf, *g_offscreen, *g_overlay, *g_fast_ram;
static struct ui_text t_supported, t_int_help, t_int_env, t_mac_env;
static struct ui_text t_fast_ram, t_fast_ram_reason;
static char mac_text[ZZCFG_MAC_CHARS + 3];
static char hdf_text[ZZCFG_HDF_CHARS + 5];
static BOOL mac_custom;

static Object *check(UWORD id, const char *text)
{
	return CheckBoxObject,
		GA_ID, UI_GID(P, id),
		GA_RelVerify, TRUE,
		GA_Text, text,
	End;
}
static Object *button(UWORD id, const char *text)
{
	return NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, id),
		GA_RelVerify, TRUE,
		GA_Text, text,
	End;
}

static const char *fast_ram_outcome(UWORD outcome)
{
	switch (outcome) {
	case ZZ_CFG_FAST_RAM_OUTCOME_OFF: return "off";
	case ZZ_CFG_FAST_RAM_OUTCOME_ABSENT: return "not configured";
	case ZZ_CFG_FAST_RAM_OUTCOME_INVALID: return "invalid configuration";
	case ZZ_CFG_FAST_RAM_OUTCOME_TRUNCATED: return "truncated configuration";
	case ZZ_CFG_FAST_RAM_OUTCOME_MEDIA_ERR: return "SD card error";
	case ZZ_CFG_FAST_RAM_OUTCOME_TIMEOUT: return "boot timeout";
	default: return "boot gate";
	}
}

/* Copy the MAC gadget into the model (string gadgets only report Enter,
 * so this also runs from tick() and flush()). */
static void commit_mac(void)
{
	const char *text;

	if (!cfg.supported) return;
	text = (const char *)ui_get(g_mac, STRINGA_TextVal);
	if (ui_get(g_mac_builtin, CHECKBOX_Checked))
		settings_clear(CFG_MAC);
	else if (text && text[0])
		settings_set_text(CFG_MAC, text);
	else
		settings_reset(CFG_MAC);
	ui_pending_changed();
}

/* Copy the SD boot controls into the model. Unchecked saves `hdf = off`;
 * an empty name or the firmware's default name over an absent key keeps
 * the key absent (zzcfg_hdf_from_gadget needs the saved value as input). */
static void commit_hdf(void)
{
	static char stored[ZZCFG_HDF_CHARS + 5];
	const char *text;

	if (!cfg.supported) return;
	text = (const char *)ui_get(g_hdf, STRINGA_TextVal);
	if (!text || !text[0]) text = ZZCFG_HDF_DEFAULT;
	if (!ui_get(g_hdf_enable, CHECKBOX_Checked)) {
		settings_set_text(CFG_HDF, ZZCFG_HDF_OFF);
	} else {
		snprintf(stored, sizeof(stored), "%s", cfg.saved.hdf);
		zzcfg_hdf_from_gadget(stored, sizeof(stored), text);
		if (stored[0])
			settings_set_text(CFG_HDF, stored);
		else
			settings_clear(CFG_HDF);
	}
	ui_pending_changed();
}

static void sync_controls(void)
{
	BOOL enabled = cfg.supported;
	BOOL builtin;
	const char *hdf = zzcfg_hdf_gadget_text(cfg.edit.hdf);
	BOOL hdf_enabled = hdf[0] != '\0';
	BOOL fast_available = cfg.fast_ram_state != ZZCFG_FAST_RAM_UNAVAILABLE;

	if (cfg.edit.mac[0]) mac_custom = FALSE;
	builtin = !mac_custom && cfg.edit.mac[0] == '\0';

	snprintf(mac_text, sizeof(mac_text), "%s", cfg.edit.mac);
	snprintf(hdf_text, sizeof(hdf_text), "%s", hdf);
	ui_set(P, g_int, CHOOSER_Selected, cfg.edit.int2 ? 1 : 0, TAG_END);
	ui_set(P, g_mac_builtin, CHECKBOX_Checked, builtin, TAG_END);
	ui_set(P, g_mac, STRINGA_TextVal, (ULONG)mac_text, TAG_END);
	ui_set(P, g_hdf_enable, CHECKBOX_Checked, hdf_enabled, TAG_END);
	ui_set(P, g_hdf, STRINGA_TextVal, (ULONG)hdf_text, TAG_END);
	ui_set(P, g_offscreen, CHECKBOX_Checked, cfg.edit.offscreen_bitmaps,
		TAG_END);
	ui_set(P, g_overlay, CHECKBOX_Checked, cfg.edit.video_overlay, TAG_END);
	ui_set(P, g_fast_ram, CHECKBOX_Checked, cfg.edit.fast_ram, TAG_END);

	ui_disable(P, g_int, !enabled);
	ui_disable(P, g_int_use, !enabled || !(cfg.env & CFG_ENV_INT2));
	ui_disable(P, g_int_remove, !enabled || !(cfg.env & CFG_ENV_INT2));
	ui_disable(P, g_mac_builtin, !enabled);
	ui_disable(P, g_mac, !enabled || builtin);
	ui_disable(P, g_mac_use, !enabled || !(cfg.env & CFG_ENV_MAC));
	ui_disable(P, g_mac_remove, !enabled || !(cfg.env & CFG_ENV_MAC));
	ui_disable(P, g_hdf_enable, !enabled);
	ui_disable(P, g_hdf, !enabled || !hdf_enabled);
	ui_disable(P, g_offscreen, !enabled);
	ui_disable(P, g_overlay, !enabled);
	ui_disable(P, g_fast_ram, !enabled || !fast_available);

	if (!enabled)
		ui_text_set(P, &t_supported,
			"These settings need firmware 2.3 or newer.");
	else
		ui_text_set(P, &t_supported,
			"All settings on this page take effect at the next power-cycle.");
	if (cfg.env & CFG_ENV_INT2)
		ui_text_set(P, &t_int_env, "ENV:ZZ9K_INT2 overrides this.");
	else
		ui_text_set(P, &t_int_env, "");
	if (cfg.env & CFG_ENV_MAC)
		ui_text_printf(P, &t_mac_env, "ENV:ZZ9K_MAC overrides this (%s).",
			cfg.env_mac);
	else if (!builtin && mac_text[0] && !settings_mac_valid(mac_text))
		ui_text_set(P, &t_mac_env,
			"Use aa:bb:cc:dd:ee:ff (or use the card's address).");
	else
		ui_text_set(P, &t_mac_env, "");

	if (!enabled) {
		ui_text_set(P, &t_fast_ram, "Effective: unavailable (no config support)");
		ui_text_set(P, &t_fast_ram_reason, "");
	} else if (!fast_available) {
		ui_text_set(P, &t_fast_ram, "Effective: unavailable on this firmware");
		ui_text_set(P, &t_fast_ram_reason,
			"Fast RAM needs firmware support for the boot outcome.");
	} else if (cfg.fast_ram_state == ZZCFG_FAST_RAM_ENABLED) {
		ui_text_printf(P, &t_fast_ram, "Effective: enabled%s",
			cfg.fast_ram_outcome == ZZ_CFG_FAST_RAM_OUTCOME_BAK_ON ?
			" (backup)" : "");
		ui_text_set(P, &t_fast_ram_reason, "");
	} else if (cfg.fast_ram_state == ZZCFG_FAST_RAM_WITHHELD) {
		ui_text_printf(P, &t_fast_ram,
			"Effective: configured on, withheld (%s)",
			fast_ram_outcome(cfg.fast_ram_outcome));
		ui_text_set(P, &t_fast_ram_reason,
			"The boot gate withheld the configured memory for safety.");
	} else {
		ui_text_printf(P, &t_fast_ram, "Effective: disabled (%s)",
			fast_ram_outcome(cfg.fast_ram_outcome));
		ui_text_set(P, &t_fast_ram_reason, "");
	}
}

static Object *create(void)
{
	int_choices = ui_chooser_labels(int_labels);
	if (!int_choices) return NULL;
	return VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, ui_note_new(&t_supported, ""),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Interrupt",
			LAYOUT_AddChild, (g_int = ChooserObject,
				GA_ID, UI_GID(P, G_INT), GA_RelVerify, TRUE,
				CHOOSER_Labels, (ULONG)int_choices, CHOOSER_PopUp, TRUE,
			End),
			Label("Interrupt"),
			LAYOUT_AddChild, ui_note_new(&t_int_help,
				"Use INT2 only when INT6 locks up; set the card's J4 jumper to match."),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ui_note_new(&t_int_env, ""),
				LAYOUT_AddChild, (g_int_use = button(G_INT_USE_ENV, "_Use it")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, (g_int_remove = button(G_INT_REMOVE_ENV, "_Remove")),
				CHILD_WeightedWidth, 0,
			End,
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Network",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, (g_mac_builtin = check(G_MAC_BUILTIN,
					"Use the card's built-in address")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, (g_mac = StringObject,
					GA_ID, UI_GID(P, G_MAC), GA_RelVerify, TRUE,
					STRINGA_MaxChars, ZZCFG_MAC_CHARS + 1,
					STRINGA_TextVal, (ULONG)mac_text,
				End),
			End,
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ui_note_new(&t_mac_env, ""),
				LAYOUT_AddChild, (g_mac_use = button(G_MAC_USE_ENV, "Use _it")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, (g_mac_remove = button(G_MAC_REMOVE_ENV, "R_emove")),
				CHILD_WeightedWidth, 0,
			End,
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "SD card boot",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, (g_hdf_enable = check(G_HDF_ENABLE,
					"Boot from an SD card image")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, (g_hdf = StringObject,
					GA_ID, UI_GID(P, G_HDF), GA_RelVerify, TRUE,
					STRINGA_MaxChars, ZZCFG_HDF_CHARS + 1,
					STRINGA_TextVal, (ULONG)hdf_text,
				End),
			End,
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Graphics",
			LAYOUT_AddChild, (g_offscreen = check(G_OFFSCREEN,
				"Off-screen bitmaps in card memory (for incompatible software)")),
			LAYOUT_AddChild, (g_overlay = check(G_OVERLAY,
				"Video overlay window (needed by in-window ZZPlay)")),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Memory",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, (g_fast_ram = check(G_FAST_RAM,
					"Zorro III Fast RAM")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, ui_text_new(&t_fast_ram, ""),
			End,
			LAYOUT_AddChild, ui_note_new(&t_fast_ram_reason, ""),
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

static void show(void)
{
	sync_controls();
}

static void flush(void)
{
	commit_mac();
	commit_hdf();
}

static void hide(void)
{
	flush();
}

/* Keep typed text in the model while the user types, so Save enables
 * without pressing Enter. */
static void tick(void)
{
	if (ui_ticks() % 5 == 0) flush();
}

static void gadget(UWORD id, UWORD code)
{
	(void)code;
	switch (id) {
	case G_INT:
		settings_set(CFG_INT2, ui_get(g_int, CHOOSER_Selected) != 0);
		break;
	case G_MAC_BUILTIN:
		mac_custom = !ui_get(g_mac_builtin, CHECKBOX_Checked);
		commit_mac();
		break;
	case G_MAC:
		commit_mac();
		break;
	case G_HDF_ENABLE:
		commit_hdf();
		break;
	case G_HDF:
		commit_hdf();
		break;
	case G_OFFSCREEN:
		settings_set(CFG_OFFSCREEN,
			ui_get(g_offscreen, CHECKBOX_Checked) != 0);
		break;
	case G_OVERLAY:
		settings_set(CFG_OVERLAY, ui_get(g_overlay, CHECKBOX_Checked) != 0);
		break;
	case G_FAST_RAM:
		settings_set(CFG_FAST_RAM, ui_get(g_fast_ram, CHECKBOX_Checked) != 0);
		break;
	case G_INT_USE_ENV:
		settings_env_adopt(CFG_ENV_INT2);
		app_models_changed();
		return;
	case G_MAC_USE_ENV:
		settings_env_adopt(CFG_ENV_MAC);
		app_models_changed();
		return;
	case G_INT_REMOVE_ENV:
		if (ui_ask("Remove|Cancel",
			"Remove ENV:ZZ9K_INT2 and its ENVARC: copy?") == 1) {
			settings_env_remove(CFG_ENV_INT2);
			app_models_changed();
		}
		return;
	case G_MAC_REMOVE_ENV:
		if (ui_ask("Remove|Cancel",
			"Remove ENV:ZZ9K_MAC and its ENVARC: copy?") == 1) {
			settings_env_remove(CFG_ENV_MAC);
			app_models_changed();
		}
		return;
	default:
		return;
	}
	ui_pending_changed();
	sync_controls();
}

static void sync(void)
{
	sync_controls();
}
static void cleanup(void)
{
	ui_chooser_free_labels(int_choices);
	int_choices = NULL;
}

struct ui_page page_system = {
	"System",
	"System settings",
	"Boot-time interrupt, network, SD boot, graphics and memory settings.",
	0,
	create,
	show,
	hide,
	gadget,
	tick,
	sync,
	flush,
	cleanup,
};
