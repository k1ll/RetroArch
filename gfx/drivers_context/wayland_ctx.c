/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2016 - Daniel De Matteis
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

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_VULKAN
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "../common/vulkan_common.h"
#endif

#include <sys/poll.h>
#include <unistd.h>
#include <signal.h>

#include <sys/mman.h>

#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-client.h>
#ifdef HAVE_EGL
#include <wayland-egl.h>
#endif
#include <wayland-util.h>

#include <string/stdstring.h>

#include "../../driver.h"
#include "../../general.h"
#include "../../runloop.h"
#ifdef HAVE_EGL
#include "../common/egl_common.h"
#endif
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include "../common/gl_common.h"
#endif

static volatile sig_atomic_t g_quit = 0;

#ifdef HAVE_VULKAN
static VkInstance cached_instance;
static VkDevice cached_device;

#endif

#ifdef HAVE_VULKAN
typedef struct gfx_ctx_vulkan_data
{
   struct vulkan_context context;

   PFN_vkGetPhysicalDeviceSurfaceSupportKHR fpGetPhysicalDeviceSurfaceSupportKHR;
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fpGetPhysicalDeviceSurfaceCapabilitiesKHR;
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fpGetPhysicalDeviceSurfaceFormatsKHR;
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fpGetPhysicalDeviceSurfacePresentModesKHR;
   PFN_vkCreateSwapchainKHR fpCreateSwapchainKHR;
   PFN_vkDestroySwapchainKHR fpDestroySwapchainKHR;
   PFN_vkGetSwapchainImagesKHR fpGetSwapchainImagesKHR;
   PFN_vkAcquireNextImageKHR fpAcquireNextImageKHR;
   PFN_vkQueuePresentKHR fpQueuePresentKHR;
   PFN_vkCreateWaylandSurfaceKHR fpCreateWaylandSurfaceKHR;
   PFN_vkDestroySurfaceKHR fpDestroySurfaceKHR;

   VkSurfaceKHR vk_surface;
   VkSwapchainKHR swapchain;
   bool need_new_swapchain;
} gfx_ctx_vulkan_data_t;
#endif

#include "wayland_ctx.h"

typedef struct gfx_ctx_wayland_data
{

#ifdef HAVE_EGL
   egl_ctx_data_t egl;
   struct wl_egl_window *win;
#endif
   bool resize;
   int fd;
   unsigned width;
   unsigned height;
   struct wl_display *dpy;
   struct wl_registry *registry;
   struct wl_compositor *compositor;
   struct wl_surface *surface;
   struct wl_shell_surface *shell_surf;
   struct wl_shell *shell;
   struct wl_keyboard *wl_keyboard;
   struct wl_pointer  *wl_pointer;
   unsigned swap_interval;

   unsigned buffer_scale;

#ifdef HAVE_VULKAN
   gfx_ctx_vulkan_data_t vk;
#endif
   struct wl_list input_list;
} gfx_ctx_wayland_data_t;

#ifdef HAVE_VULKAN
/* Forward declaration */
static bool vulkan_create_swapchain(gfx_ctx_vulkan_data_t *vk,
      unsigned width, unsigned height, unsigned swap_interval);
#endif

static enum gfx_ctx_api wl_api;

typedef struct wayland_input
{
   struct wl_seat *seat;
   struct wl_keyboard *keyboard;
   struct wl_pointer  *pointer;
   struct wl_touch *touch;
   struct xkb_context *xkb_context;
   struct {
           struct xkb_keymap *keymap;
           struct xkb_state *state;
           /*xkb_mod_mask_t control_mask;
           xkb_mod_mask_t alt_mask;
           xkb_mod_mask_t shift_mask;*/
   } xkb;

   struct wl_list link;
} wayland_input_t;

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

static void sighandler(int sig)
{
   (void)sig;
   g_quit = 1;
}

static void install_sighandlers(void)
{
   struct sigaction sa;

   sa.sa_sigaction = NULL;
   sa.sa_handler   = sighandler;
   sa.sa_flags     = SA_RESTART;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);
}

typedef struct touch_pos
{
  bool active;
  int32_t id;
  unsigned x;
  unsigned y;
} touch_pos_t;

int num_active_touches;
touch_pos_t active_touch_positions[MAX_TOUCHES];

bool wayland_context_gettouchpos(unsigned id, unsigned* touch_x, unsigned* touch_y)
{
  if (id >= MAX_TOUCHES)
    return false;

  *touch_x = active_touch_positions[id].x;
  *touch_y = active_touch_positions[id].y;
  return active_touch_positions[id].active;
}

typedef struct mouse_state
{
   int x;
   int y;
   int abs_x;
   int abs_y;
   int l;
   int r;
   int m;
   int wu;
   int wd;
   int wl;
   int wr;
} mouse_state_t;

mouse_state_t mouse;

//TODO: Sadly no support for relative mouse motions yet - mouse_x and mouse_y stay 0
void wayland_context_getmousepos(int *mouse_x, int *mouse_y, int *mouse_abs_x, int *mouse_abs_y)
{
   *mouse_x = mouse.x;
   *mouse_y = mouse.y;
   *mouse_abs_x = mouse.abs_x;
   *mouse_abs_y = mouse.abs_y;
}

void wayland_context_getmousestate(int *mouse_l, int *mouse_r, int *mouse_m, int *mouse_wu, 
                                   int *mouse_wd, int *mouse_wl, int *mouse_wr)
{
   *mouse_l = mouse.l;
   *mouse_r = mouse.r;
   *mouse_m = mouse.m;
   *mouse_wu = mouse.wu;
   *mouse_wd = mouse.wd;
   *mouse_wl = mouse.wl;
   *mouse_wr = mouse.wr;
}

/* Shell surface callbacks. */
static void shell_surface_handle_ping(void *data,
      struct wl_shell_surface *shell_surface,
      uint32_t serial)
{
   (void)data;
   wl_shell_surface_pong(shell_surface, serial);
}

static void shell_surface_handle_configure(void *data,
      struct wl_shell_surface *shell_surface,
      uint32_t edges, int32_t width, int32_t height)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   (void)shell_surface;
   (void)edges;

   wl->width  = wl->buffer_scale * width;
   wl->height = wl->buffer_scale * height;

   RARCH_LOG("[Wayland/EGL]: Surface configure: %u x %u.\n",
         wl->width, wl->height);
}

static void shell_surface_handle_popup_done(void *data,
      struct wl_shell_surface *shell_surface)
{
   (void)data;
   (void)shell_surface;
}

static const struct wl_shell_surface_listener shell_surface_listener = {
   shell_surface_handle_ping,
   shell_surface_handle_configure,
   shell_surface_handle_popup_done,
};

//TODO: Just move the function definitions here?
static const struct wl_seat_listener seat_listener;

wayland_input_t *input_init()
{
   wayland_input_t *input = (wayland_input_t*)calloc(1, sizeof(wayland_input_t));
   input->xkb_context = xkb_context_new(0);
   if (input->xkb_context == NULL) {
      RARCH_ERR("[Wayland] Failed to create XKB context\n");
   }

   return input;
}

/* Registry callbacks. */
static void registry_handle_global(void *data, struct wl_registry *reg,
      uint32_t id, const char *interface, uint32_t version)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   (void)version;

   if (string_is_equal(interface, "wl_compositor"))
   {
      unsigned num = 1;
      switch (wl_api)
      {
         case GFX_CTX_OPENGL_API:
         case GFX_CTX_OPENGL_ES_API:
         case GFX_CTX_OPENVG_API:
            break;
         case GFX_CTX_VULKAN_API:
            num = 3;
            break;
         case GFX_CTX_NONE:
         default:
            break;
      }

      wl->compositor = (struct wl_compositor*)wl_registry_bind(reg,
            id, &wl_compositor_interface, num);
   }
   else if (string_is_equal(interface, "wl_shell"))
      wl->shell = (struct wl_shell*)wl_registry_bind(reg, id, &wl_shell_interface, 1);
   else if (string_is_equal(interface, "wl_seat"))
   {
      wayland_input_t *input = input_init();
      wl_list_insert(&wl->input_list, &input->link);
      input->seat = (struct wl_seat*)wl_registry_bind(reg, id, &wl_seat_interface, 1);
      wl_seat_add_listener(input->seat, &seat_listener, input);
      wl_seat_set_user_data(input->seat, input);
   }
}

static void registry_handle_global_remove(void *data,
      struct wl_registry *registry, uint32_t id)
{
   (void)data;
   (void)registry;
   (void)id;
}

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove,
};



static void gfx_ctx_wl_get_video_size(void *data,
      unsigned *width, unsigned *height);

static void input_destroy(wayland_input_t *input)
{
   if (!input)
      return;

   if (input->touch)
      wl_touch_destroy(input->touch);
   if (input->pointer)
      wl_pointer_destroy(input->pointer);
   if (input->keyboard)
      wl_keyboard_destroy(input->keyboard);

   xkb_state_unref(input->xkb.state);
   xkb_keymap_unref(input->xkb.keymap);

   xkb_context_unref(input->xkb_context);

   wl_list_remove(&input->link);
   wl_seat_destroy(input->seat);
   free(input);
}

static void destroy_inputs(gfx_ctx_wayland_data_t* wl)
{
   wayland_input_t *tmp;
   wayland_input_t *input;

   if(!wl)
      return;

   wl_list_for_each_safe(input, tmp, &wl->input_list, link)
   {
      input_destroy(input);
   }
}

static void gfx_ctx_wl_destroy_resources(gfx_ctx_wayland_data_t *wl)
{
   unsigned i;

   if (!wl)
      return;

   (void)i;

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         egl_destroy(wl);

         if (wl->win)
            wl_egl_window_destroy(wl->win);
#endif
         break;
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         if (wl->vk.context.queue)
            vkQueueWaitIdle(wl->vk.context.queue);
         if (wl->vk.swapchain)
            wl->vk.fpDestroySwapchainKHR(wl->vk.context.device,
                  wl->vk.swapchain, NULL);

         if (wl->surface)
            wl->vk.fpDestroySurfaceKHR(wl->vk.context.instance,
                  wl->vk.vk_surface, NULL);

         for (i = 0; i < VULKAN_MAX_SWAPCHAIN_IMAGES; i++)
         {
            if (wl->vk.context.swapchain_semaphores[i] != VK_NULL_HANDLE)
               vkDestroySemaphore(wl->vk.context.device,
                     wl->vk.context.swapchain_semaphores[i], NULL);
            if (wl->vk.context.swapchain_fences[i] != VK_NULL_HANDLE)
               vkDestroyFence(wl->vk.context.device,
                     wl->vk.context.swapchain_fences[i], NULL);
         }

         if (video_driver_ctl(RARCH_DISPLAY_CTL_IS_VIDEO_CACHE_CONTEXT, NULL))
         {
            cached_device   = wl->vk.context.device;
            cached_instance = wl->vk.context.instance;
         }
         else
         {
            if (wl->vk.context.device)
               vkDestroyDevice(wl->vk.context.device, NULL);
            if (wl->vk.context.instance)
               vkDestroyInstance(wl->vk.context.instance, NULL);
         }

         if (wl->fd >= 0)
            close(wl->fd);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   if (wl->shell)
      wl_shell_destroy(wl->shell);
   if (wl->compositor)
      wl_compositor_destroy(wl->compositor);
   if (wl->registry)
      wl_registry_destroy(wl->registry);
   if (wl->shell_surf)
      wl_shell_surface_destroy(wl->shell_surf);
   if (wl->surface)
      wl_surface_destroy(wl->surface);

   destroy_inputs(wl);

   if (wl->dpy)
   {
      wl_display_flush(wl->dpy);
      wl_display_disconnect(wl->dpy);
   }

   wl->win        = NULL;
   wl->shell      = NULL;
   wl->compositor = NULL;
   wl->registry   = NULL;
   wl->dpy        = NULL;
   wl->shell_surf = NULL;
   wl->surface    = NULL;

   wl->width  = 0;
   wl->height = 0;
}

static void flush_wayland_fd(gfx_ctx_wayland_data_t *wl)
{
   struct pollfd fd = {0};

   wl_display_dispatch_pending(wl->dpy);
   wl_display_flush(wl->dpy);

   fd.fd = wl->fd;
   fd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;

   if (poll(&fd, 1, 0) > 0)
   {
      if (fd.revents & (POLLERR | POLLHUP))
      {
         close(wl->fd);
         g_quit = true;
      }

      if (fd.revents & POLLIN)
         wl_display_dispatch(wl->dpy);
      if (fd.revents & POLLOUT)
         wl_display_flush(wl->dpy);
   }
}

static void gfx_ctx_wl_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height,
      unsigned frame_count)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;
   unsigned new_width, new_height;

   (void)frame_count;

   flush_wayland_fd(wl);

   new_width = *width;
   new_height = *height;

   gfx_ctx_wl_get_video_size(data, &new_width, &new_height);

   switch (wl_api)
   {
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         /* Swapchains are recreated in set_resize as a 
          * central place, so use that to trigger swapchain reinit. */
         *resize = wl->vk.need_new_swapchain;
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   if (new_width != *width || new_height != *height)
   {
      *resize = true;
      *width  = new_width;
      *height = new_height;
   }

   *quit = g_quit;
}

static bool gfx_ctx_wl_set_resize(void *data, unsigned width, unsigned height)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         wl_egl_window_resize(wl->win, width, height, 0, 0);
#endif
         break;
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         wl->width  = width;
         wl->height = height;

         if (vulkan_create_swapchain(&wl->vk, width, height, wl->swap_interval))
            wl->vk.context.invalid_swapchain = true;
         else
         {
            RARCH_ERR("[Wayland/Vulkan]: Failed to update swapchain.\n");
            return false;
         }

         wl->vk.need_new_swapchain = false;
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   return true;
}

static void gfx_ctx_wl_update_window_title(void *data)
{
   char buf[128]              = {0};
   char buf_fps[128]          = {0};
   settings_t *settings       = config_get_ptr();
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   if (video_monitor_get_fps(buf, sizeof(buf),  
            buf_fps, sizeof(buf_fps)))
      wl_shell_surface_set_title(wl->shell_surf, buf);

   if (settings->fps_show)
      runloop_msg_queue_push(buf_fps, 1, 1, false);
}

static void gfx_ctx_wl_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   *width  = wl->width;
   *height = wl->height;
}

#define DEFAULT_WINDOWED_WIDTH 640
#define DEFAULT_WINDOWED_HEIGHT 480

#ifdef HAVE_EGL
#define WL_EGL_ATTRIBS_BASE \
   EGL_SURFACE_TYPE,    EGL_WINDOW_BIT, \
   EGL_RED_SIZE,        1, \
   EGL_GREEN_SIZE,      1, \
   EGL_BLUE_SIZE,       1, \
   EGL_ALPHA_SIZE,      0, \
   EGL_DEPTH_SIZE,      0
#endif

#ifdef HAVE_VULKAN

#define VK_GET_INSTANCE_PROC_ADDR(vk, inst, entrypoint) do {                               \
   vk->fp##entrypoint = (PFN_vk##entrypoint) vkGetInstanceProcAddr(inst, "vk"#entrypoint); \
   if (vk->fp##entrypoint == NULL) {                                                       \
      RARCH_ERR("vkGetInstanceProcAddr failed to find vk%s\n", #entrypoint);               \
      return false;                                                                        \
   }                                                                                       \
} while(0)

#define VK_GET_DEVICE_PROC_ADDR(vk, dev, entrypoint) do {                                \
   vk->fp##entrypoint = (PFN_vk##entrypoint) vkGetDeviceProcAddr(dev, "vk" #entrypoint); \
   if (vk->fp##entrypoint == NULL) {                                                     \
      RARCH_ERR("vkGetDeviceProcAddr failed to find vk%s\n", #entrypoint);               \
      return false;                                                                      \
   }                                                                                     \
} while(0)

static void vulkan_destroy_context(gfx_ctx_vulkan_data_t *vk)
{
   if (vk->context.queue_lock)
      slock_free(vk->context.queue_lock);
}

static bool vulkan_init_context(gfx_ctx_vulkan_data_t *vk)
{
   unsigned i;
   uint32_t queue_count;
   VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkQueueFamilyProperties queue_properties[32];
   uint32_t gpu_count = 1;
   bool found_queue = false;
   static const float one = 1.0f;

   static const char *instance_extensions[] = {
      "VK_KHR_surface",
      "VK_KHR_wayland_surface",
   };

   static const char *device_extensions[] = {
      "VK_KHR_swapchain",
   };
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkPhysicalDeviceFeatures features = { false };

   app.pApplicationName = "RetroArch";
   app.applicationVersion = 0;
   app.pEngineName = "RetroArch";
   app.engineVersion = 0;
   app.apiVersion = VK_API_VERSION;

   info.pApplicationInfo = &app;
   info.enabledExtensionCount   = ARRAY_SIZE(instance_extensions);
   info.ppEnabledExtensionNames = instance_extensions;

   if (cached_instance)
   {
      vk->context.instance = cached_instance;
      cached_instance         = NULL;
   }
   else if (vkCreateInstance(&info, NULL, &vk->context.instance) != VK_SUCCESS)
      return false;

   if (vkEnumeratePhysicalDevices(vk->context.instance,
            &gpu_count, &vk->context.gpu) != VK_SUCCESS)
      return false;

   if (gpu_count != 1)
   {
      RARCH_ERR("[Wayland/Vulkan]: Failed to enumerate Vulkan physical device.\n");
      return false;
   }

   vkGetPhysicalDeviceProperties(vk->context.gpu, &vk->context.gpu_properties);
   vkGetPhysicalDeviceMemoryProperties(vk->context.gpu, &vk->context.memory_properties);
   vkGetPhysicalDeviceQueueFamilyProperties(vk->context.gpu, &queue_count, NULL);
   if (queue_count < 1 || queue_count > 32)
      return false;
   vkGetPhysicalDeviceQueueFamilyProperties(vk->context.gpu, &queue_count, queue_properties);

   for (i = 0; i < queue_count; i++)
   {
      if (queue_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      {
         vk->context.graphics_queue_index = i;
         RARCH_LOG("[Wayland/Vulkan]: Device supports %u sub-queues.\n",
               queue_properties[i].queueCount);
         found_queue = true;
         break;
      }
   }

   if (!found_queue)
   {
      RARCH_ERR("[Wayland/Vulkan]: Did not find suitable graphics queue.\n");
      return false;
   }

   queue_info.queueFamilyIndex         = vk->context.graphics_queue_index;
   queue_info.queueCount               = 1;
   queue_info.pQueuePriorities         = &one;

   device_info.queueCreateInfoCount    = 1;
   device_info.pQueueCreateInfos       = &queue_info;
   device_info.enabledExtensionCount   = ARRAY_SIZE(device_extensions);
   device_info.ppEnabledExtensionNames = device_extensions;
   device_info.pEnabledFeatures        = &features;

   if (cached_device)
   {
      vk->context.device = cached_device;
      cached_device = NULL;
      video_driver_ctl(RARCH_DISPLAY_CTL_SET_VIDEO_CACHE_CONTEXT_ACK, NULL);
      RARCH_LOG("[Vulkan]: Using cached Vulkan context.\n");
   }
   else if (vkCreateDevice(vk->context.gpu, &device_info,
            NULL, &vk->context.device) != VK_SUCCESS)
      return false;

   vkGetDeviceQueue(vk->context.device,
         vk->context.graphics_queue_index, 0, &vk->context.queue);

   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, GetPhysicalDeviceSurfaceSupportKHR);
   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, GetPhysicalDeviceSurfaceFormatsKHR);
   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, GetPhysicalDeviceSurfacePresentModesKHR);
   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, CreateWaylandSurfaceKHR);
   VK_GET_INSTANCE_PROC_ADDR(vk,
         vk->context.instance, DestroySurfaceKHR);
   VK_GET_DEVICE_PROC_ADDR(vk,
         vk->context.device, CreateSwapchainKHR);
   VK_GET_DEVICE_PROC_ADDR(vk,
         vk->context.device, DestroySwapchainKHR);
   VK_GET_DEVICE_PROC_ADDR(vk,
         vk->context.device, GetSwapchainImagesKHR);
   VK_GET_DEVICE_PROC_ADDR(vk,
         vk->context.device, AcquireNextImageKHR);
   VK_GET_DEVICE_PROC_ADDR(vk,
         vk->context.device, QueuePresentKHR);

   vk->context.queue_lock = slock_new();
   if (!vk->context.queue_lock)
      return false;

   return true;
}
#endif

static void *gfx_ctx_wl_init(void *video_driver)
{

#ifdef HAVE_OPENGL
   static const EGLint egl_attribs_gl[] = {
      WL_EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE,
   };
#endif

#ifdef HAVE_OPENGLES
#ifdef HAVE_OPENGLES2
   static const EGLint egl_attribs_gles[] = {
      WL_EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE,
   };
#endif

#ifdef HAVE_OPENGLES3
#ifdef EGL_KHR_create_context
   static const EGLint egl_attribs_gles3[] = {
      WL_EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
      EGL_NONE,
   };
#endif
#endif

#endif

#ifdef HAVE_EGL
   static const EGLint egl_attribs_vg[] = {
      WL_EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENVG_BIT,
      EGL_NONE,
   };

   EGLint major = 0, minor = 0;
   EGLint n;
   const EGLint *attrib_ptr = NULL;
#endif
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)
      calloc(1, sizeof(gfx_ctx_wayland_data_t));

   if (!wl)
      return NULL;

   (void)video_driver;

   switch (wl->egl.api)
   {
      case GFX_CTX_OPENGL_API:
#ifdef HAVE_OPENGL
         attrib_ptr = egl_attribs_gl;
#endif
         break;
      case GFX_CTX_OPENGL_ES_API:
#ifdef HAVE_OPENGLES
#ifdef HAVE_OPENGLES3
#ifdef EGL_KHR_create_context
         if (g_egl_major >= 3)
            attrib_ptr = egl_attribs_gles3;
         else
#endif
#endif
            attrib_ptr = egl_attribs_gles;
#endif
         break;
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_VG
         attrib_ptr = egl_attribs_vg;
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   g_quit = 0;

   wl->dpy = wl_display_connect(NULL);
   if (!wl->dpy)
   {
      RARCH_ERR("Failed to connect to Wayland server.\n");
      goto error;
   }

   install_sighandlers();

   wl_list_init(&wl->input_list);

   wl->registry = wl_display_get_registry(wl->dpy);
   wl_registry_add_listener(wl->registry, &registry_listener, wl);

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         wl_display_dispatch(wl->dpy);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   wl_display_roundtrip(wl->dpy);

   if (!wl->compositor)
   {
      RARCH_ERR("Failed to create compositor.\n");
      goto error;
   }

   if (!wl->shell)
   {
      RARCH_ERR("Failed to create shell.\n");
      goto error;
   }

   wl->fd = wl_display_get_fd(wl->dpy);

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         if (!egl_init_context(wl, (EGLNativeDisplayType)wl->dpy,
                  &major, &minor, &n, attrib_ptr))
         {
            egl_report_error();
            goto error;
         }

         if (n == 0 || !egl_has_config(wl))
            goto error;
#endif
         break;
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         if (!vulkan_init_context(&wl->vk))
            goto error;
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   mouse.x = 0;
   mouse.y = 0;
   mouse.abs_x = 0;
   mouse.abs_y = 0;
   mouse.l = 0;
   mouse.r = 0;
   mouse.m = 0;
   mouse.wu = 0;
   mouse.wd = 0;
   mouse.wl = 0;
   mouse.wr = 0;

   num_active_touches = 0;

   for (int i=0;i<MAX_TOUCHES;i++)
   {
       active_touch_positions[i].active = false;
       active_touch_positions[i].id = -1;
       active_touch_positions[i].x = (unsigned) 0;
       active_touch_positions[i].y = (unsigned) 0;
   }

   return wl;

error:
   gfx_ctx_wl_destroy_resources(wl);

   if (wl)
      free(wl);

   return NULL;
}

#ifdef HAVE_EGL
static EGLint *egl_fill_attribs(gfx_ctx_wayland_data_t *wl, EGLint *attr)
{
   switch (wl->egl.api)
   {
#ifdef EGL_KHR_create_context
      case GFX_CTX_OPENGL_API:
      {
#ifdef HAVE_OPENGL
         unsigned version = wl->egl.major * 1000 + wl->egl.minor;
         bool core = version >= 3001;
#ifdef GL_DEBUG
         bool debug = true;
#else
         const struct retro_hw_render_callback *hw_render =
            (const struct retro_hw_render_callback*)video_driver_callback();
         bool debug = hw_render->debug_context;
#endif

         if (core)
         {
            *attr++ = EGL_CONTEXT_MAJOR_VERSION_KHR;
            *attr++ = wl->egl.major;
            *attr++ = EGL_CONTEXT_MINOR_VERSION_KHR;
            *attr++ = wl->egl.minor;
            /* Technically, we don't have core/compat until 3.2.
             * Version 3.1 is either compat or not depending on GL_ARB_compatibility. */
            if (version >= 3002)
            {
               *attr++ = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
               *attr++ = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
            }
         }

         if (debug)
         {
            *attr++ = EGL_CONTEXT_FLAGS_KHR;
            *attr++ = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
         }
#endif

         break;
      }
#endif

      case GFX_CTX_OPENGL_ES_API:
#ifdef HAVE_OPENGLES
         *attr++ = EGL_CONTEXT_CLIENT_VERSION; /* Same as EGL_CONTEXT_MAJOR_VERSION */
         *attr++ = wl->egl.major ? (EGLint)wl->egl.major : 2;
#ifdef EGL_KHR_create_context
         if (wl->egl.minor > 0)
         {
            *attr++ = EGL_CONTEXT_MINOR_VERSION_KHR;
            *attr++ = wl->egl.minor;
         }
#endif
#endif
         break;

      case GFX_CTX_NONE:
      default:
         break;
   }

   *attr = EGL_NONE;
   return attr;
}
#endif

static void gfx_ctx_wl_destroy(void *data)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   if (!wl)
      return;

   gfx_ctx_wl_destroy_resources(wl);

   switch (wl_api)
   {
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         vulkan_destroy_context(&wl->vk);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   free(wl);
}

static void gfx_ctx_wl_set_swap_interval(void *data, unsigned swap_interval)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         egl_set_swap_interval(data, swap_interval);
#endif
         break;
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         if (wl->swap_interval != swap_interval)
         {
            wl->swap_interval = swap_interval;
            if (wl->vk.swapchain)
               wl->vk.need_new_swapchain = true;
         }
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }
}

static bool gfx_ctx_wl_set_video_mode(void *data,
      unsigned width, unsigned height,
      bool fullscreen)
{
#ifdef HAVE_EGL
   EGLint egl_attribs[16];
   EGLint *attr              = egl_fill_attribs(
         (gfx_ctx_wayland_data_t*)data, egl_attribs);
#endif
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   wl->width        = width  ? width  : DEFAULT_WINDOWED_WIDTH;
   wl->height       = height ? height : DEFAULT_WINDOWED_HEIGHT;

   /* TODO: Use wl_output::scale to obtain correct value. */
   wl->buffer_scale = 1;

   wl->surface    = wl_compositor_create_surface(wl->compositor);

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         wl->win        = wl_egl_window_create(wl->surface, wl->width, wl->height);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }
   wl->shell_surf = wl_shell_get_shell_surface(wl->shell, wl->surface);

   wl_shell_surface_add_listener(wl->shell_surf, &shell_surface_listener, wl);
   wl_shell_surface_set_toplevel(wl->shell_surf);
   wl_shell_surface_set_class(wl->shell_surf, "RetroArch");
   wl_shell_surface_set_title(wl->shell_surf, "RetroArch");

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL

         if (!egl_create_context(wl, (attr != egl_attribs) ? egl_attribs : NULL))
         {
            egl_report_error();
            goto error;
         }

         if (!egl_create_surface(wl, (EGLNativeWindowType)wl->win))
            goto error;
         egl_set_swap_interval(wl, wl->egl.interval);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   if (fullscreen)
      wl_shell_surface_set_fullscreen(wl->shell_surf,
            WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, NULL);

   flush_wayland_fd(wl);

   switch (wl_api)
   {
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         {
            VkWaylandSurfaceCreateInfoKHR wl_info = 
            { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
            wl_display_roundtrip(wl->dpy);

            wl_info.display = wl->dpy;
            wl_info.surface = wl->surface;

            wl->vk.fpCreateWaylandSurfaceKHR(wl->vk.context.instance,
                  &wl_info, NULL, &wl->vk.vk_surface);

            if (!vulkan_create_swapchain(
                     &wl->vk, wl->width, wl->height, wl->swap_interval))
               goto error;
         }
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }

   return true;

error:
   gfx_ctx_wl_destroy(data);
   return false;
}

static void gfx_ctx_wl_input_driver(void *data,
      const input_driver_t **input, void **input_data)
{
   (void)data;
#if 0
   void *wl    = input_wayland.init();
   *input      = wl ? &input_wayland : NULL;
   *input_data = wl;
#endif
   *input = NULL;
   *input_data = NULL;
}

static bool gfx_ctx_wl_has_focus(void *data)
{
   (void)data;
   return true;
}

static bool gfx_ctx_wl_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;
   return true;
}

static bool gfx_ctx_wl_has_windowed(void *data)
{
   (void)data;
   return true;
}

static bool gfx_ctx_wl_bind_api(void *video_driver,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   g_egl_major = major;
   g_egl_minor = minor;
   g_egl_api   = api;

   switch (api)
   {
      case GFX_CTX_OPENGL_API:
#ifdef HAVE_OPENGL
#ifndef EGL_KHR_create_context
         if ((major * 1000 + minor) >= 3001)
            return false;
#endif
         wl_api = api;
         return eglBindAPI(EGL_OPENGL_API);
#else
         break;
#endif
      case GFX_CTX_OPENGL_ES_API:
#ifdef HAVE_OPENGLES
#ifndef EGL_KHR_create_context
         if (major >= 3)
            return false;
#endif
         wl_api = api;
         return eglBindAPI(EGL_OPENGL_ES_API);
#else
         break;
#endif
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_VG
         wl_api = api;
         return eglBindAPI(EGL_OPENVG_API);
#else
         break;
#endif
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         wl_api = api;
         return true;
#else
         break;
#endif
      case GFX_CTX_NONE:
      default:
         break;
   }

   return false;
}

static void keyboard_handle_keymap(void* data,
struct wl_keyboard* keyboard,
uint32_t format,
int fd,
uint32_t size)
{
   wayland_input_t *input = (wayland_input_t*)data;

   if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP)
      RARCH_ERR("[Wayland]: No Keyboard Keymap Format\n");
   if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
   {
      RARCH_LOG("[Wayland]: XKB v1 Keyboard Keymap Format\n");
      struct xkb_keymap *keymap;
      struct xkb_state *state;
      char *map_str;

      if (!input) {
        close(fd);
        return;
      }

      map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
      if (map_str == MAP_FAILED) {
        close(fd);
        return;
      }

      keymap = xkb_keymap_new_from_string(input->xkb_context,
                                          map_str,
                                          XKB_KEYMAP_FORMAT_TEXT_V1,
                                          0);
      munmap(map_str, size);
      close(fd);

      if (!keymap) {
        RARCH_ERR("[Wayland]: Failed to compile keymap\n");
        return;
      }

      state = xkb_state_new(keymap);
      if (!state) {
        RARCH_ERR("[Wayland]: Failed to create XKB state\n");
        xkb_keymap_unref(keymap);
        return;
      }

      xkb_keymap_unref(input->xkb.keymap);
      xkb_state_unref(input->xkb.state);
      input->xkb.keymap = keymap;
      input->xkb.state = state;
   }
}

static void keyboard_handle_enter(void* data,
struct wl_keyboard* keyboard,
uint32_t serial,
struct wl_surface* surface,
struct wl_array* keys)
{
   /* TODO */
}

static void keyboard_handle_leave(void* data,
struct wl_keyboard* keyboard,
uint32_t serial,
struct wl_surface* surface)
{
   /* TODO */
}

static void keyboard_handle_key(void* data,
struct wl_keyboard* keyboard,
uint32_t serial,
uint32_t time,
uint32_t key,
uint32_t state)
{
   // Only handling xkb v1 compatible format atm
   uint32_t code, num_syms;
   const xkb_keysym_t *syms;
   xkb_keysym_t sym;
   bool pressed;
   wayland_input_t *input = (wayland_input_t*)data;

   code = key + 8;
   if (!input->xkb.state)
      return;

   num_syms = xkb_state_key_get_syms(input->xkb.state, code, &syms);

   sym = XKB_KEY_NoSymbol;
   if (num_syms == 1)
      sym = syms[0];

   //TODO: Textinput
   //char text[16];
   //xkb_keysym_to_utf8(sym, text, sizeof(text)) <= 0

   if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
   {
      pressed = true;
      //RARCH_LOG("[WL]: Key Press %lu\n", key);
      //RARCH_LOG("[WL]: Key Press (HEX xkb) %x\n", key+8L);
   }
   if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
   {
     pressed = false;
     //RARCH_LOG("[WL]: Key Release %lu\n", key);
   }

   //TODO: Find missing Key equivalents
   /*if (sym == XKB_KEY_)
     wl_key_state[RETROK_FIRST] = pressed;
   else*/ if (sym == XKB_KEY_BackSpace)
     wl_key_state[RETROK_BACKSPACE] = pressed;
   else if (sym == XKB_KEY_Tab)
     wl_key_state[RETROK_TAB] = pressed;
   else if (sym == XKB_KEY_Clear)
     wl_key_state[RETROK_CLEAR] = pressed;
   else if (sym == XKB_KEY_Return)
     wl_key_state[RETROK_RETURN] = pressed;
   else if (sym == XKB_KEY_Pause)
     wl_key_state[RETROK_PAUSE] = pressed;
   else if (sym == XKB_KEY_Escape)
     wl_key_state[RETROK_ESCAPE] = pressed;
   else if (sym == XKB_KEY_space)
     wl_key_state[RETROK_SPACE] = pressed;
   else if (sym == XKB_KEY_exclam)
     wl_key_state[RETROK_EXCLAIM] = pressed;
   else if (sym == XKB_KEY_quotedbl)
     wl_key_state[RETROK_QUOTEDBL] = pressed;
   else if (sym == XKB_KEY_numbersign)
     wl_key_state[RETROK_HASH] = pressed;
   else if (sym == XKB_KEY_dollar)
     wl_key_state[RETROK_DOLLAR] = pressed;
   else if (sym == XKB_KEY_ampersand)
     wl_key_state[RETROK_AMPERSAND] = pressed;
   else if (sym == XKB_KEY_leftsinglequotemark)
     wl_key_state[RETROK_QUOTE] = pressed;
   else if (sym == XKB_KEY_parenleft)
     wl_key_state[RETROK_LEFTPAREN] = pressed;
   else if (sym == XKB_KEY_parenright)
     wl_key_state[RETROK_RIGHTPAREN] = pressed;
   else if (sym == XKB_KEY_asterisk)
     wl_key_state[RETROK_ASTERISK] = pressed;
   else if (sym == XKB_KEY_plus)
     wl_key_state[RETROK_PLUS] = pressed;
   else if (sym == XKB_KEY_comma)
     wl_key_state[RETROK_COMMA] = pressed;
   else if (sym == XKB_KEY_minus)
     wl_key_state[RETROK_MINUS] = pressed;
   else if (sym == XKB_KEY_period)
     wl_key_state[RETROK_PERIOD] = pressed;
   else if (sym == XKB_KEY_slash)
     wl_key_state[RETROK_SLASH] = pressed;
   else if (sym == XKB_KEY_0)
     wl_key_state[RETROK_0] = pressed;
   else if (sym == XKB_KEY_1)
     wl_key_state[RETROK_1] = pressed;
   else if (sym == XKB_KEY_2)
     wl_key_state[RETROK_2] = pressed;
   else if (sym == XKB_KEY_3)
     wl_key_state[RETROK_3] = pressed;
   else if (sym == XKB_KEY_4)
     wl_key_state[RETROK_4] = pressed;
   else if (sym == XKB_KEY_5)
     wl_key_state[RETROK_5] = pressed;
   else if (sym == XKB_KEY_5)
     wl_key_state[RETROK_6] = pressed;
   else if (sym == XKB_KEY_7)
     wl_key_state[RETROK_7] = pressed;
   else if (sym == XKB_KEY_8)
     wl_key_state[RETROK_8] = pressed;
   else if (sym == XKB_KEY_9)
     wl_key_state[RETROK_9] = pressed;
   else if (sym == XKB_KEY_colon)
     wl_key_state[RETROK_COLON] = pressed;
   else if (sym == XKB_KEY_semicolon)
     wl_key_state[RETROK_SEMICOLON] = pressed;
   else if (sym == XKB_KEY_less)
     wl_key_state[RETROK_LESS] = pressed;
   else if (sym == XKB_KEY_equal)
     wl_key_state[RETROK_EQUALS] = pressed;
   else if (sym == XKB_KEY_greater)
     wl_key_state[RETROK_GREATER] = pressed;
   else if (sym == XKB_KEY_question)
     wl_key_state[RETROK_QUESTION] = pressed;
   else if (sym == XKB_KEY_at)
     wl_key_state[RETROK_AT] = pressed;
   else if (sym == XKB_KEY_bracketleft)
     wl_key_state[RETROK_LEFTBRACKET] = pressed;
   else if (sym == XKB_KEY_backslash)
     wl_key_state[RETROK_BACKSLASH] = pressed;
   else if (sym == XKB_KEY_bracketright)
     wl_key_state[RETROK_RIGHTBRACKET] = pressed;
   else if (sym == XKB_KEY_asciicircum)
     wl_key_state[RETROK_CARET] = pressed;
   else if (sym == XKB_KEY_underscore)
     wl_key_state[RETROK_UNDERSCORE] = pressed;
   else if (sym == XKB_KEY_grave)
     wl_key_state[RETROK_BACKQUOTE] = pressed;
   else if (sym == XKB_KEY_a)
     wl_key_state[RETROK_a] = pressed;
   else if (sym == XKB_KEY_b)
     wl_key_state[RETROK_b] = pressed;
   else if (sym == XKB_KEY_c)
     wl_key_state[RETROK_c] = pressed;
   else if (sym == XKB_KEY_d)
     wl_key_state[RETROK_d] = pressed;
   else if (sym == XKB_KEY_e)
     wl_key_state[RETROK_e] = pressed;
   else if (sym == XKB_KEY_f)
     wl_key_state[RETROK_f] = pressed;
   else if (sym == XKB_KEY_g)
     wl_key_state[RETROK_g] = pressed;
   else if (sym == XKB_KEY_h)
     wl_key_state[RETROK_h] = pressed;
   else if (sym == XKB_KEY_i)
     wl_key_state[RETROK_i] = pressed;
   else if (sym == XKB_KEY_j)
     wl_key_state[RETROK_j] = pressed;
   else if (sym == XKB_KEY_k)
     wl_key_state[RETROK_k] = pressed;
   else if (sym == XKB_KEY_l)
     wl_key_state[RETROK_l] = pressed;
   else if (sym == XKB_KEY_m)
     wl_key_state[RETROK_m] = pressed;
   else if (sym == XKB_KEY_n)
     wl_key_state[RETROK_n] = pressed;
   else if (sym == XKB_KEY_o)
     wl_key_state[RETROK_o] = pressed;
   else if (sym == XKB_KEY_p)
     wl_key_state[RETROK_p] = pressed;
   else if (sym == XKB_KEY_q)
     wl_key_state[RETROK_q] = pressed;
   else if (sym == XKB_KEY_r)
     wl_key_state[RETROK_r] = pressed;
   else if (sym == XKB_KEY_s)
     wl_key_state[RETROK_s] = pressed;
   else if (sym == XKB_KEY_t)
     wl_key_state[RETROK_t] = pressed;
   else if (sym == XKB_KEY_u)
     wl_key_state[RETROK_u] = pressed;
   else if (sym == XKB_KEY_v)
     wl_key_state[RETROK_v] = pressed;
   else if (sym == XKB_KEY_w)
     wl_key_state[RETROK_w] = pressed;
   else if (sym == XKB_KEY_x)
     wl_key_state[RETROK_x] = pressed;
   else if (sym == XKB_KEY_y)
     wl_key_state[RETROK_y] = pressed;
   else if (sym == XKB_KEY_z)
     wl_key_state[RETROK_z] = pressed;
   else if (sym == XKB_KEY_Delete)
     wl_key_state[RETROK_DELETE] = pressed;
   else if (sym == XKB_KEY_KP_0)
     wl_key_state[RETROK_KP0] = pressed;
   else if (sym == XKB_KEY_KP_1)
     wl_key_state[RETROK_KP1] = pressed;
   else if (sym == XKB_KEY_KP_2)
     wl_key_state[RETROK_KP2] = pressed;
   else if (sym == XKB_KEY_KP_3)
     wl_key_state[RETROK_KP3] = pressed;
   else if (sym == XKB_KEY_KP_4)
     wl_key_state[RETROK_KP4] = pressed;
   else if (sym == XKB_KEY_KP_5)
     wl_key_state[RETROK_KP5] = pressed;
   else if (sym == XKB_KEY_KP_6)
     wl_key_state[RETROK_KP6] = pressed;
   else if (sym == XKB_KEY_KP_7)
     wl_key_state[RETROK_KP7] = pressed;
   else if (sym == XKB_KEY_KP_8)
     wl_key_state[RETROK_KP8] = pressed;
   else if (sym == XKB_KEY_KP_9)
     wl_key_state[RETROK_KP9] = pressed;
   else if (sym == XKB_KEY_KP_Separator)
     wl_key_state[RETROK_KP_PERIOD] = pressed;
   else if (sym == XKB_KEY_KP_Divide)
     wl_key_state[RETROK_KP_DIVIDE] = pressed;
   else if (sym == XKB_KEY_KP_Multiply)
     wl_key_state[RETROK_KP_MULTIPLY] = pressed;
   else if (sym == XKB_KEY_KP_Subtract)
     wl_key_state[RETROK_KP_MINUS] = pressed;
   else if (sym == XKB_KEY_KP_Add)
     wl_key_state[RETROK_KP_PLUS] = pressed;
   else if (sym == XKB_KEY_KP_Enter)
     wl_key_state[RETROK_KP_ENTER] = pressed;
   else if (sym == XKB_KEY_KP_Equal)
     wl_key_state[RETROK_KP_EQUALS] = pressed;
   else if (sym == XKB_KEY_Up)
     wl_key_state[RETROK_UP] = pressed;
   else if (sym == XKB_KEY_Down)
     wl_key_state[RETROK_DOWN] = pressed;
   else if (sym == XKB_KEY_Right)
     wl_key_state[RETROK_RIGHT] = pressed;
   else if (sym == XKB_KEY_Left)
     wl_key_state[RETROK_LEFT] = pressed;
   else if (sym == XKB_KEY_Insert)
     wl_key_state[RETROK_INSERT] = pressed;
   else if (sym == XKB_KEY_Home)
     wl_key_state[RETROK_HOME] = pressed;
   else if (sym == XKB_KEY_End)
     wl_key_state[RETROK_END] = pressed;
   else if (sym == XKB_KEY_Page_Up)
     wl_key_state[RETROK_PAGEUP] = pressed;
   else if (sym == XKB_KEY_Page_Down)
     wl_key_state[RETROK_PAGEDOWN] = pressed;
   else if (sym == XKB_KEY_F1)
     wl_key_state[RETROK_F1] = pressed;
   else if (sym == XKB_KEY_F2)
     wl_key_state[RETROK_F2] = pressed;
   else if (sym == XKB_KEY_F3)
     wl_key_state[RETROK_F3] = pressed;
   else if (sym == XKB_KEY_F4)
     wl_key_state[RETROK_F4] = pressed;
   else if (sym == XKB_KEY_F5)
     wl_key_state[RETROK_F5] = pressed;
   else if (sym == XKB_KEY_F6)
     wl_key_state[RETROK_F6] = pressed;
   else if (sym == XKB_KEY_F7)
     wl_key_state[RETROK_F7] = pressed;
   else if (sym == XKB_KEY_F8)
     wl_key_state[RETROK_F8] = pressed;
   else if (sym == XKB_KEY_F9)
     wl_key_state[RETROK_F9] = pressed;
   else if (sym == XKB_KEY_F10)
     wl_key_state[RETROK_F10] = pressed;
   else if (sym == XKB_KEY_F11)
     wl_key_state[RETROK_F11] = pressed;
   else if (sym == XKB_KEY_F12)
     wl_key_state[RETROK_F12] = pressed;
   else if (sym == XKB_KEY_F13)
     wl_key_state[RETROK_F13] = pressed;
   else if (sym == XKB_KEY_F14)
     wl_key_state[RETROK_F14] = pressed;
   else if (sym == XKB_KEY_F15)
     wl_key_state[RETROK_F15] = pressed;
   else if (sym == XKB_KEY_Num_Lock)
     wl_key_state[RETROK_NUMLOCK] = pressed;
   else if (sym == XKB_KEY_Caps_Lock)
     wl_key_state[RETROK_CAPSLOCK] = pressed;
   else if (sym == XKB_KEY_Scroll_Lock)
     wl_key_state[RETROK_SCROLLOCK] = pressed;
   else if (sym == XKB_KEY_Shift_R)
     wl_key_state[RETROK_RSHIFT] = pressed;
   else if (sym == XKB_KEY_Shift_L)
     wl_key_state[RETROK_LSHIFT] = pressed;
   else if (sym == XKB_KEY_Control_R)
     wl_key_state[RETROK_RCTRL] = pressed;
   else if (sym == XKB_KEY_Control_L)
     wl_key_state[RETROK_LCTRL] = pressed;
   else if (sym == XKB_KEY_Alt_R)
     wl_key_state[RETROK_RALT] = pressed;
   else if (sym == XKB_KEY_Alt_L)
     wl_key_state[RETROK_LALT] = pressed;
   else if (sym == XKB_KEY_Meta_R)
     wl_key_state[RETROK_RMETA] = pressed;
   else if (sym == XKB_KEY_Meta_L)
     wl_key_state[RETROK_LMETA] = pressed;
   else if (sym == XKB_KEY_Super_L)
     wl_key_state[RETROK_LSUPER] = pressed;
   else if (sym == XKB_KEY_Super_R)
     wl_key_state[RETROK_RSUPER] = pressed;
   else if (sym == XKB_KEY_Mode_switch)
     wl_key_state[RETROK_MODE] = pressed;
   /*else if (sym == XKB_KEY_) 
     wl_key_state[RETROK_COMPOSE] = pressed;*/
   else if (sym == XKB_KEY_Help)
     wl_key_state[RETROK_HELP] = pressed;
   else if (sym == XKB_KEY_Print)
     wl_key_state[RETROK_PRINT] = pressed;
   else if (sym == XKB_KEY_Sys_Req)
     wl_key_state[RETROK_SYSREQ] = pressed;
   else if (sym == XKB_KEY_Break)
     wl_key_state[RETROK_BREAK] = pressed;
   else if (sym == XKB_KEY_Menu)
     wl_key_state[RETROK_MENU] = pressed;
   /*else if (sym == XKB_KEY_Power)
     wl_key_state[RETROK_POWER] = pressed;*/
   /*else if (sym == XKB_KEY_)
     wl_key_state[RETROK_EURO] = pressed;*/
   else if (sym == XKB_KEY_Undo)
     wl_key_state[RETROK_UNDO] = pressed;
}

static void keyboard_handle_modifiers(void* data,
struct wl_keyboard* keyboard,
uint32_t serial,
uint32_t modsDepressed,
uint32_t modsLatched,
uint32_t modsLocked,
uint32_t group)
{
   /* TODO */
}

static const struct wl_keyboard_listener keyboard_listener = {
   keyboard_handle_keymap,
   keyboard_handle_enter,
   keyboard_handle_leave,
   keyboard_handle_key,
   keyboard_handle_modifiers,
};

static void pointer_handle_enter(void* data,
struct wl_pointer* pointer,
uint32_t serial,
struct wl_surface* surface,
wl_fixed_t sx,
wl_fixed_t sy)
{
   mouse.abs_x = wl_fixed_to_int(sx);
   mouse.abs_y = wl_fixed_to_int(sy);
   wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}

static void pointer_handle_leave(void* data,
struct wl_pointer* pointer,
uint32_t serial,
struct wl_surface* surface)
{
   /* Nothing to do here */
}

static void pointer_handle_motion(void* data,
struct wl_pointer* pointer,
uint32_t time,
wl_fixed_t sx,
wl_fixed_t sy)
{
   mouse.abs_x = wl_fixed_to_int(sx);
   mouse.abs_y = wl_fixed_to_int(sy);
}

static void pointer_handle_button(void* data,
struct wl_pointer* wl_pointer,
uint32_t serial,
uint32_t time,
uint32_t button,
uint32_t state)
{
   int pressed;

   switch(state)
   {
      case WL_POINTER_BUTTON_STATE_RELEASED:
         pressed = 0;
         break;
      case WL_POINTER_BUTTON_STATE_PRESSED:
         pressed = 1;
         break;
   }

   switch(button)
   {
      case BTN_LEFT:
         mouse.l = pressed;
         break;
      case BTN_RIGHT:
         mouse.r = pressed;
         break;
      case BTN_MIDDLE:
         mouse.m = pressed;
         break;
   }
}

static void pointer_handle_axis(void* data,
struct wl_pointer* wl_pointer,
uint32_t time,
uint32_t axis,
wl_fixed_t value)
{
   switch(axis)
   {
      case WL_POINTER_AXIS_VERTICAL_SCROLL:
	 if(wl_fixed_to_int(value) < 0)
	 {
	    mouse.wu = 1;
	    mouse.wd = 0;
	 }
	 else
	 {
	    mouse.wu = 0;
	    mouse.wd = 1;
	 }
         break;
      case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
	 if(wl_fixed_to_int(value) < 0)
	 {
	    mouse.wr = 1;
	    mouse.wl = 0;
	 }
	 else
	 {
	    mouse.wr = 0;
	    mouse.wl = 1;
	 }
         break;
   }
}


static const struct wl_pointer_listener pointer_listener = {
   pointer_handle_enter,
   pointer_handle_leave,
   pointer_handle_motion,
   pointer_handle_button,
   pointer_handle_axis,
};

static void touch_handle_down(void *data,
struct wl_touch *wl_touch,
uint32_t serial,
uint32_t time,
struct wl_surface *surface,
int32_t id,
wl_fixed_t x,
wl_fixed_t y)
{
   if (num_active_touches < MAX_TOUCHES)
   {
     for (int i=0;i<MAX_TOUCHES;i++)
     {
       // Use next empty slot
       if (!active_touch_positions[i].active)
       {
         active_touch_positions[num_active_touches].active = true;
         active_touch_positions[num_active_touches].id = id;
         active_touch_positions[num_active_touches].x = (unsigned) wl_fixed_to_int(x);
         active_touch_positions[num_active_touches].y = (unsigned) wl_fixed_to_int(y);
         num_active_touches++;
         break;
       }
     }
   }
}

static void reorder_touches()
{
   if (num_active_touches == 0)
      return;

   for (int i=0;i<MAX_TOUCHES;i++)
   {
      if (!active_touch_positions[i].active)
      {
         for (int j=i+1;j<MAX_TOUCHES;j++)
         {
            if (active_touch_positions[j].active)
            {
               active_touch_positions[i].active = active_touch_positions[j].active;
               active_touch_positions[i].id = active_touch_positions[j].id;
               active_touch_positions[i].x = active_touch_positions[j].x;
               active_touch_positions[i].y = active_touch_positions[j].y;

               active_touch_positions[j].active = false;
               active_touch_positions[j].id = -1;
               active_touch_positions[j].x = (unsigned) 0;
               active_touch_positions[j].y = (unsigned) 0;
               break;
            }
            if (j == MAX_TOUCHES)
               return;
         }
      }
   }
}

static void touch_handle_up(void *data,
struct wl_touch *wl_touch,
uint32_t serial,
uint32_t time,
int32_t id)
{
   for (int i=0;i<MAX_TOUCHES;i++)
   {
     if (active_touch_positions[i].active && active_touch_positions[i].id == id)
     {
       active_touch_positions[i].active = false;
       active_touch_positions[i].id = -1;
       active_touch_positions[i].x = (unsigned) 0;
       active_touch_positions[i].y = (unsigned) 0;
       num_active_touches--;
     }
   }
   reorder_touches();
}

static void touch_handle_motion(void *data,
struct wl_touch *wl_touch,
uint32_t time,
int32_t id,
wl_fixed_t x,
wl_fixed_t y)
{
   for (int i=0;i<MAX_TOUCHES;i++)
   {
     if (active_touch_positions[i].active && active_touch_positions[i].id == id)
     {
       active_touch_positions[i].x = (unsigned) wl_fixed_to_int(x);
       active_touch_positions[i].y = (unsigned) wl_fixed_to_int(y);
     }
   }
}

static void touch_handle_frame(void *data,
struct wl_touch *wl_touch)
{
   /* TODO */
}

static void touch_handle_cancel(void *data,
struct wl_touch *wl_touch)
{
   //If i understand the spec correctly we have to reset all touches here
   //since they were not ment for us anyway
   for (int i=0;i<MAX_TOUCHES;i++)
   {
       active_touch_positions[i].active = false;
       active_touch_positions[i].id = -1;
       active_touch_positions[i].x = (unsigned) 0;
       active_touch_positions[i].y = (unsigned) 0;
   }
   num_active_touches = 0;
}

static const struct wl_touch_listener touch_listener = {
   touch_handle_down,
   touch_handle_up,
   touch_handle_motion,
   touch_handle_frame,
   touch_handle_cancel,
};

static void seat_handle_capabilities(void *data,
struct wl_seat *seat, unsigned caps)
{
   wayland_input_t *input = (wayland_input_t*)data;

   if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard)
   {
      input->keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(input->keyboard, &keyboard_listener, input);
   }
   else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard)
   {
      wl_keyboard_destroy(input->keyboard);
      input->keyboard = NULL;
   }
   if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer)
   {
      input->pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(input->pointer, &pointer_listener, input);
   }
   else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer)
   {
      wl_pointer_destroy(input->pointer);
      input->pointer = NULL;
   }
   if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch)
   {
      input->touch = wl_seat_get_touch(seat);
      wl_touch_add_listener(input->touch, &touch_listener, input);
   }
   else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch)
   {
      wl_touch_destroy(input->touch);
      input->touch = NULL;
   }
}

static void seat_handle_name(void *data,
struct wl_seat *seat, const char *name)
{
   /* TODO */
}

/* Seat callbacks - TODO/FIXME */
static const struct wl_seat_listener seat_listener = {
   seat_handle_capabilities,
   seat_handle_name,
};


#ifdef HAVE_VULKAN
static void *gfx_ctx_wl_get_context_data(void *data)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;
   return &wl->vk.context;
}

static void vulkan_acquire_next_image(gfx_ctx_vulkan_data_t *vk)
{
   unsigned index;
   VkResult err;
   VkFence fence;
   VkFence *next_fence;
   VkSemaphoreCreateInfo sem_info = 
   { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

   vkCreateFence(vk->context.device, &info, NULL, &fence);

   err = vk->fpAcquireNextImageKHR(vk->context.device,
         vk->swapchain, UINT64_MAX,
         VK_NULL_HANDLE, fence, &vk->context.current_swapchain_index);

   index = vk->context.current_swapchain_index;
   if (vk->context.swapchain_semaphores[index] == VK_NULL_HANDLE)
      vkCreateSemaphore(vk->context.device, &sem_info,
            NULL, &vk->context.swapchain_semaphores[index]);

   vkWaitForFences(vk->context.device, 1, &fence, true, UINT64_MAX);
   vkDestroyFence(vk->context.device, fence, NULL);

   next_fence = &vk->context.swapchain_fences[index];
   if (*next_fence != VK_NULL_HANDLE)
   {
      vkWaitForFences(vk->context.device, 1, next_fence, true, UINT64_MAX);
      vkResetFences(vk->context.device, 1, next_fence);
   }
   else
      vkCreateFence(vk->context.device, &info, NULL, next_fence);

   if (err != VK_SUCCESS)
   {
      RARCH_LOG("[Wayland/Vulkan]: AcquireNextImage failed, invalidating swapchain.\n");
      vk->context.invalid_swapchain = true;
   }
}

static bool vulkan_create_swapchain(gfx_ctx_vulkan_data_t *vk,
      unsigned width, unsigned height,
      unsigned swap_interval)
{
   unsigned i;
   uint32_t format_count;
   uint32_t desired_swapchain_images;
   VkSurfaceCapabilitiesKHR surface_properties;
   VkSurfaceFormatKHR formats[256];
   VkSurfaceFormatKHR format;
   VkExtent2D swapchain_size;
   VkSwapchainKHR old_swapchain;
   VkSurfaceTransformFlagBitsKHR pre_transform;
   VkPresentModeKHR swapchain_present_mode = swap_interval 
      ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
   VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };

   RARCH_LOG("[Wayland/Vulkan]: Creating swapchain with present mode: %u\n",
         (unsigned)swapchain_present_mode);

   vk->fpGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->context.gpu,
         vk->vk_surface, &surface_properties);
   vk->fpGetPhysicalDeviceSurfaceFormatsKHR(vk->context.gpu,
         vk->vk_surface, &format_count, NULL);
   vk->fpGetPhysicalDeviceSurfaceFormatsKHR(vk->context.gpu,
         vk->vk_surface, &format_count, formats);

   if (format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
   {
      format = formats[0];
      format.format = VK_FORMAT_B8G8R8A8_UNORM;
   }
   else
   {
      if (format_count == 0)
      {
         RARCH_ERR("[Wayland Vulkan]: Surface has no formats.\n");
         return false;
      }

      format = formats[0];
   }

   if (surface_properties.currentExtent.width == -1)
   {
      swapchain_size.width     = width;
      swapchain_size.height    = height;
   }
   else
      swapchain_size           = surface_properties.currentExtent;

   desired_swapchain_images    = surface_properties.minImageCount + 1;
   if ((surface_properties.maxImageCount > 0) 
         && (desired_swapchain_images > surface_properties.maxImageCount))
      desired_swapchain_images = surface_properties.maxImageCount;

   if (surface_properties.supportedTransforms 
         & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
      pre_transform            = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   else
      pre_transform            = surface_properties.currentTransform;

   old_swapchain               = vk->swapchain;

   info.surface                = vk->vk_surface;
   info.minImageCount          = desired_swapchain_images;
   info.imageFormat            = format.format;
   info.imageColorSpace        = format.colorSpace;
   info.imageExtent.width      = swapchain_size.width;
   info.imageExtent.height     = swapchain_size.height;
   info.imageArrayLayers       = 1;
   info.imageUsage             = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   info.imageSharingMode       = VK_SHARING_MODE_EXCLUSIVE;
   info.preTransform           = pre_transform;
   info.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   info.presentMode            = swapchain_present_mode;
   info.clipped                = true;
   info.oldSwapchain           = old_swapchain;

   vk->fpCreateSwapchainKHR(vk->context.device, &info, NULL, &vk->swapchain);
   if (old_swapchain != VK_NULL_HANDLE)
      vk->fpDestroySwapchainKHR(vk->context.device, old_swapchain, NULL);

   vk->context.swapchain_width  = swapchain_size.width;
   vk->context.swapchain_height = swapchain_size.height;

   /* Make sure we create a backbuffer format that is as we expect. */
   switch (format.format)
   {
      case VK_FORMAT_B8G8R8A8_SRGB:
         vk->context.swapchain_format  = VK_FORMAT_B8G8R8A8_UNORM;
         vk->context.swapchain_is_srgb = true;
         break;

      case VK_FORMAT_R8G8B8A8_SRGB:
         vk->context.swapchain_format  = VK_FORMAT_R8G8B8A8_UNORM;
         vk->context.swapchain_is_srgb = true;
         break;

      case VK_FORMAT_R8G8B8_SRGB:
         vk->context.swapchain_format  = VK_FORMAT_R8G8B8_UNORM;
         vk->context.swapchain_is_srgb = true;
         break;

      case VK_FORMAT_B8G8R8_SRGB:
         vk->context.swapchain_format  = VK_FORMAT_B8G8R8_UNORM;
         vk->context.swapchain_is_srgb = true;
         break;

      default:
         vk->context.swapchain_format  = format.format;
         break;
   }

   vk->fpGetSwapchainImagesKHR(vk->context.device, vk->swapchain,
         &vk->context.num_swapchain_images, NULL);
   vk->fpGetSwapchainImagesKHR(vk->context.device, vk->swapchain,
         &vk->context.num_swapchain_images, vk->context.swapchain_images);

   for (i = 0; i < vk->context.num_swapchain_images; i++)
   {
      if (vk->context.swapchain_fences[i])
      {
         vkDestroyFence(vk->context.device,
               vk->context.swapchain_fences[i], NULL);
         vk->context.swapchain_fences[i] = VK_NULL_HANDLE;
      }
   }

   vulkan_acquire_next_image(vk);

   return true;
}

static void vulkan_present(gfx_ctx_vulkan_data_t *vk, unsigned index)
{
   VkResult result            = VK_SUCCESS;
   VkResult err               = VK_SUCCESS;
   VkPresentInfoKHR present   = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
   present.swapchainCount     = 1;
   present.pSwapchains        = &vk->swapchain;
   present.pImageIndices      = &index;
   present.pResults           = &result;
   present.waitSemaphoreCount = 1;
   present.pWaitSemaphores    = &vk->context.swapchain_semaphores[index];

   /* Better hope QueuePresent doesn't block D: */
   slock_lock(vk->context.queue_lock);
   err = vk->fpQueuePresentKHR(vk->context.queue, &present);

   if (err != VK_SUCCESS || result != VK_SUCCESS)
   {
      RARCH_LOG("[Wayland/Vulkan]: QueuePresent failed, invalidating swapchain.\n");
      vk->context.invalid_swapchain = true;
   }
   slock_unlock(vk->context.queue_lock);
}
#endif

static void gfx_ctx_wl_swap_buffers(void *data)
{
   gfx_ctx_wayland_data_t *wl = (gfx_ctx_wayland_data_t*)data;

   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         egl_swap_buffers(data);
#endif
         break;
      case GFX_CTX_VULKAN_API:
#ifdef HAVE_VULKAN
         vulkan_present(&wl->vk, wl->vk.context.current_swapchain_index);
         vulkan_acquire_next_image(&wl->vk);
         flush_wayland_fd(wl);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }
}

static gfx_ctx_proc_t gfx_ctx_wl_get_proc_address(const char *symbol)
{
   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         return egl_get_proc_address(symbol);
#else
         break;
#endif
      case GFX_CTX_NONE:
      default:
         break;
   }

   return NULL;
}

static void gfx_ctx_wl_bind_hw_render(void *data, bool enable)
{
   switch (wl_api)
   {
      case GFX_CTX_OPENGL_API:
      case GFX_CTX_OPENGL_ES_API:
      case GFX_CTX_OPENVG_API:
#ifdef HAVE_EGL
         egl_bind_hw_render(data, enable);
#endif
         break;
      case GFX_CTX_NONE:
      default:
         break;
   }
}

const gfx_ctx_driver_t gfx_ctx_wayland = {
   gfx_ctx_wl_init,
   gfx_ctx_wl_destroy,
   gfx_ctx_wl_bind_api,
   gfx_ctx_wl_set_swap_interval,
   gfx_ctx_wl_set_video_mode,
   gfx_ctx_wl_get_video_size,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_metrics */
   NULL,
   gfx_ctx_wl_update_window_title,
   gfx_ctx_wl_check_window,
   gfx_ctx_wl_set_resize,
   gfx_ctx_wl_has_focus,
   gfx_ctx_wl_suppress_screensaver,
   gfx_ctx_wl_has_windowed,
   gfx_ctx_wl_swap_buffers,
   gfx_ctx_wl_input_driver,
   gfx_ctx_wl_get_proc_address,
   NULL,
   NULL,
   NULL,
   "wayland",
   gfx_ctx_wl_bind_hw_render,
#ifdef HAVE_VULKAN
   gfx_ctx_wl_get_context_data
#else
   NULL
#endif
};
