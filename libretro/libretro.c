#include "libretro.h"
#include "libretro_core_options.h"
#include "log.h"
#include <string.h>
#include <streams/file_stream.h>

#include "../types.h"
#include "../state.h"
#include "../neopopsound.h"
#include "../sound.h"
#include "../input.h"
#include "../flash.h"
#include "../tlcs900h.h"
#include "../race-memory.h"
#include "../graphics.h"
#include "../state.h"

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

#define RACE_NAME_MODULE "race"
#define RACE_NAME "RACE"
#define RACE_VERSION "v2.16"
#define RACE_EXTENSIONS "ngp|ngc|ngpc|npc"
#define RACE_TIMING_FPS 60.25
#define RACE_GEOMETRY_BASE_W 160
#define RACE_GEOMETRY_BASE_H 152
#define RACE_GEOMETRY_MAX_W 160
#define RACE_GEOMETRY_MAX_H 152
#define RACE_GEOMETRY_ASPECT_RATIO 1.05

#define FB_WIDTH 160
#define FB_HEIGHT 152

/* Frameskipping Support */

static unsigned frameskip_type             = 0;
static unsigned frameskip_threshold        = 0;
static uint16_t frameskip_counter          = 0;

static bool retro_audio_buff_active        = false;
static unsigned retro_audio_buff_occupancy = 0;
static bool retro_audio_buff_underrun      = false;
/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 60

static unsigned audio_latency              = 0;
static bool update_audio_latency           = false;

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frameskip disabled - frontend does not support audio buffer status monitoring.\n");

         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         audio_latency              = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / (float)RACE_TIMING_FPS;

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }

   update_audio_latency = true;
}

/* core options */
#if !defined(SF2000)
static int RETRO_SAMPLE_RATE = 44100;
#else
static int RETRO_SAMPLE_RATE = 11025;
#endif

struct ngp_screen* screen;
int setting_ngp_language; /* 0x6F87 - language */

int gfx_hacks = 0;
int tipo_consola = 0; /* 0x6F91 - OS version */
static bool libretro_supports_input_bitmasks;

char retro_save_directory[2048];

struct map
{
   unsigned retro;
   unsigned ngp;
};

static struct map btn_map[] = {
   { RETRO_DEVICE_ID_JOYPAD_A, 0x20 },
   { RETRO_DEVICE_ID_JOYPAD_B, 0x10 },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x08 },
   { RETRO_DEVICE_ID_JOYPAD_LEFT, 0x04 },
   { RETRO_DEVICE_ID_JOYPAD_UP, 0x01 },
   { RETRO_DEVICE_ID_JOYPAD_DOWN, 0x02 },
   { RETRO_DEVICE_ID_JOYPAD_START, 0x40 },
};

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void graphics_paint(unsigned char render)
{
   video_cb((bool)render ? screen->pixels : NULL,
         screen->w, screen->h, FB_WIDTH << 1);
}

static void check_variables(bool first_run)
{
   struct retro_variable var  = {0};
   unsigned dark_filter_level = 0;
   unsigned old_frameskip_type;

   if (first_run)
   {
      var.key = "race_language";
      var.value = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         /* user must manually restart core for change to happen
          * > 0: English
          * > 1: Japanese
          */
         if (!strcmp(var.value, "japanese"))
            setting_ngp_language = 1;
         else if (!strcmp(var.value, "english"))
            setting_ngp_language = 0;
      }
   }

   var.key   = "race_dark_filter_level";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      dark_filter_level = (unsigned)(atoi(var.value));
   graphicsSetDarkFilterLevel(dark_filter_level);

   old_frameskip_type = frameskip_type;
   frameskip_type     = 0;
   var.key            = "race_frameskip";
   var.value          = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
         frameskip_type = 1;
      else if (!strcmp(var.value, "manual"))
         frameskip_type = 2;
   }

   frameskip_threshold = 33;
   var.key             = "race_frameskip_threshold";
   var.value           = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   /* Reinitialise frameskipping, if required */
   if ((frameskip_type != old_frameskip_type) && !first_run)
      init_frameskip();
}

void retro_init(void)
{
   char *dir = NULL;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   audio_latency              = 0;
   update_audio_latency       = false;

   /* set up some logging */
   init_log(environ_cb);

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      sprintf(retro_save_directory, "%s%c", dir, path_default_slash_c());

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[RACE]: Save directory: %s.\n", retro_save_directory);

   if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt) && log_cb)
      log_cb(RETRO_LOG_ERROR, "[could not set RGB565]\n");

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_input_bitmasks = true;
}

void retro_reset(void)
{
   flashShutdown();
   system_sound_chipreset();
   mainemuinit();
}

void retro_deinit(void)
{
    flashShutdown();
    libretro_supports_input_bitmasks = false;
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   static const struct retro_system_content_info_override content_overrides[] = {
      {
         RACE_EXTENSIONS, /* extensions */
#if defined(LOW_MEMORY)
         true,            /* need_fullpath */
#else
         false,           /* need_fullpath */
#endif
         false            /* persistent_data */
      },
      { NULL, false, false }
   };

   environ_cb = cb;

   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   libretro_set_core_options(environ_cb);
   environ_cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE,
         (void*)content_overrides);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static unsigned get_race_input_bitmasks(void)
{
   unsigned i = 0;
   unsigned res = 0;
   unsigned ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
      res |= (ret & (1 << btn_map[i].retro)) ? btn_map[i].ngp : 0;
   return res;
}

static unsigned get_race_input(void)
{
   unsigned i = 0;
   unsigned res = 0;
   for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
      res |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, btn_map[i].retro) ? btn_map[i].ngp : 0;
   return res;
}

static void race_input(void)
{
   ngpInputState = 0;
   input_poll_cb();
   if (libretro_supports_input_bitmasks)
      ngpInputState = get_race_input_bitmasks();
   else
      ngpInputState = get_race_input();
}

static bool race_initialize_sound(void)
{
    system_sound_chipreset();
    return true;
}

static bool race_initialize_system(const char *gamepath,
      const unsigned char *gamedata, size_t gamesize)
{
   mainemuinit();

   if (!handleInputFile(gamepath, gamedata, (int)gamesize))
   {
      handle_error("ERROR handleInputFile");
      return false;
   }

   return true;
}

void retro_set_controller_port_device(unsigned a, unsigned b) { }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = RACE_NAME;
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif

   info->need_fullpath    = true;
   info->library_version  = RACE_VERSION GIT_VERSION;
   info->valid_extensions = RACE_EXTENSIONS;
   info->block_extract    = false;
}
void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = RACE_TIMING_FPS;
   info->timing.sample_rate    = RETRO_SAMPLE_RATE;
   info->geometry.base_width   = RACE_GEOMETRY_BASE_W;
   info->geometry.base_height  = RACE_GEOMETRY_BASE_H;
   info->geometry.max_width    = RACE_GEOMETRY_MAX_W;
   info->geometry.max_height   = RACE_GEOMETRY_MAX_H;
   info->geometry.aspect_ratio = RACE_GEOMETRY_ASPECT_RATIO;
}

#define CPU_FREQ 6144000

void retro_run(void)
{
   unsigned i;
   bool updated = false;
   static int16_t sampleBuffer[2048];
   static int16_t stereoBuffer[2048];
   int16_t *p = NULL;
   uint16_t samplesPerFrame;
   int skipFrame = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);

   race_input();

   /* Check whether current frame should be skipped */
   if ((frameskip_type > 0) && retro_audio_buff_active)
   {
      switch (frameskip_type)
      {
         case 1: /* auto */
            skipFrame = retro_audio_buff_underrun ? 1 : 0;
            break;
         case 2: /* manual */
            skipFrame = (retro_audio_buff_occupancy < frameskip_threshold) ? 1 : 0;
            break;
         default:
            skipFrame = 0;
            break;
      }

      if (!skipFrame || (frameskip_counter >= FRAMESKIP_MAX))
      {
         skipFrame         = 0;
         frameskip_counter = 0;
      }
      else
         frameskip_counter++;
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   tlcs_execute(CPU_FREQ / 60, skipFrame);

   /* Get the number of samples in a frame */
   samplesPerFrame = RETRO_SAMPLE_RATE / 60;

   memset(sampleBuffer, 0, samplesPerFrame * sizeof(int16_t));

   sound_update((uint16_t*)sampleBuffer, samplesPerFrame * sizeof(int16_t)); /* Get sound data */
   dac_update((uint16_t*)sampleBuffer, samplesPerFrame * sizeof(int16_t));

   p = stereoBuffer;
   
   for (i = 0; i < samplesPerFrame; i++)
   {
      p[0] = sampleBuffer[i];
      p[1] = sampleBuffer[i];
      p += 2;
   }

   audio_batch_cb(stereoBuffer, samplesPerFrame);
}

size_t retro_serialize_size(void)
{
   return state_get_size();
}

bool retro_serialize(void *data, size_t size)
{
   return state_store_mem(data);
}

bool retro_unserialize(const void *data, size_t size)
{
   int ret = state_restore_mem((void*)data);
   return (ret == 1);
}

bool retro_load_game(const struct retro_game_info *info)
{
   const struct retro_game_info_ext *info_ext = NULL;
   const unsigned char *content_data          = NULL;
   size_t content_size                        = 0;
   char content_path[_MAX_PATH];

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Option" },

      { 0 },
   };

   content_path[0] = '\0';

   /* Attempt to fetch extended game info */
   if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info_ext))
   {
#if !defined(LOW_MEMORY)
      content_data = (const unsigned char *)info_ext->data;
      content_size = info_ext->size;
#endif
      if (info_ext->file_in_archive)
      {
         /* We don't have a 'physical' file in this
          * case, but the core still needs a filename
          * in order to build the save file path.
          * We therefore fake it, using the content
          * directory, canonical content name, and
          * content file extension */
         snprintf(content_path, sizeof(content_path), "%s%c%s.%s",
               info_ext->dir, path_default_slash_c(),
               info_ext->name, info_ext->ext);
      }
      else
      {
         strncpy(content_path, info_ext->full_path, sizeof(content_path));
         content_path[sizeof(content_path) - 1] = '\0';
      }
   }
   else
   {
      if (!info || !info->path)
         return false;

      strncpy(content_path, info->path, sizeof(content_path));
      content_path[sizeof(content_path) - 1] = '\0';
   }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   screen         = (struct ngp_screen*)calloc(1, sizeof(*screen));

   if (!screen)
      return false;

   screen->w      = FB_WIDTH;
   screen->h      = FB_HEIGHT;

   screen->pixels = calloc(1, FB_WIDTH * FB_HEIGHT * 2);

   if (!screen->pixels)
   {
      free(screen);
      return false;
   }

   check_variables(true);
   init_frameskip();

   if (!race_initialize_system(content_path,
         content_data, content_size))
      return false;

   if (!race_initialize_sound())
      return false;

   {
      /* TODO: Mappings might need updating
       * Size is based on what is exposed in Mednafen NGP */
      struct retro_memory_descriptor descs = {
         RETRO_MEMDESC_SYSTEM_RAM, mainram, 0, 0, 0, 0, 16384, "RAM"
      };
      struct retro_memory_map retro_map = {
         &descs, 1
      };
      environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retro_map);
   }

   return true;
}

bool retro_load_game_special(unsigned a, const struct retro_game_info *b, size_t c)
{
   return false;
}

void retro_unload_game(void)
{
   if (screen)
   {
      if (screen->pixels)
         free(screen->pixels);
      free(screen);
      screen = NULL;
   }
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned type)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
   return 0;
}
