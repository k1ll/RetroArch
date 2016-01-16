/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2014-2015 - Higor Euripedes
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>

#include <boolean.h>

#include "../../driver.h"

#include "../../gfx/drivers_context/wayland_ctx.h"
#include "../../gfx/video_context_driver.h"
#include "../../general.h"
#include "../../libretro.h"
#include "../input_autodetect.h"
#include "../input_config.h"
#include "../input_joypad_driver.h"
#include "../input_keymaps.h"
#include "../input_keyboard.h"

typedef struct wayland_input
{
   bool blocked;
   const input_device_driver_t *joypad;

   int mouse_x, mouse_y;
   int mouse_abs_x, mouse_abs_y;
   int mouse_l, mouse_r, mouse_m, mouse_wu, mouse_wd, mouse_wl, mouse_wr;
} wayland_input_t;

static void *wayland_input_init(void)
{
   settings_t *settings;
   wayland_input_t *wayland;
   settings = config_get_ptr();
   wayland = (wayland_input_t*)calloc(1, sizeof(*wayland));
   RARCH_LOG("[WL]: Trying to init wayland input.\n");
   if (!wayland)
      return NULL;

   //wayland->joypad = input_joypad_init_driver(settings->input.joypad_driver, wayland);

   RARCH_LOG("[WL]: Input driver initialized.\n");
   return wayland;
}

static bool wayland_key_pressed(int key)
{

   if (key >= RETROK_LAST)
      return false;

   return wl_key_state[key];
}

static bool wayland_is_pressed(wayland_input_t *wayland, unsigned port_num, const struct retro_keybind *binds, unsigned key)
{
   if (wayland_key_pressed(binds[key].key))
      return true;

#if 0
   return input_joypad_pressed(wayland->joypad, port_num, binds, key);
#endif
   return false; 
}

static int16_t wayland_analog_pressed(wayland_input_t *wayland, const struct retro_keybind *binds,
      unsigned idx, unsigned id)
{
   int16_t pressed_minus = 0, pressed_plus = 0;
   unsigned id_minus = 0;
   unsigned id_plus  = 0;

   input_conv_analog_id_to_bind_id(idx, id, &id_minus, &id_plus);

   if (wayland_key_pressed(binds[id_minus].key))
      pressed_minus = -0x7fff;
   if (wayland_key_pressed(binds[id_plus].key))
      pressed_plus  = 0x7fff;

   return pressed_plus + pressed_minus;
}

static bool wayland_input_key_pressed(void *data, int key)
{
   settings_t *settings = config_get_ptr();
   const struct retro_keybind *binds = settings->input.binds[0];
   if (key >= 0 && key < RARCH_BIND_LIST_END)
      return wayland_is_pressed((wayland_input_t*)data, 0, binds, key);
   return false;
}

static bool wayland_input_meta_key_pressed(void *data, int key)
{
   return false;
}

static int16_t wayland_joypad_device_state(wayland_input_t *wayland, const struct retro_keybind **binds_, 
      unsigned port_num, unsigned id)
{
#if 0
   const struct retro_keybind *binds = binds_[port_num];
   if (id < RARCH_BIND_LIST_END)
      return binds[id].valid && wayland_is_pressed(wayland, port_num, binds, id);
#endif
   return 0;
}

static int16_t wayland_analog_device_state(wayland_input_t *wayland, const struct retro_keybind **binds,
      unsigned port_num, unsigned idx, unsigned id)
{
   int16_t ret = wayland_analog_pressed(wayland, binds[port_num], idx, id);
#if 0
   if (!ret)
      ret = input_joypad_analog(wayland->joypad, port_num, idx, id, binds[port_num]);
#endif
   return ret;
}

static int16_t wayland_keyboard_device_state(wayland_input_t *wayland, unsigned id)
{
   return wayland_key_pressed(id);
}

static int16_t wayland_mouse_device_state(wayland_input_t *wayland, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return wayland->mouse_l;
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return wayland->mouse_r;
      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         return wayland->mouse_wu;
      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         return wayland->mouse_wd;
      case RETRO_DEVICE_ID_MOUSE_X:
         return wayland->mouse_x;
      case RETRO_DEVICE_ID_MOUSE_Y:
         return wayland->mouse_y;
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return wayland->mouse_m;
   }

   return 0;
}

static int16_t wayland_pointer_device_state(wayland_input_t *wayland,
      unsigned idx, unsigned id, bool screen)
{
   bool valid, inside;
   int16_t res_x = 0, res_y = 0, res_screen_x = 0, res_screen_y = 0;

   /* TODO */

   return 0;
}

static int16_t wayland_lightgun_device_state(wayland_input_t *wayland, unsigned id)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_LIGHTGUN_X:
         return wayland->mouse_x;
      case RETRO_DEVICE_ID_LIGHTGUN_Y:
         return wayland->mouse_y;
      case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
         return wayland->mouse_l;
      case RETRO_DEVICE_ID_LIGHTGUN_CURSOR:
         return wayland->mouse_m;
      case RETRO_DEVICE_ID_LIGHTGUN_TURBO:
         return wayland->mouse_r;
      case RETRO_DEVICE_ID_LIGHTGUN_START:
         return wayland->mouse_m && wayland->mouse_r; 
      case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
         return wayland->mouse_m && wayland->mouse_l; 
   }

   return 0;
}

static int16_t wayland_input_state(void *data_, const struct retro_keybind **binds,
      unsigned port, unsigned device, unsigned idx, unsigned id)
{
   wayland_input_t *data = (wayland_input_t*)data_;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         return wayland_joypad_device_state(data, binds, port, id);
      case RETRO_DEVICE_ANALOG:
         return wayland_analog_device_state(data, binds, port, idx, id);
      case RETRO_DEVICE_MOUSE:
         return wayland_mouse_device_state(data, id);
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         return wayland_pointer_device_state(data, idx, id, device == RARCH_DEVICE_POINTER_SCREEN);
      case RETRO_DEVICE_KEYBOARD:
         return wayland_keyboard_device_state(data, id);
      case RETRO_DEVICE_LIGHTGUN:
         return wayland_lightgun_device_state(data, id);
   }

   return 0;
}

static void wayland_input_free(void *data)
{
   wayland_input_t *wayland = (wayland_input_t*)data;

   if (!data)
      return;

   if (wayland->joypad)
      wayland->joypad->destroy();

   free(data);
}

//Not possible yet. Let's see what the future holds for us.
static void wayland_grab_mouse(void *data, bool state)
{
   /* TODO */
}

static bool wayland_set_rumble(void *data, unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   wayland_input_t *wayland = (wayland_input_t*)data;
   if (!wayland)
      return false;
   return input_joypad_set_rumble(wayland->joypad, port, effect, strength);
}

static const input_device_driver_t *wayland_get_joypad_driver(void *data)
{
   wayland_input_t *wayland = (wayland_input_t*)data;
   if (!wayland)
      return NULL;
   return wayland->joypad;
}

static void wayland_input_poll(void *data)
{
   wayland_input_t *wayland = (wayland_input_t*)data;
}

static uint64_t wayland_get_capabilities(void *data)
{
   uint64_t caps = 0;

   caps |= (1 << RETRO_DEVICE_JOYPAD);
   caps |= (1 << RETRO_DEVICE_MOUSE);
   caps |= (1 << RETRO_DEVICE_KEYBOARD);
   caps |= (1 << RETRO_DEVICE_LIGHTGUN);
   caps |= (1 << RETRO_DEVICE_POINTER);
   caps |= (1 << RETRO_DEVICE_ANALOG);

   return caps;
}

static bool wayland_keyboard_mapping_is_blocked(void *data)
{
   wayland_input_t *wayland = (wayland_input_t*)data;
   if (!wayland)
      return false;
   return wayland->blocked;
}

static void wayland_keyboard_mapping_set_block(void *data, bool value)
{
   wayland_input_t *wayland = (wayland_input_t*)data;
   if (!wayland)
      return;
   wayland->blocked = value;
}

input_driver_t input_wayland = {
   wayland_input_init,
   wayland_input_poll,
   wayland_input_state,
   wayland_input_key_pressed,
   wayland_input_meta_key_pressed,
   wayland_input_free,
   NULL,
   NULL,
   wayland_get_capabilities,
   "wayland",
   wayland_grab_mouse,
   NULL,
   wayland_set_rumble,
   wayland_get_joypad_driver,
   wayland_keyboard_mapping_is_blocked,
   wayland_keyboard_mapping_set_block,
};
