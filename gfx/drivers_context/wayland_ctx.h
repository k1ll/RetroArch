/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2015 - Daniel De Matteis
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

#ifndef __VIDEO_CONTEXT_DRIVER_WAYLAND_H
#define __VIDEO_CONTEXT_DRIVER_WAYLAND_H
   #include "libretro.h"
   enum
   {
     MAX_TOUCHES = 16
   };
   bool wayland_context_gettouchpos(unsigned id, unsigned* touch_x, unsigned* touch_y);
   void wayland_context_getmousepos(int *mouse_x, int *mouse_y, int *mouse_abs_x, int *mouse_abs_y);
   void wayland_context_getmousestate(int *mouse_l, int *mouse_r, int *mouse_m, int *mouse_wu, 
                                      int *mouse_wd, int *mouse_wl, int *mouse_wr);
   bool wl_key_state[RETROK_LAST];
#endif
