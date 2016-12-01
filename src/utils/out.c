/*
 * Copyright © 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel van Vugt <daniel.van.vugt@canonical.com>
 */

#include "mir_toolkit/mir_client_library.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

static const char *power_mode_name(MirPowerMode m)
{
    /* XXX it would be safer to define these strings in the client header */
    static const char * const name[] =
    {
        "on",
        "standby",
        "suspended",
        "off"
    };
    return ((unsigned)m < sizeof(name)/sizeof(name[0])) ? name[m] : "unknown";
}

static const char *orientation_name(MirOrientation ori)
{
    static const char * const name[] =
    {
        "normal",
        "left",
        "inverted",
        "right"
    };
    return name[(ori % 360) / 90];
}

static char const* state_name(MirOutputConnectionState s)
{
    static char const* const name[] =
    {
        "disconnected",
        "connected",
        "unknown",
    };
    unsigned int u = s;
    return u < 3 ? name[u] : "out-of-range";
}

static char const* subpixel_name(MirSubpixelArrangement s)
{
    static char const* const name[] =
    {
        "unknown",
        "HRGB",
        "HBGR",
        "VRGB",
        "VBGR",
    };
    return ((unsigned)s < sizeof(name)/sizeof(name[0])) ? name[s]
                                                        : "out-of-range";
}

static char const* form_factor_name(MirFormFactor f)
{
    static char const* const name[] =
    {
        "unknown",
        "phone",
        "tablet",
        "monitor",
        "TV",
        "projector",
    };
    return ((unsigned)f < sizeof(name)/sizeof(name[0])) ? name[f]
                                                        : "out-of-range";
}

static bool modify(MirDisplayConfig* conf, int actionc, char** actionv)
{
    int const max_targets = 256;
    MirOutput* target[max_targets];
    int targets = 0;

    /* Until otherwise specified we apply actions to all outputs */
    int num_outputs = mir_display_config_get_num_outputs(conf);
    if (num_outputs > max_targets)
        num_outputs = max_targets;
    for (int i = 0; i < num_outputs; ++i)
    {
        MirOutput const* out = mir_display_config_get_output(conf, i);
        if (mir_output_connection_state_connected ==
            mir_output_get_connection_state(out))
            target[targets++] = mir_display_config_get_mutable_output(conf, i);
    }

    char** action_end = actionv + actionc;
    for (char** action = actionv; action < action_end; ++action)
    {
        if (!strcmp(*action, "output"))
        {
            int output_id;
            if (++action < action_end && 1 == sscanf(*action, "%d", &output_id))
            {
                targets = 0;
                for (int i = 0; i < num_outputs; ++i)
                {
                    MirOutput* out =
                        mir_display_config_get_mutable_output(conf, i);
                    if (output_id == mir_output_get_id(out))
                    {
                        targets = 1;
                        target[0] = out;
                        break;
                    }
                }
                if (!targets)
                {
                    fprintf(stderr, "Output ID `%s' not found\n", *action);
                    return false;
                }
            }
            else
            {
                if (action >= action_end)
                    fprintf(stderr, "Missing output ID after `%s'\n",
                            action[-1]);
                else
                    fprintf(stderr, "Invalid output ID `%s'\n", *action);
                return false;
            }
        }
        else if (!strcmp(*action, "off"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_set_power_mode(target[t], mir_power_mode_off);
        }
        else if (!strcmp(*action, "on"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_set_power_mode(target[t], mir_power_mode_on);
        }
        else if (!strcmp(*action, "standby"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_set_power_mode(target[t], mir_power_mode_standby);
        }
        else if (!strcmp(*action, "suspend"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_set_power_mode(target[t], mir_power_mode_suspend);
        }
        else if (!strcmp(*action, "enable"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_enable(target[t]);
        }
        else if (!strcmp(*action, "disable"))
        {
            for (int t = 0; t < targets; ++t)
                mir_output_disable(target[t]);
        }
        else if (!strcmp(*action, "rotate"))
        {
            if (++action >= action_end)
            {
                fprintf(stderr, "Missing parameter after `%s'\n", action[-1]);
                return false;
            }
            enum {orientations = 4};
            static const MirOrientation orientation[orientations] =
            {
                mir_orientation_normal,
                mir_orientation_left,
                mir_orientation_inverted,
                mir_orientation_right,
            };

            int i;
            for (i = 0; i < orientations; ++i)
                if (!strcmp(*action, orientation_name(orientation[i])))
                    break;

            if (i >= orientations)
            {
                fprintf(stderr, "Unknown rotation `%s'\n", *action);
                return false;
            }
            else
            {
                for (int t = 0; t < targets; ++t)
                    mir_output_set_orientation(target[t], orientation[i]);
            }
        }
        else if (!strcmp(*action, "place"))
        {
            int x, y;
            if (++action >= action_end)
            {
                fprintf(stderr, "Missing placement parameter after `%s'\n",
                        action[-1]);
                return false;
            }
            else if (2 != sscanf(*action, "%d%d", &x, &y))
            {
                fprintf(stderr, "Invalid placement `%s'\n", *action);
                return false;
            }
            else
            {
                for (int t = 0; t < targets; ++t)
                    mir_output_set_position(target[t], x, y);
            }
        }
        else if (!strcmp(*action, "mode") || !strcmp(*action, "rate"))
        {
            bool have_rate = !strcmp(*action, "rate");
            if (++action >= action_end)
            {
                fprintf(stderr, "Missing parameter after `%s'\n", action[-1]);
                return false;
            }

            int w = -1, h = -1;
            char target_hz[64] = "";

            if (!have_rate)
            {
                if (strcmp(*action, "preferred") &&
                    2 != sscanf(*action, "%dx%d", &w, &h))
                {
                    fprintf(stderr, "Invalid dimensions `%s'\n", *action);
                    return false;
                }

                if (action+2 < action_end && !strcmp(action[1], "rate"))
                {
                    have_rate = true;
                    action += 2;
                }
            }

            if (have_rate)
            {
                if (1 != sscanf(*action, "%63[0-9.]", target_hz))
                {
                    fprintf(stderr, "Invalid refresh rate `%s'\n", *action);
                    return false;
                }
                else if (!strchr(target_hz, '.'))
                {
                    size_t len = strlen(target_hz);
                    if (len < (sizeof(target_hz)-4))
                        snprintf(target_hz+len, 4, ".00");
                }
            }

            for (int t = 0; t < targets; ++t)
            {
                MirOutputMode const* set_mode = NULL;
                MirOutputMode const* preferred =
                    mir_output_get_preferred_mode(target[t]);

                if (w <= 0 && !target_hz[0])
                {
                    set_mode = preferred;
                }
                else
                {
                    if (w <= 0)
                    {
                        w = mir_output_mode_get_width(preferred);
                        h = mir_output_mode_get_height(preferred);
                    }
                    int const num_modes = mir_output_get_num_modes(target[t]);
                    for (int m = 0; m < num_modes; ++m)
                    {
                        MirOutputMode const* mode =
                            mir_output_get_mode(target[t], m);
                        if (w == mir_output_mode_get_width(mode) &&
                            h == mir_output_mode_get_height(mode))
                        {
                            if (!target_hz[0])
                            {
                                set_mode = mode;
                                break;
                            }
                            char hz[64];
                            snprintf(hz, sizeof hz, "%.2f",
                                mir_output_mode_get_refresh_rate(mode));
                            if (!strcmp(target_hz, hz))
                            {
                                set_mode = mode;
                                break;
                            }
                        }
                    }
                }

                if (set_mode)
                {
                    mir_output_set_current_mode(target[t], set_mode);
                }
                else
                {
                    fprintf(stderr, "No matching mode for `%s'\n", *action);
                    return false;
                }
            }
        }
        else
        {
            fprintf(stderr, "Unrecognized action `%s'\n", *action);
            return false;
        }
    }
    return true;
}

enum descriptor_type
{
    monitor_serial_number = 0xff,
    unspecified_text = 0xfe,
    monitor_name = 0xfc
};

union descriptor
{
    struct
    {
        uint16_t pixel_clock;
    } detailed_timing;
    struct
    {
        uint16_t zero0;
        uint8_t  zero2;
        uint8_t  type; /* enum descriptor_type */
        uint8_t  zero4;
        char     text[13];
    } other;
};

static int edid_get_descriptor(uint8_t const* edid, enum descriptor_type type,
                               char str[14])
{
    union descriptor const* desc = (union descriptor const*)(edid + 54);
    union descriptor const* desc_end = desc + 4;
    int len = 0;
    for (; desc < desc_end; ++desc)
    {
        if (!desc->detailed_timing.pixel_clock)
        {
            if (desc->other.type == (uint8_t)type)
            {
                memcpy(str, desc->other.text, 13);
                /* Standard padding (if any) is 0x0a 0x20 0x20... */
                char* lf = strchr(str, '\n');
                len = lf ? lf - str : 13;
                break;
            }
        }
    }
    str[len] = '\0';
    return len;
}

int main(int argc, char *argv[])
{
    char const* server = NULL;
    char** actionv = NULL;
    int actionc = 0;
    bool verbose = false;

    for (int a = 1; a < argc; a++)
    {
        const char *arg = argv[a];
        if (arg[0] == '-')
        {
            if (arg[1] == '-' && arg[2] == '\0')
                break;

            switch (arg[1])
            {
                case 'v':
                    verbose = true;
                    break;
                case 'h':
                default:
                    printf("Usage: %s [OPTIONS] [/path/to/mir/socket] [[output OUTPUTID] ACTION ...]\n"
                           "Options:\n"
                           "    -h  Show this help information.\n"
                           "    -v  Show verbose information.\n"
                           "    --  Ignore the rest of the command line.\n"
                           "Actions:\n"
                           "    off | suspend | standby | on\n"
                           "    disable | enable\n"
                           "    rotate (normal | inverted | left | right)\n"
                           "    place +X+Y\n"
                           "    mode (WIDTHxHEIGHT | preferred) [rate HZ]\n"
                           "    rate HZ\n"
                           , argv[0]);
                    return 0;
            }
        }
        else if (arg[0] == '/')
        {
            server = arg;
        }
        else
        {
            actionv = argv + a;
            actionc = argc - a;
            break;
        }
    }

    MirConnection *conn = mir_connect_sync(server, argv[0]);
    if (!mir_connection_is_valid(conn))
    {
        fprintf(stderr, "Could not connect to a display server: %s\n", mir_connection_get_error_message(conn));
        return 1;
    }

    int ret = 0;
    MirDisplayConfig* conf = mir_connection_create_display_configuration(conn);
    if (conf == NULL)
    {
        fprintf(stderr, "Failed to get display configuration (!?)\n");
    }
    else if (actionc)
    {
        if (modify(conf, actionc, actionv))
        {
            mir_connection_preview_base_display_configuration(conn, conf, 10);
            mir_connection_confirm_base_display_configuration(conn, conf);
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        printf("Connected to server: %s\n", server ? server : "<default>");

        int num_outputs = mir_display_config_get_num_outputs(conf);

        printf("Max %d simultaneous outputs\n",
               mir_display_config_get_max_simultaneous_outputs(conf));

        for (int i = 0; i < num_outputs; ++i)
        {
            MirOutput const* out = mir_display_config_get_output(conf, i);
            MirOutputConnectionState const state =
                mir_output_get_connection_state(out);
            uint8_t const* edid = mir_output_get_edid(out);
            char name[14] = "";

            printf("Output %d: %s, %s",
                   mir_output_get_id(out),
                   mir_output_type_name(mir_output_get_type(out)),
                   state_name(state));

            /* If there's an EDID the standard requires monitor_name present */
            if (edid && edid_get_descriptor(edid, monitor_name, name))
            {
                printf(", \"%s\"", name);
            }

            if (state == mir_output_connection_state_connected)
            {
                MirOutputMode const* current_mode =
                    mir_output_get_current_mode(out);
                if (current_mode)
                {
                    printf(", %dx%d",
                           mir_output_mode_get_width(current_mode),
                           mir_output_mode_get_height(current_mode));
                }
                else
                {
                    printf(", ");
                }

                int physical_width_mm = mir_output_get_physical_width_mm(out);
                int physical_height_mm = mir_output_get_physical_height_mm(out);
                float inches = sqrtf(
                    (physical_width_mm * physical_width_mm) +
                    (physical_height_mm * physical_height_mm))
                    / 25.4f;

                printf("%+d%+d, %s, %s, %dmm x %dmm (%.1f\"), %s, %.2fx, %s, %s",
                       mir_output_get_position_x(out),
                       mir_output_get_position_y(out),
                       mir_output_is_enabled(out) ? "enabled" : "disabled",
                       power_mode_name(mir_output_get_power_mode(out)),
                       physical_width_mm,
                       physical_height_mm,
                       inches,
                       orientation_name(mir_output_get_orientation(out)),
                       mir_output_get_scale_factor(out),
                       subpixel_name(mir_output_get_subpixel_arrangement(out)),
                       form_factor_name(mir_output_get_form_factor(out)));
            }
            printf("\n");

            if (verbose && edid)
            {
                static char const indent[] = "    ";
                /* The EDID is guaranteed to be at least 128 bytes */
                int const len = 128;
                printf("%sEDID (first %d bytes):", indent, len);
                for (int i = 0; i < len; ++i)
                {
                    if ((i % 16) == 0)
                    {
                        printf("\n%s%s", indent, indent);
                    }
                    printf("%.2hhx", edid[i]);
                }
                printf("\n");
            }

            int const num_modes = mir_output_get_num_modes(out);
            int const current_mode_index =
                mir_output_get_current_mode_index(out);
            int const preferred_mode_index =
                mir_output_get_preferred_mode_index(out);
            int prev_width = -1, prev_height = -1;

            for (int m = 0; m < num_modes; ++m)
            {
                MirOutputMode const* mode = mir_output_get_mode(out, m);
                int const width = mir_output_mode_get_width(mode);
                int const height = mir_output_mode_get_height(mode);

                if (m == 0 || width != prev_width || height != prev_height)
                {
                    if (m)
                        printf("\n");

                    printf("%8dx%-8d", width, height);
                }

                printf("%6.2f%c%c",
                       mir_output_mode_get_refresh_rate(mode),
                       (m == current_mode_index) ? '*' : ' ',
                       (m == preferred_mode_index) ? '+' : ' ');

                prev_width = width;
                prev_height = height;
            }

            if (num_modes)
                printf("\n");
        }

        mir_display_config_release(conf);
    }

    mir_connection_release(conn);

    return ret;
}
