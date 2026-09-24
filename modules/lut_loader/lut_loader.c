/*
 * lut_loader - load .mlut colour presets and drive the picture style pipeline
 *
 * A preset is a fixed 6194 byte record: a Q10 3x3 mixing matrix, one
 * 1024 point curve per channel and six axis hue/saturation trims.  See
 * mlut.h for the exact layout and tools/mlut_pack.py for the writer.
 *
 * Layering:
 *   mlut.h            protocol, constants, error codes
 *   mlut_parse.c      card access and validation
 *   mlut_map.c        preset -> camera blob mapping (offset tables live here)
 *   mlut_apply.c      property writes, backup, restore, change detection
 *   lut_loader.c      lifecycle, menu, directory scan (this file)
 *
 * The camera stores a whole custom picture style in one big property
 * whose internal layout was never reverse engineered by Magic Lantern
 * (only the style name at offset 4 is known, see src/picstyle.c).  Until
 * that layout has been confirmed on a real camera, writes go to the four
 * documented sliders instead; the Expo menu item "Dump camera data" plus
 * tools/mlut_diff.py is how the offsets get found.
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <menu.h>
#include <config.h>
#include <lens.h>
#include <notify_box.h>
#include <stdio.h>
#include <string.h>
#include <fio-ml.h>

#include "mlut.h"
#include "mlut_parse.h"
#include "mlut_map.h"
#include "mlut_apply.h"

/* ------------------------------------------------------------------ *
 * persistent settings
 * ------------------------------------------------------------------ */

static CONFIG_INT("lut_loader.enabled", lut_loader_enabled, 1);
static CONFIG_INT("lut_loader.preset", lut_loader_preset_index, 0);
static CONFIG_INT("lut_loader.slot", lut_loader_slot, MLUT_SLOT_DEFAULT);

/* ------------------------------------------------------------------ *
 * preset list
 * ------------------------------------------------------------------ */

static char       g_preset_files[MLUT_MAX_PRESETS][MLUT_PRESET_NAME_MAX];
static char       g_preset_labels[MLUT_MAX_PRESETS][MLUT_PRESET_NAME_MAX];
static const char *g_preset_choices[MLUT_MAX_PRESETS];

static int        g_preset_count = 0;
static int        g_scan_failed = 0;

/* The pickbox reads choices[] by index, so entry zero must always point
 * at a real string even when nothing was found on the card. */
static const char *g_no_preset_label = "(none)";

/* ------------------------------------------------------------------ *
 * menu
 * ------------------------------------------------------------------ */

enum
{
    MENU_ENABLED = 0,
    MENU_PRESET,
    MENU_RESCAN,
    MENU_APPLY,
    MENU_RESTORE,
    MENU_DUMP,
    MENU_SLOT,
    MENU_STATUS,
    MENU_COUNT
};

static MENU_SELECT_FUNC(lut_loader_rescan_select);
static MENU_SELECT_FUNC(lut_loader_apply_select);
static MENU_SELECT_FUNC(lut_loader_restore_select);
static MENU_SELECT_FUNC(lut_loader_dump_select);
static MENU_SELECT_FUNC(lut_loader_slot_select);
static MENU_SELECT_FUNC(lut_loader_status_select);

static MENU_UPDATE_FUNC(lut_loader_preset_update);
static MENU_UPDATE_FUNC(lut_loader_status_update);

static struct menu_entry g_menu[MENU_COUNT] =
{
    [MENU_ENABLED] =
    {
        .name       = "Enable module",
        .priv       = &lut_loader_enabled,
        .max        = 1,
        .icon_type  = IT_BOOL,
        .help       = "Keep the applied preset alive when the camera",
        .help2      = "switches picture styles back and forth.",
    },
    [MENU_PRESET] =
    {
        .name       = "Preset",
        .priv       = &lut_loader_preset_index,
        .min        = 0,
        .max        = 0,
        .choices    = g_preset_choices,
        .update     = lut_loader_preset_update,
        .icon_type  = IT_DICE,
        .help       = "Preset to use, from " MLUT_DIR,
    },
    [MENU_RESCAN] =
    {
        .name       = "Rescan presets",
        .select     = lut_loader_rescan_select,
        .icon_type  = IT_ACTION,
        .help       = "Re-read " MLUT_DIR " after copying new files.",
    },
    [MENU_APPLY] =
    {
        .name       = "Apply preset",
        .select     = lut_loader_apply_select,
        .icon_type  = IT_ACTION,
        .help       = "Write the selected preset to the managed slot",
        .help2      = "and switch the camera to that picture style.",
    },
    [MENU_RESTORE] =
    {
        .name       = "Restore original",
        .select     = lut_loader_restore_select,
        .icon_type  = IT_ACTION,
        .help       = "Put the camera's own style data back.",
    },
    [MENU_DUMP] =
    {
        .name       = "Dump camera data",
        .select     = lut_loader_dump_select,
        .icon_type  = IT_ACTION,
        .help       = "Export the raw slot contents to " MLUT_BACKUP_DIR,
        .help2      = "Use it with tools/mlut_diff.py to find offsets.",
    },
    [MENU_SLOT] =
    {
        .name       = "Managed slot",
        .priv       = &lut_loader_slot,
        .min        = 0,
        .max        = MLUT_SLOT_COUNT - 1,
        .choices    = CHOICES("UserDef1", "UserDef2", "UserDef3"),
        .select     = lut_loader_slot_select,
        .help       = "Which user defined picture style to own.",
    },
    [MENU_STATUS] =
    {
        .name       = "Status",
        .update     = lut_loader_status_update,
        .select     = lut_loader_status_select,
        .icon_type  = IT_ALWAYS_ON,
        .help       = "What the module has received from the camera.",
        .help2      = "Press SET to show the selected preset's contents.",
    },
};

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

static int lut_loader_selected_index(void)
{
    if (g_preset_count <= 0)
        return -1;

    if (lut_loader_preset_index < 0 || lut_loader_preset_index >= g_preset_count)
        lut_loader_preset_index = 0;

    return lut_loader_preset_index;
}

static void lut_loader_update_preset_menu(void)
{
    if (g_preset_count <= 0)
    {
        g_menu[MENU_PRESET].max = 0;
        lut_loader_preset_index = 0;
        return;
    }

    g_menu[MENU_PRESET].max = g_preset_count - 1;

    if (lut_loader_preset_index < 0 || lut_loader_preset_index >= g_preset_count)
        lut_loader_preset_index = 0;
}

static void lut_loader_make_path(int index, char *dst, int dst_len)
{
    dst[0] = '\0';

    if (index < 0 || index >= g_preset_count)
        return;

    mlut_build_path(MLUT_DIR, g_preset_files[index], dst, dst_len);
}

/* Strip a trailing .MLUT / .MLU so the menu label stays readable. */
static void lut_loader_label_from_file(const char *filename, char *dst, int dst_len)
{
    if (!dst || dst_len <= 0)
        return;

    snprintf(dst, dst_len, "%s", filename);

    int n = strlen(dst);

    if (n > 5 && streq(dst + n - 5, MLUT_EXT_LONG))
        dst[n - 5] = '\0';
    else if (n > 5 && streq(dst + n - 5, ".mlut"))
        dst[n - 5] = '\0';
    else if (n > 4 && streq(dst + n - 4, MLUT_EXT_SHORT))
        dst[n - 4] = '\0';
    else if (n > 4 && streq(dst + n - 4, ".mlu"))
        dst[n - 4] = '\0';
}

/* ------------------------------------------------------------------ *
 * backup status cache
 *
 * MENU_UPDATE_FUNC runs on every menu redraw.  MENU_SET_HELP() only
 * keeps its arguments when the entry is selected, but it evaluates them
 * first, so an is_file() call in there would put a card access on the
 * menu drawing path even while the entry is merely being scrolled past.
 * The answer depends only on the managed slot and on whether an apply has
 * happened, so it is cached and refreshed from exactly those events.
 * ------------------------------------------------------------------ */

static int g_backup_cache_slot   = -1;
static int g_backup_cache_exists = 0;

static int lut_loader_backup_exists(int slot)
{
    if (slot != g_backup_cache_slot)
    {
        g_backup_cache_exists = mlut_apply_backup_exists((unsigned) slot);
        g_backup_cache_slot = slot;
    }

    return g_backup_cache_exists;
}

static void lut_loader_backup_refresh(void)
{
    g_backup_cache_slot = -1;
}

/* ------------------------------------------------------------------ *
 * directory scan
 *
 * Only file names are collected here: parsing every preset would mean
 * reading a few hundred kB off the card, which is felt as a stall.  The
 * contents are parsed on demand from the Status item instead.
 * ------------------------------------------------------------------ */

static void lut_loader_scan_presets(void)
{
    g_preset_count = 0;
    g_scan_failed = 0;
    g_preset_choices[0] = g_no_preset_label;

    if (!is_dir(MLUT_DIR))
    {
        /* first run on a fresh card; the return code convention of
         * FIO_CreateDirectory is best not relied on, check instead */
        FIO_CreateDirectory(MLUT_DIR);

        if (!is_dir(MLUT_DIR))
        {
            g_scan_failed = 1;
            return;
        }
    }

    struct fio_file *file = alloc_fio_file();

    if (!file)
    {
        g_scan_failed = 1;
        return;
    }

    struct fio_dirent *dirent = FIO_FindFirstEx(MLUT_DIR, file);

    if (IS_ERROR(dirent))
    {
        free(file);
        g_scan_failed = 1;
        return;
    }

    do
    {
        struct file_info info = convert_fio_file_info(file);

        if (info.mode & ATTR_DIRECTORY)
            continue;

        if (!mlut_is_preset_name(info.name))
            continue;

        if (g_preset_count >= MLUT_MAX_PRESETS)
            break;

        int i = g_preset_count;

        snprintf(g_preset_files[i], sizeof(g_preset_files[i]), "%s", info.name);
        lut_loader_label_from_file(info.name, g_preset_labels[i], sizeof(g_preset_labels[i]));
        g_preset_choices[i] = g_preset_labels[i];

        g_preset_count++;
    }
    while (FIO_FindNextEx(dirent, file) == 0);

    FIO_FindClose(dirent);
    free(file);

    lut_loader_update_preset_menu();
}

/* ------------------------------------------------------------------ *
 * menu callbacks
 * ------------------------------------------------------------------ */

static MENU_UPDATE_FUNC(lut_loader_preset_update)
{
    int index = lut_loader_selected_index();

    if (index < 0)
    {
        MENU_SET_VALUE("none");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                         "Copy a .mlut file into " MLUT_DIR " and rescan.");
        return;
    }

    MENU_SET_VALUE("%s", g_preset_labels[index]);
    MENU_SET_HELP("%s%s", MLUT_DIR, g_preset_files[index]);
}

static MENU_UPDATE_FUNC(lut_loader_status_update)
{
    const mlut_slot_profile_t *p = mlut_profile((unsigned) lut_loader_slot);

    MENU_SET_VALUE("%s", p ? p->label : "?");

    if (!mlut_apply_supported())
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                         "Unsupported camera generation, writes are disabled.");
        return;
    }

    if (p && p->ready)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Style blob offsets confirmed.");
    }
    else
    {
        MENU_SET_WARNING(MENU_WARN_ADVICE,
                         "Offsets unprobed: presets only drive the sliders.");
    }

    MENU_SET_HELP("Camera data: %u bytes, backup: %s",
                  mlut_apply_blob_len((unsigned) lut_loader_slot),
                  lut_loader_backup_exists(lut_loader_slot) ? "yes" : "no");
}

static MENU_SELECT_FUNC(lut_loader_rescan_select)
{
    lut_loader_scan_presets();

    /* the user may have added or deleted a backup by hand */
    lut_loader_backup_refresh();

    if (g_scan_failed)
        NotifyBox(3000, "Cannot read " MLUT_DIR);
    else
        NotifyBox(2000, "Found %d preset(s)", g_preset_count);
}

static MENU_SELECT_FUNC(lut_loader_slot_select)
{
    if (lut_loader_slot < 0)
        lut_loader_slot = 0;

    if (lut_loader_slot >= MLUT_SLOT_COUNT)
        lut_loader_slot = MLUT_SLOT_COUNT - 1;

    mlut_apply_set_managed_slot((unsigned) lut_loader_slot);
}

static MENU_SELECT_FUNC(lut_loader_apply_select)
{
    int index = lut_loader_selected_index();

    if (index < 0)
    {
        NotifyBox(3000, "No preset in " MLUT_DIR);
        return;
    }

    char path[FIO_MAX_PATH_LENGTH];
    lut_loader_make_path(index, path, sizeof(path));

    ml_lut_preset_t *preset = malloc(sizeof(ml_lut_preset_t));

    if (!preset)
    {
        NotifyBox(3000, "Out of memory");
        return;
    }

    mlut_err_t err = mlut_load_file(path, preset);

    if (err == MLUT_OK)
        err = mlut_apply((unsigned) lut_loader_slot, preset);

    /* an apply may have just created the slot's first backup */
    lut_loader_backup_refresh();

    if (err != MLUT_OK)
    {
        NotifyBox(4000, "%s\n%s", g_preset_files[index], mlut_err_str(err));
    }
    else if (mlut_apply_used_fallback())
    {
        NotifyBox(4000, "Applied, sliders only\n%s\n(matrix and curves need "
                        "probed offsets)", g_preset_labels[index]);
    }
    else
    {
        NotifyBox(4000, "Applied\n%s", g_preset_labels[index]);
    }

    free(preset);
}

static MENU_SELECT_FUNC(lut_loader_restore_select)
{
    mlut_err_t err = mlut_restore((unsigned) lut_loader_slot);

    if (err == MLUT_OK)
    {
        NotifyBox(3000, "Original style data restored");
        return;
    }

    if (err == MLUT_ERR_NOBACKUP)
    {
        NotifyBox(4000, "No backup yet.\n%s",
                  mlut_apply_backup_name((unsigned) lut_loader_slot));
        return;
    }

    NotifyBox(4000, "Restore failed:\n%s", mlut_err_str(err));
}

static MENU_SELECT_FUNC(lut_loader_dump_select)
{
    unsigned slot = (unsigned) lut_loader_slot;
    char path[FIO_MAX_PATH_LENGTH];

    mlut_apply_probe_default_path(slot, path, sizeof(path));

    mlut_err_t err = mlut_apply_probe_dump(slot, path);

    if (err == MLUT_OK)
        NotifyBox(4000, "Written:\n%s", path);
    else if (err == MLUT_ERR_NOBLOB)
        NotifyBox(4000, "Camera has not sent slot data yet.\n"
                        "Power cycle, then try again.");
    else
        NotifyBox(4000, "Dump failed:\n%s", mlut_err_str(err));
}

/* Pressing SET on the Status item parses the selected preset and reports
 * what is inside it, without touching the camera. */
static MENU_SELECT_FUNC(lut_loader_status_select)
{
    int index = lut_loader_selected_index();

    if (index < 0)
    {
        NotifyBox(3000, "No preset in " MLUT_DIR);
        return;
    }

    char path[FIO_MAX_PATH_LENGTH];
    lut_loader_make_path(index, path, sizeof(path));

    ml_lut_preset_t *preset = malloc(sizeof(ml_lut_preset_t));

    if (!preset)
    {
        NotifyBox(3000, "Out of memory");
        return;
    }

    mlut_err_t err = mlut_load_file(path, preset);

    if (err != MLUT_OK)
    {
        NotifyBox(4000, "%s\n%s", g_preset_files[index], mlut_err_str(err));
        free(preset);
        return;
    }

    char name[MLUT_NAME_LEN + 1];
    mlut_name_of(preset, name, sizeof(name));

    NotifyBox(5000,
              "%s\n"
              "M %d %d %d / %d %d %d / %d %d %d\n"
              "R %u..%u  G %u..%u  B %u..%u\n"
              "size %u, profile %s",
              name,
              preset->matrix[0][0], preset->matrix[0][1], preset->matrix[0][2],
              preset->matrix[1][0], preset->matrix[1][1], preset->matrix[1][2],
              preset->matrix[2][0], preset->matrix[2][1], preset->matrix[2][2],
              (unsigned) preset->gamma_r[0],
              (unsigned) preset->gamma_r[MLUT_GAMMA_POINTS - 1],
              (unsigned) preset->gamma_g[0],
              (unsigned) preset->gamma_g[MLUT_GAMMA_POINTS - 1],
              (unsigned) preset->gamma_b[0],
              (unsigned) preset->gamma_b[MLUT_GAMMA_POINTS - 1],
              (unsigned) sizeof(ml_lut_preset_t),
              mlut_profile_is_ready((unsigned) lut_loader_slot) ? "ready" : "unprobed");

    free(preset);
}

/* ------------------------------------------------------------------ *
 * property subscriptions
 *
 * Two purposes: learn the real length of each property (a write whose
 * length does not match exactly is refused by src/property.c), and keep
 * a copy of the untouched data so it can be backed up and probed.
 * ------------------------------------------------------------------ */

PROP_HANDLER(PROP_PC_FLAVOR1_PARAM)
{
    mlut_apply_on_flavor_blob(0, buf, len);
}

PROP_HANDLER(PROP_PC_FLAVOR2_PARAM)
{
    mlut_apply_on_flavor_blob(1, buf, len);
}

PROP_HANDLER(PROP_PC_FLAVOR3_PARAM)
{
    mlut_apply_on_flavor_blob(2, buf, len);
}

PROP_HANDLER(MLUT_PROP_SETTINGS_USERDEF1)
{
    mlut_apply_on_settings_blob(0, buf, len);
}

PROP_HANDLER(MLUT_PROP_SETTINGS_USERDEF2)
{
    mlut_apply_on_settings_blob(1, buf, len);
}

PROP_HANDLER(MLUT_PROP_SETTINGS_USERDEF3)
{
    mlut_apply_on_settings_blob(2, buf, len);
}

/* ------------------------------------------------------------------ *
 * callbacks
 * ------------------------------------------------------------------ */

/* Runs every shoot task iteration, so it must stay very cheap: one
 * integer compare plus, at most once per style switch, one deferred
 * non blocking property push. */
static unsigned int lut_loader_shoot_cbr(unsigned int ctx)
{
    (void) ctx;

    mlut_apply_notify_picstyle(lens_info.raw_picstyle);

    if (lut_loader_enabled)
        mlut_apply_settle();

    return CBR_RET_CONTINUE;
}

/* ------------------------------------------------------------------ *
 * module lifecycle
 * ------------------------------------------------------------------ */

static unsigned int lut_loader_init(void)
{
    if (lut_loader_slot < 0 || lut_loader_slot >= MLUT_SLOT_COUNT)
        lut_loader_slot = MLUT_SLOT_DEFAULT;

    mlut_apply_init();
    mlut_apply_set_managed_slot((unsigned) lut_loader_slot);

    lut_loader_scan_presets();

    printf("lut_loader: %d preset(s) in " MLUT_DIR ", slot %d, %s\n",
           g_preset_count, lut_loader_slot,
           mlut_apply_supported() ? "writes enabled" : "writes DISABLED, unsupported body");

    menu_add("Expo", g_menu, COUNT(g_menu));

    return 0;
}

static unsigned int lut_loader_deinit(void)
{
    menu_remove("Expo", g_menu, COUNT(g_menu));
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(lut_loader_init)
    MODULE_DEINIT(lut_loader_deinit)
    MODULE_LONGNAME("LUT Loader")
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, lut_loader_shoot_cbr, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(lut_loader_enabled)
    MODULE_CONFIG(lut_loader_preset_index)
    MODULE_CONFIG(lut_loader_slot)
MODULE_CONFIGS_END()

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(PROP_PC_FLAVOR1_PARAM)
    MODULE_PROPHANDLER(PROP_PC_FLAVOR2_PARAM)
    MODULE_PROPHANDLER(PROP_PC_FLAVOR3_PARAM)
    MODULE_PROPHANDLER(MLUT_PROP_SETTINGS_USERDEF1)
    MODULE_PROPHANDLER(MLUT_PROP_SETTINGS_USERDEF2)
    MODULE_PROPHANDLER(MLUT_PROP_SETTINGS_USERDEF3)
MODULE_PROPHANDLERS_END()
