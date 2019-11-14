#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>
#include "plus4lib/plus4emu.h"
#include "../common/circle.h"
#include "../common/emux_api.h"
#include "../common/keycodes.h"
#include "../common/overlay.h"
#include "../common/demo.h"
#include "../common/menu.h"

static Plus4VM            *vm = NULL;
static Plus4VideoDecoder  *videoDecoder = NULL;

// Global state variables
static uint8_t *fb_buf;
static int fb_pitch;
static int ui_trap;
static int wait_vsync;
static int ui_warp;
static int joy_latch_value[2];
static int is_tape_motor;
static int is_tape_motor_tick;
static int is_tape_seeking;
static int is_tape_seeking_dir;
static int is_tape_seeking_tick;
static double tape_counter_offset;

// Things that need to be saved and restored.
int reset_tape_with_cpu = 1;
int tape_feedback = 0;
int ram_size = 64;
int sid_model = 0;
int sid_write_access = 0;
int sid_digiblaster = 0;
char rom_basic[MAX_STR_VAL_LEN];
char rom_kernal[MAX_STR_VAL_LEN];
char rom_c0_lo[MAX_STR_VAL_LEN];
char rom_c0_hi[MAX_STR_VAL_LEN];
char rom_c1_lo[MAX_STR_VAL_LEN];
char rom_c1_hi[MAX_STR_VAL_LEN];
char rom_c2_lo[MAX_STR_VAL_LEN];
char rom_c2_hi[MAX_STR_VAL_LEN];
char rom_1541[MAX_STR_VAL_LEN];
char rom_1551[MAX_STR_VAL_LEN];
char rom_1581[MAX_STR_VAL_LEN];
int rom_basic_off;
int rom_kernal_off;
int rom_c0_lo_off;
int rom_c0_hi_off;
int rom_c1_lo_off;
int rom_c1_hi_off;
int rom_c2_lo_off;
int rom_c2_hi_off;
int rom_1541_off;
int rom_1551_off;
int rom_1581_lo_off;
int rom_1581_hi_off;

static struct menu_item *sid_model_item;
static struct menu_item *sid_write_access_item;
static struct menu_item *sid_digiblaster_item;
static struct menu_item *tape_feedback_item;
static struct menu_item *ram_size_item;

static struct menu_item *c0_lo_item;
static struct menu_item *c0_hi_item;
static struct menu_item *c1_lo_item;
static struct menu_item *c1_hi_item;
static struct menu_item *c2_lo_item;
static struct menu_item *c2_hi_item;

static struct menu_item *c0_lo_offset_item;
static struct menu_item *c0_hi_offset_item;
static struct menu_item *c1_lo_offset_item;
static struct menu_item *c1_hi_offset_item;
static struct menu_item *c2_lo_offset_item;
static struct menu_item *c2_hi_offset_item;

#define COLOR16(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

static void rtrim(char *txt) {
  if (!txt) return;
  int p=strlen(txt)-1;
  while (isspace(txt[p])) { txt[p] = '\0'; p--; }
}

static char* ltrim(char *txt) {
  if (!txt) return NULL;
  int p=0;
  while (isspace(txt[p])) { p++; }
  return txt+p;
}

static void get_key_and_value(char *line, char **key, char **value) {
   for (int i=0;i<strlen(line);i++) {
      if (line[i] == '=') {
         line[i] = '\0';
         *key = ltrim(&line[0]);
         rtrim(*key);
         *value = ltrim(&line[i+1]);
         rtrim(*value);
         return;
      }
   }
   *key = 0;
   *value = 0;
}

static void set_video_font(void) {
  int i;

  // Temporary for now. Need to figure out how to get this from the emulator's
  // ROM. Just read the file ourselves.
  uint8_t* chargen = malloc(4096); // never freed
  FILE* fp = fopen(rom_kernal, "r");
  fseek(fp, 0x1000, SEEK_SET);
  fread(chargen,1,4096,fp);
  fclose(fp);

  video_font = &chargen[0x400];
  raw_video_font = &chargen[0x000];
  for (i = 0; i < 256; ++i) {
    video_font_translate[i] = 8 * ascii_to_petscii[i];
  }
}

static void apply_sid_config() {
  int sid_flags = sid_model_item->choice_ints[sid_model_item->value];
  if (sid_write_access_item->value) sid_flags |= 0x2;
  Plus4VM_SetSIDConfiguration(vm, sid_flags, sid_digiblaster_item->value, 0);
}

static int apply_rom_config() {
  if (Plus4VM_LoadROM(vm, 0x00, rom_basic, 0) != PLUS4EMU_SUCCESS)
    return 1;

  if (Plus4VM_LoadROM(vm, 0x01, rom_kernal, 0) != PLUS4EMU_SUCCESS)
    return 1;

  if (Plus4VM_LoadROM(vm, 0x10, rom_1541, 0) != PLUS4EMU_SUCCESS)
    return 1;

  if (strlen(c0_lo_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 2, c0_lo_item->str_value, c0_lo_offset_item->value);
  if (strlen(c0_hi_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 3, c0_hi_item->str_value, c0_hi_offset_item->value);

  if (strlen(c1_lo_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 4, c1_lo_item->str_value, c1_lo_offset_item->value);
  if (strlen(c1_hi_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 5, c1_hi_item->str_value, c1_hi_offset_item->value);

  if (strlen(c2_lo_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 6, c2_lo_item->str_value, c2_lo_offset_item->value);
  if (strlen(c2_hi_item->str_value) > 0)
    Plus4VM_LoadROM(vm, 7, c2_hi_item->str_value, c2_hi_offset_item->value);

  return 0;
}

static int apply_settings() {
  // Here, we should make whatever calls are necessary to configure the VM
  // according to any settings that were loaded.
  apply_sid_config();
  Plus4VM_SetTapeFeedbackLevel(vm, tape_feedback);
  Plus4VM_SetRAMConfiguration(vm, ram_size, 0x99999999UL);
  return apply_rom_config();
}

// Reusing this from kernel to make implementing emux_kbd_set_latch_keyarr
// easier.
static long rowColToKeycode[8][8] = {
 {KEYCODE_Backspace,  KEYCODE_3,         KEYCODE_5, KEYCODE_7, KEYCODE_9, KEYCODE_Down,         KEYCODE_Left,         KEYCODE_1},
 {KEYCODE_Return,     KEYCODE_w,         KEYCODE_r, KEYCODE_y, KEYCODE_i, KEYCODE_p,            KEYCODE_Dash,         KEYCODE_BackQuote},
 {KEYCODE_BackSlash,  KEYCODE_a,         KEYCODE_d, KEYCODE_g, KEYCODE_j, KEYCODE_l,            KEYCODE_SingleQuote,  KEYCODE_Tab},
 {KEYCODE_F7,         KEYCODE_4,         KEYCODE_6, KEYCODE_8, KEYCODE_0, KEYCODE_Up,           KEYCODE_Right,        KEYCODE_2},
 {KEYCODE_F1,         KEYCODE_z,         KEYCODE_c, KEYCODE_b, KEYCODE_m, KEYCODE_Period,       KEYCODE_RightShift,   KEYCODE_Space},
 {KEYCODE_F3,         KEYCODE_s,         KEYCODE_f, KEYCODE_h, KEYCODE_k, KEYCODE_SemiColon,    KEYCODE_RightBracket, KEYCODE_LeftControl},
 {KEYCODE_F5,         KEYCODE_e,         KEYCODE_t, KEYCODE_u, KEYCODE_o, KEYCODE_LeftBracket,  KEYCODE_Equals,       KEYCODE_q},
 {KEYCODE_Insert,     KEYCODE_LeftShift, KEYCODE_x, KEYCODE_v, KEYCODE_n, KEYCODE_Comma,        KEYCODE_Slash,        KEYCODE_Escape},
};

//     0: Del          1: Return       2: £            3: Help
//     4: F1           5: F2           6: F3           7: @
//     8: 3            9: W           10: A           11: 4
//    12: Z           13: S           14: E           15: Shift
//    16: 5           17: R           18: D           19: 6
//    20: C           21: F           22: T           23: X
//    24: 7           25: Y           26: G           27: 8
//    28: B           29: H           30: U           31: V
//    32: 9           33: I           34: J           35: 0
//    36: M           37: K           38: O           39: N
//    40: Down        41: P           42: L           43: Up
//    44: .           45: :           46: -           47: ,
//    48: Left        49: *           50: ;           51: Right
//    52: Esc         53: =           54: +           55: /
//    56: 1           57: Home        58: Ctrl        59: 2
//    60: Space       61: C=          62: Q           63: Stop
static int bmc64_keycode_to_plus4emu(long keycode) {
   switch (keycode) {
      case KEYCODE_Backspace:
         return 0;
      case KEYCODE_Return:
         return 1;
      case KEYCODE_BackSlash:
         return 2;
      case KEYCODE_F7:
         return 3;
      case KEYCODE_F1:
         return 4;
      case KEYCODE_F2:
         return 5;
      case KEYCODE_F3:
         return 6;
      case KEYCODE_Insert:
         return 7;
      case KEYCODE_3:
         return 8;
      case KEYCODE_w:
         return 9;
      case KEYCODE_a:
         return 10;
      case KEYCODE_4:
         return 11;
      case KEYCODE_z:
         return 12;
      case KEYCODE_s:
         return 13;
      case KEYCODE_e:
         return 14;
      case KEYCODE_LeftShift:
      case KEYCODE_RightShift:
         return 15;
      case KEYCODE_5:
         return 16;
      case KEYCODE_r:
         return 17;
      case KEYCODE_d:
         return 18;
      case KEYCODE_6:
         return 19;
      case KEYCODE_c:
         return 20;
      case KEYCODE_f:
         return 21;
      case KEYCODE_t:
         return 22;
      case KEYCODE_x:
         return 23;
      case KEYCODE_7:
         return 24;
      case KEYCODE_y:
         return 25;
      case KEYCODE_g:
         return 26;
      case KEYCODE_8:
         return 27;
      case KEYCODE_b:
         return 28;
      case KEYCODE_h:
         return 29;
      case KEYCODE_u:
         return 30;
      case KEYCODE_v:
         return 31;
      case KEYCODE_9:
         return 32;
      case KEYCODE_i:
         return 33;
      case KEYCODE_j:
         return 34;
      case KEYCODE_0:
         return 35;
      case KEYCODE_m:
         return 36;
      case KEYCODE_k:
         return 37;
      case KEYCODE_o:
         return 38;
      case KEYCODE_n:
         return 39;
      case KEYCODE_Down:
         return 40;
      case KEYCODE_p:
         return 41;
      case KEYCODE_l:
         return 42;
      case KEYCODE_Up:
         return 43;
      case KEYCODE_Period:
         return 44;
      case KEYCODE_SemiColon:
         return 45;
      case KEYCODE_LeftBracket:
         return 46;
      case KEYCODE_Comma:
         return 47;
      case KEYCODE_Left:
         return 48;
      case KEYCODE_Dash:
         return 49;
      case KEYCODE_SingleQuote:
         return 50;
      case KEYCODE_Right:
         return 51;
      case KEYCODE_BackQuote:
         return 52;
      case KEYCODE_RightBracket:
         return 53;
      case KEYCODE_Equals:
         return 54;
      case KEYCODE_Slash:
         return 55;
      case KEYCODE_1:
         return 56;
      case KEYCODE_Home:
         return 57;
      case KEYCODE_Tab:
         return 58;
      case KEYCODE_2:
         return 59;
      case KEYCODE_Space:
         return 60;
      case KEYCODE_LeftControl:
         return 61;
      case KEYCODE_q:
         return 62;
      case KEYCODE_Escape:
         return 63;
      default:
         return -1;
   }
}

static void errorMessage(const char *fmt, ...)
{
  va_list args;
  fprintf(stderr, " *** Plus/4 error: ");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  Plus4VM_Destroy(vm);
  exit(-1);
}

static void vmError(void)
{
  fprintf(stderr, " *** Plus/4 error: %s\n", Plus4VM_GetLastErrorMessage(vm));
  Plus4VM_Destroy(vm);
  exit(-1);
}

static void audioOutputCallback(void *userData,
                                const int16_t *buf, size_t nFrames)
{
  if (!ui_warp)
     circle_sound_write((int16_t*)buf, nFrames);
}

static void videoLineCallback(void *userData,
                              int lineNum, const Plus4VideoLineData *lineData)
{
   lineNum = lineNum / 2;
   if (lineNum >= 0 && lineNum < 288) {
     Plus4VideoDecoder_DecodeLine(videoDecoder, fb_buf + lineNum * fb_pitch, 384, lineData);
   }
}

static void videoFrameCallback(void *userData)
{
#ifndef HOST_BUILD
  circle_frames_ready_fbl(FB_LAYER_VIC,
                          -1 /* no 2nd layer */,
                          !ui_warp /* sync */);

  // Something is waiting for vsync, ack and return.
  if (wait_vsync) {
    wait_vsync = 0;
    return;
  }

  emux_ensure_video();

  // This render will handle any OSDs we have. ODSs don't pause emulation.
  if (ui_enabled) {
    // The only way we can be here and have ui_enabled=1
    // is for an osd to be enabled.
    ui_render_now(-1); // only render top most menu
    circle_frames_ready_fbl(FB_LAYER_UI, -1 /* no 2nd layer */,
       0 /* no sync */);
    ui_check_key();
  }

  if (statusbar_showing || vkbd_showing) {
    overlay_check();
    if (overlay_dirty) {
       circle_frames_ready_fbl(FB_LAYER_STATUS,                                                                -1 /* no 2nd layer */,
                               0 /* no sync */);
       overlay_dirty = 0;
    }
  }

  circle_yield();
  circle_check_gpio();

  int reset_demo = 0;

  circle_lock_acquire();
  while (pending_emu_key.head != pending_emu_key.tail) {
    int i = pending_emu_key.head & 0xf;
    reset_demo = 1;
    if (vkbd_enabled) {
      // Kind of nice to have virtual keyboard's state
      // stay in sync with changes happening from USB
      // key events.
      vkbd_sync_event(pending_emu_key.key[i], pending_emu_key.pressed[i]);
    }
    int p4code = bmc64_keycode_to_plus4emu(
       pending_emu_key.key[i]);
    if (p4code >= 0) {
       Plus4VM_KeyboardEvent(vm, p4code, pending_emu_key.pressed[i]);
    }
    pending_emu_key.head++;
  }

  // Joystick event dequeue
  while (pending_emu_joy.head != pending_emu_joy.tail) {
    int i = pending_emu_joy.head & 0x7f;
    reset_demo = 1;
    if (vkbd_enabled) {
      int value = pending_emu_joy.value[i];
      int devd = pending_emu_joy.device[i];
      switch (pending_emu_joy.type[i]) {
      case PENDING_EMU_JOY_TYPE_ABSOLUTE:
        if (!vkbd_press[devd]) {
           if (value & 0x1 && !vkbd_up[devd]) {
             vkbd_up[devd] = 1;
             vkbd_nav_up();
           } else if (!(value & 0x1) && vkbd_up[devd]) {
             vkbd_up[devd] = 0;
           }
           if (value & 0x2 && !vkbd_down[devd]) {
             vkbd_down[devd] = 1;
             vkbd_nav_down();
           } else if (!(value & 0x2) && vkbd_down[devd]) {
             vkbd_down[devd] = 0;
           }
           if (value & 0x4 && !vkbd_left[devd]) {
             vkbd_left[devd] = 1;
             vkbd_nav_left();
           } else if (!(value & 0x4) && vkbd_left[devd]) {
             vkbd_left[devd] = 0;
           }
           if (value & 0x8 && !vkbd_right[devd]) {
             vkbd_right[devd] = 1;
             vkbd_nav_right();
           } else if (!(value & 0x8) && vkbd_right[devd]) {
             vkbd_right[devd] = 0;
           }
        }
        if (value & 0x10 && !vkbd_press[devd]) vkbd_nav_press(1, devd);
        else if (!(value & 0x10) && vkbd_press[devd]) vkbd_nav_press(0, devd);
        break;
      }
    } else {
      int port = pending_emu_joy.port[i]-1;
      int oldv = joy_latch_value[port];
      switch (pending_emu_joy.type[i]) {
      case PENDING_EMU_JOY_TYPE_ABSOLUTE:
        // If new bit is 0 and old bit is 1, it is an up event
        // If new bit is 1 and old bit is 0, it is a down event
        joy_latch_value[port] = pending_emu_joy.value[i];
        break;
      case PENDING_EMU_JOY_TYPE_AND:
        // If new bit is 0 and old bit is 1, it is an up event
        joy_latch_value[port] &= pending_emu_joy.value[i];
        break;
      case PENDING_EMU_JOY_TYPE_OR:
        // If new bit is 1 and old bit is 0, it is a down event
        joy_latch_value[port] |= pending_emu_joy.value[i];
        break;
      default:
        break;
      }

      //    72: Joy2 Up     73: Joy2 Down   74: Joy2 Left   75: Joy2 Right
      //    79: Joy2 Fire
      //    80: Joy1 Up     81: Joy1 Down   82: Joy1 Left   83: Joy1 Right
      //    86: Joy1 Fire

      int newv = joy_latch_value[port];
      if (!(newv & 0x01) && (oldv & 0x01)) {
        Plus4VM_KeyboardEvent(vm, 72 + 8*port, 0);
      } else if ((newv & 0x01) && !(oldv & 0x01)) {
        Plus4VM_KeyboardEvent(vm, 72 + 8*port, 1);
      }
      if (!(newv & 0x02) && (oldv & 0x02)) {
        Plus4VM_KeyboardEvent(vm, 73 + 8*port, 0);
      } else if ((newv & 0x02) && !(oldv & 0x02)) {
        Plus4VM_KeyboardEvent(vm, 73 + 8*port, 1);
      }
      if (!(newv & 0x04) && (oldv & 0x04)) {
        Plus4VM_KeyboardEvent(vm, 74 + 8*port, 0);
      } else if ((newv & 0x04) && !(oldv & 0x04)) {
        Plus4VM_KeyboardEvent(vm, 74 + 8*port, 1);
      }
      if (!(newv & 0x08) && (oldv & 0x08)) {
        Plus4VM_KeyboardEvent(vm, 75 + 8*port, 0);
      } else if ((newv & 0x08) && !(oldv & 0x08)) {
        Plus4VM_KeyboardEvent(vm, 75 + 8*port, 1);
      }
      if (!(newv & 0x10) && (oldv & 0x10)) {
        Plus4VM_KeyboardEvent(vm, 79 + 7*port, 0);
      } else if ((newv & 0x10) && !(oldv & 0x10)) {
        Plus4VM_KeyboardEvent(vm, 79 + 7*port, 1);
      }
    }
    pending_emu_joy.head++;
  }

  if (ui_trap) {
      ui_trap = 0;
      circle_lock_release();
      emu_pause_trap(0, NULL);
      circle_lock_acquire();
  }

  circle_lock_release();

  ui_handle_toggle_or_quick_func();

  if (reset_demo) {
    demo_reset_timeout();
  }

  if (raspi_demo_mode) {
    demo_check();
  }

  if (is_tape_motor) {
     // Plus4Emu doesn't have a rewind/fastforward state so we
     // fake it here.
     if (is_tape_seeking) {
        is_tape_seeking_tick--;
        if (is_tape_seeking_tick == 0) {
          double pos = Plus4VM_GetTapePosition(vm);
          double newpos = pos + 1 * is_tape_seeking_dir;
          double len = Plus4VM_GetTapeLength(vm);
          if (newpos < 0)
             newpos = 0;
          else if (newpos > len)
             newpos = len;

          if (Plus4VM_TapeSeek(vm, newpos) != PLUS4EMU_SUCCESS) {
             is_tape_seeking = 0;
             is_tape_motor = 0;
          }

          int showing = (int)pos - (int)tape_counter_offset;
          if (showing < 0) showing += 1000;
          emux_display_tape_counter(showing);
          is_tape_seeking_tick = 5;
        }
     }
     is_tape_motor_tick--;
     if (is_tape_motor_tick == 0) {
       double pos = Plus4VM_GetTapePosition(vm);
       int showing = (int)pos - (int)tape_counter_offset;
       if (showing < 0) showing += 1000;
       emux_display_tape_counter(showing);
       is_tape_motor_tick = 50;
     }
  }
#endif
}

// This is made to look like VICE's main entry point so our
// Plus4Emu version of EmulatorCore can look more or less the same
// as the Vice version.
int main_program(int argc, char **argv)
{
  int     quitFlag = 0;
  int     timeAdvance;

  (void) argc;
  (void) argv;

  printf ("Init\n");

#ifndef HOST_BUILD
  // BMC64 Video Init
  if (circle_alloc_fbl(FB_LAYER_VIC, 1 /* RGB565 */, &fb_buf,
                              384, 288, &fb_pitch)) {
    printf ("Failed to create video buf.\n");
    assert(0);
  }
  circle_clear_fbl(FB_LAYER_VIC);
  circle_show_fbl(FB_LAYER_VIC);
#else
  fb_buf = (uint8_t*) malloc(384*288*2);
  fb_pitch = 384;
#endif

  vm = Plus4VM_Create();
  if (!vm)
    errorMessage("could not create Plus/4 emulator object");

  Plus4VM_SetAudioOutputCallback(vm, &audioOutputCallback, NULL);
  if (Plus4VM_SetAudioOutputQuality(vm, 1) != PLUS4EMU_SUCCESS)
    vmError();

  int audioSampleRate;
  int fragsize;
  int fragnr;
  int channels;

  circle_sound_init(NULL, &audioSampleRate, &fragsize, &fragnr, &channels);
  if (Plus4VM_SetAudioSampleRate(vm, audioSampleRate) != PLUS4EMU_SUCCESS)
    vmError();

  if (Plus4VM_SetWorkingDirectory(vm, ".") != PLUS4EMU_SUCCESS)
    vmError();
  /* enable read-write IEC level drive emulation for unit 8 */
  Plus4VM_SetIECDriveReadOnlyMode(vm, 0);

  emux_detach_disk(0);

  videoDecoder =
      Plus4VideoDecoder_Create(&videoLineCallback, &videoFrameCallback, NULL);
  if (!videoDecoder)
    errorMessage("could not create video decoder object");
  //Plus4VideoDecoder_UpdatePalette(videoDecoder, 0, 16, 8, 0); // not using rgb
  Plus4VM_SetVideoOutputCallback(vm, &Plus4VideoDecoder_VideoCallback,
                                 (void *) videoDecoder);

  vic_enabled = 1; // really TED

  // This loads settings vars
  ui_init_menu();

  canvas_state[vic_canvas_index].gfx_w = 40*8;
  canvas_state[vic_canvas_index].gfx_h = 25*8;

  int timing = circle_get_machine_timing();
  if (timing == MACHINE_TIMING_NTSC_HDMI ||
      timing == MACHINE_TIMING_NTSC_COMPOSITE ||
      timing == MACHINE_TIMING_NTSC_CUSTOM) {
    canvas_state[vic_canvas_index].max_border_w = 32;
    canvas_state[vic_canvas_index].max_border_h = 16;
    timeAdvance = 1666;
    Plus4VM_SetVideoClockFrequency(vm, 14318180);
    strcpy(rom_kernal,"/PLUS4EMU/p4_ntsc.rom");
    Plus4VideoDecoder_SetNTSCMode(videoDecoder, 1);
  } else {
    canvas_state[vic_canvas_index].max_border_w = 32;
    canvas_state[vic_canvas_index].max_border_h = 40;
    timeAdvance = 2000;
    Plus4VM_SetVideoClockFrequency(vm, 17734475);
    strcpy(rom_kernal,"/PLUS4EMU/p4kernal.rom");
    Plus4VideoDecoder_SetNTSCMode(videoDecoder, 0);
  }
  strcpy(rom_basic,"/PLUS4EMU/p4_basic.rom");
  strcpy(rom_1541,"/PLUS4EMU/dos1541.rom");
  strcpy(rom_1551,"/PLUS4EMU/dos1551.rom");
  strcpy(rom_1581,"/PLUS4EMU/dos1581.rom");

  // Global settings vars have been restored by our load settings hook.
  // Use them to configure the VM.
  if (apply_settings()) {
     return -1;
  }

  set_video_font();

  printf ("Enter emulation loop\n");
  Plus4VM_Reset(vm, 1);

  do {
    if (Plus4VM_Run(vm, timeAdvance) != PLUS4EMU_SUCCESS)
      vmError();
  } while (!quitFlag);

  Plus4VM_Destroy(vm);
  Plus4VideoDecoder_Destroy(videoDecoder);
  return 0;
}

// Begin emu_api impl.

void emu_machine_init() {
  emux_machine_class = BMC64_MACHINE_CLASS_PLUS4EMU;
}


#ifdef HOST_BUILD

int circle_sound_init(const char *param, int *speed, int *fragsize,
                        int *fragnr, int *channels) {
   *speed = 48000;
   *fragsize = 2048;
   *fragnr = 16;
   *channels = 1;
}

int circle_sound_write(int16_t *pbuf, size_t nr) {
}

int main(int argc, char *argv[]) {
  main_program(argc, argv);
}

#endif


void emux_trap_main_loop_ui(void) {
  circle_lock_acquire();
  ui_trap = 1;
  circle_lock_release();
}

void emux_trap_main_loop(void (*trap_func)(uint16_t, void *data), void* data) {
}

void emux_kbd_set_latch_keyarr(int row, int col, int pressed) {
  long keycode = rowColToKeycode[col][row];
  int p4code = bmc64_keycode_to_plus4emu(keycode);
  if (p4code >= 0) {
    Plus4VM_KeyboardEvent(vm, p4code, pressed);
  }
}

int emux_attach_disk_image(int unit, char *filename) {
  if (Plus4VM_SetDiskImageFile(vm, unit-8, filename, 0) != PLUS4EMU_SUCCESS) {
    return 1;
  }
  return 0;
}

void emux_detach_disk(int unit) {
  Plus4VM_SetDiskImageFile(vm, unit-8, "", 1);
}

int emux_attach_tape_image(char *filename) {
  is_tape_seeking = 0;
  is_tape_motor = 0;
  emux_display_tape_counter(0);
  emux_display_tape_motor_status(EMUX_TAPE_STOP);
  if (Plus4VM_SetTapeFileName(vm, filename) != PLUS4EMU_SUCCESS) {
    return 1;
  }
  return 0;
}

void emux_detach_tape(void) {
  is_tape_seeking = 0;
  is_tape_motor = 0;
  emux_display_tape_counter(0);
  emux_display_tape_motor_status(EMUX_TAPE_STOP);
  Plus4VM_SetTapeFileName(vm, "");
}

int emux_attach_cart(int menu_id, char *filename) {
  int bank;
  int offset;
  struct menu_item* item;

  ui_info("Attaching...");

  switch (menu_id) {
     case MENU_PLUS4_CART_C0_LO_FILE:
        bank = 2;
        offset = c0_lo_offset_item->value;
        item = c0_lo_item;
        break;
     case MENU_PLUS4_CART_C0_HI_FILE:
        bank = 3;
        offset = c0_hi_offset_item->value;
        item = c0_hi_item;
        break;
     case MENU_PLUS4_CART_C1_LO_FILE:
        bank = 4;
        offset = c1_lo_offset_item->value;
        item = c1_lo_item;
        break;
     case MENU_PLUS4_CART_C1_HI_FILE:
        bank = 5;
        offset = c1_hi_offset_item->value;
        item = c1_hi_item;
        break;
     case MENU_PLUS4_CART_C2_LO_FILE:
        bank = 6;
        offset = c2_lo_offset_item->value;
        item = c2_lo_item;
        break;
     case MENU_PLUS4_CART_C2_HI_FILE:
        bank = 7;
        offset = c2_hi_offset_item->value;
        item = c2_hi_item;
        break;
     default:
        assert(0);
  }

  if (Plus4VM_LoadROM(vm, bank, filename, offset) != PLUS4EMU_SUCCESS) {
     ui_pop_menu();
     ui_error("Failed to attach cart image");
     return 1;
  } else {
     ui_pop_all_and_toggle();
     Plus4VM_Reset(vm, 1);
  }

  // Update attached cart name
  strncpy(item->str_value, filename, MAX_STR_VAL_LEN - 1);
  strncpy(item->displayed_value, filename, MAX_DSP_VAL_LEN - 1);
  return 0;
}

void emux_set_cart_default(void) {
}

void emux_detach_cart(int menu_id) {
  int bank;
  struct menu_item* item;
  switch (menu_id) {
     case MENU_PLUS4_DETACH_CART_C0_LO:
        bank = 2;
        item = c0_lo_item;
        break;
     case MENU_PLUS4_DETACH_CART_C0_HI:
        bank = 3;
        item = c0_hi_item;
        break;
     case MENU_PLUS4_DETACH_CART_C1_LO:
        bank = 4;
        item = c1_lo_item;
        break;
     case MENU_PLUS4_DETACH_CART_C1_HI:
        bank = 5;
        item = c1_hi_item;
        break;
     case MENU_PLUS4_DETACH_CART_C2_LO:
        bank = 6;
        item = c2_lo_item;
        break;
     case MENU_PLUS4_DETACH_CART_C2_HI:
        bank = 7;
        item = c2_hi_item;
        break;
     default:
        break;
  }

  Plus4VM_LoadROM(vm, bank, "", 0);
  Plus4VM_Reset(vm, 1);
  item->displayed_value[0] = '\0';
  item->str_value[0] = '\0';
}

static void reset_tape_drive() {
  Plus4VM_TapeSeek(vm, 0);
  Plus4VM_TapeStop(vm);
  is_tape_motor = 0;
  tape_counter_offset = 0;
  emux_display_tape_counter(0);
}

void emux_reset(int isSoft) {
  Plus4VM_Reset(vm, !isSoft);
  if (reset_tape_with_cpu) {
     emux_display_tape_control_status(EMUX_TAPE_STOP);
     reset_tape_drive();
     emux_display_tape_motor_status(is_tape_motor);
  }
}

int emux_save_state(char *filename) {
  if (Plus4VM_SaveState(vm, filename) != PLUS4EMU_SUCCESS) {
    return 1;
  }
  return 0;
}

int emux_load_state(char *filename) {
  if (Plus4VM_LoadState(vm, filename) != PLUS4EMU_SUCCESS) {
    return 1;
  }
  return 0;
}

int emux_tape_control(int cmd) {
    emux_display_tape_control_status(cmd);
    switch (cmd) {
    case EMUX_TAPE_PLAY:
      Plus4VM_TapePlay(vm);
      is_tape_seeking = 0;
      is_tape_motor = 1;
      is_tape_motor_tick = 50;
      break;
    case EMUX_TAPE_STOP:
      Plus4VM_TapeStop(vm);
      is_tape_seeking = 0;
      is_tape_motor = 0;
      break;
    case EMUX_TAPE_REWIND:
      is_tape_seeking = 1;
      is_tape_seeking_dir = -1;
      is_tape_seeking_tick = 5;
      is_tape_motor = 1;
      is_tape_motor_tick = 50;
      break;
    case EMUX_TAPE_FASTFORWARD:
      is_tape_seeking = 1;
      is_tape_seeking_dir = 1;
      is_tape_seeking_tick = 5;
      is_tape_motor = 1;
      is_tape_motor_tick = 50;
      break;
    case EMUX_TAPE_RECORD:
      Plus4VM_TapeRecord(vm);
      is_tape_seeking = 0;
      is_tape_motor = 1;
      is_tape_motor_tick = 50;
      break;
    case EMUX_TAPE_RESET:
      reset_tape_drive();
      break;
    case EMUX_TAPE_ZERO:
      tape_counter_offset = Plus4VM_GetTapePosition(vm);
      emux_display_tape_counter(0);
      break;
    default:
      assert(0);
      break;
  }
  emux_display_tape_motor_status(is_tape_motor);
}

void emux_show_cart_osd_menu(void) {
}

unsigned long emux_calculate_timing(double fps) {
}

int emux_autostart_file(char* filename) {
  if (Plus4VM_LoadProgram(vm, filename) != PLUS4EMU_SUCCESS) {
     return 1;
  }
  return 0;
}

void emux_drive_change_model(int unit) {
}

void emux_add_parallel_cable_option(struct menu_item* parent,
                                    int id, int drive) {
  // Not supported for plus/4
}

void emux_create_disk(struct menu_item* item, fullpath_func fullpath) {
  // Not supported for plus/4
}

void emux_set_joy_port_device(int port_num, int dev_id) {
}

void emux_set_joy_pot_x(int value) {
  // Not supported on plus4emu
}

void emux_set_joy_pot_y(int value) {
  // Not supported on plus4emu
}

void emux_add_tape_options(struct menu_item* parent) {
  tape_feedback_item =
      ui_menu_add_range(MENU_TAPE_FEEDBACK, parent,
          "Tape Audible Feedback Level", 0, 10, 1, tape_feedback);
}

void emux_add_sound_options(struct menu_item* parent) {
  // TODO: Why is 6581 so slow?
  struct menu_item* child = sid_model_item =
      ui_menu_add_multiple_choice(MENU_SID_MODEL, parent, "Sid Model");
  child->num_choices = 1;
  child->value = sid_model;
  strcpy(child->choices[0], "8580");
  child->choice_ints[0] = 0; // 8580 for sidflags

  // Write access at $d400-d41f
  sid_write_access_item =
      ui_menu_add_toggle(MENU_SID_WRITE_D400, parent,
          "Write Access $D400-D41F", sid_write_access);

  // Digiblaster
  sid_digiblaster_item =
      ui_menu_add_toggle(MENU_SID_DIGIBLASTER, parent,
          "Enable Digiblaster", sid_digiblaster);
}

void emux_video_color_setting_changed(int display_num) {
  Plus4VideoDecoder_UpdatePalette(videoDecoder);
  // Plus4Emu doesn't use an indexed palette so we have to allow
  // the decoder to draw a frame after we change a color param.
  wait_vsync = 1;
  do {
    if (Plus4VM_Run(vm, 2000) != PLUS4EMU_SUCCESS)
      vmError();
  } while (wait_vsync);
}

void emux_set_color_brightness(int display_num, int value) {
  // Incoming 0-2000, Outgoing -.5 - .5
  // Default 0
  double v = value;
  v = v - 1000;
  v = v / 2000;
  Plus4VideoDecoder_SetBrightness(videoDecoder,v,v,v,v);
}

void emux_set_color_contrast(int display_num, int value) {
  // Incoming 0-2000, Outgoing .5 - 2.0
  // Default 1
  double v = value / 1333.33d;
  v = v + .5;
  if (v < .5) v = .5;
  if (v > 2.0) v = 2.0;
  Plus4VideoDecoder_SetContrast(videoDecoder,v,v,v,v);
}

void emux_set_color_gamma(int display_num, int value) {
  // Incoming 0-4000, Outgoing .25 - 4.0
  // Default 1
  double v = value / 1066.66d;
  v = v + .25;
  if (v < .25) v = .25;
  if (v > 4.0) v = 4.0;
  Plus4VideoDecoder_SetGamma(videoDecoder,v,v,v,v);
}

void emux_set_color_tint(int display_num, int value) {
  // Incoming 0-2000, Outgoing -180, 180
  // Default 0
  double v = value / 5.555d;
  v = v - 180;
  if (v < -180) v = -180;
  if (v > 180) v = 180;
  Plus4VideoDecoder_SetHueShift(videoDecoder,v);
}

int emux_get_color_brightness(int display_num) {
  double saved_value = 0; // TODO : Restore this
  return saved_value * 2000 + 1000;
}

int emux_get_color_contrast(int display_num) {
  double saved_value = 1; // TODO: Restore this
  return (saved_value - .5) * 1333.33d;
}

int emux_get_color_gamma(int display_num) {
  double saved_value = 1; // TODO: Restore this
  return (saved_value - .25) * 1066.66d;
}

int emux_get_color_tint(int display_num) {
  double saved_value = 0; // TODO: Restore this
  return (saved_value + 180) * 5.555d;
}

void emux_set_video_cache(int value) {
  // Ignore for plus/4
}

void emux_set_hw_scale(int value) {
  // Ignore for plus/4
}

void emux_cartridge_trigger_freeze(void) {
  // Ignore for plus/4
}

struct menu_item* emux_add_palette_options(int menu_id,
                                           struct menu_item* parent) {
  // None for plus/4
}

static void menu_value_changed(struct menu_item *item) {
  // Forward to our emux_ handler
  emux_handle_menu_change(item);
}

void emux_add_machine_options(struct menu_item* parent) {
  // TODO : Memory and cartridge configurations
  // C16-16k
  // C16-64k
  // Plus/4-64k

  ram_size_item =
      ui_menu_add_multiple_choice(MENU_MEMORY, parent, "Memory");
  ram_size_item->num_choices = 3;

  switch (ram_size) {
    case 16:
      ram_size_item->value = 0;
      break;
    case 32:
      ram_size_item->value = 1;
      break;
    case 64:
    default:
      ram_size_item->value = 2;
      break;
  }

  strcpy(ram_size_item->choices[0], "16k");
  strcpy(ram_size_item->choices[1], "32k");
  strcpy(ram_size_item->choices[2], "64k");
  ram_size_item->choice_ints[0] = 16;
  ram_size_item->choice_ints[1] = 32;
  ram_size_item->choice_ints[2] = 64;
  ram_size_item->on_value_changed = menu_value_changed;
}

struct menu_item* emux_add_cartridge_options(struct menu_item* root) {
  struct menu_item* parent = ui_menu_add_folder(root, "Cartridge");

  ui_menu_add_divider(parent);
  c0_lo_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
     "C0 LO:",0,rom_c0_lo,"");
  c0_lo_item->prefer_str = 1;
  strncpy(c0_lo_item->displayed_value, rom_c0_lo, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C0_LO, parent, "Attach...");
  c0_lo_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C0_LO_OFFSET,
     parent, "Offset", 0, 16384, 16384, rom_c0_lo_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C0_LO, parent, "Detach");
  ui_menu_add_divider(parent);

  c0_hi_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
      "C0 HI:",0,rom_c0_hi,"");
  c0_hi_item->prefer_str = 1;
  strncpy(c0_hi_item->displayed_value, rom_c0_hi, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C0_HI, parent, "Attach...");
  c0_hi_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C0_HI_OFFSET,
     parent, "Offset", 0, 16384, 16384, rom_c0_hi_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C0_HI, parent, "Detach");
  ui_menu_add_divider(parent);

  c1_lo_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
      "C1 LO:",0,rom_c1_lo,"");
  c1_lo_item->prefer_str = 1;
  strncpy(c1_lo_item->displayed_value, rom_c1_lo, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C1_LO, parent, "Attach...");
  c1_lo_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C1_LO_OFFSET,
      parent, "Offset", 0, 16384, 16384, rom_c1_lo_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C1_LO, parent, "Detach");
  ui_menu_add_divider(parent);

  c1_hi_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
      "C1 HI:",0,rom_c1_hi,"");
  c1_hi_item->prefer_str = 1;
  strncpy(c1_hi_item->displayed_value, rom_c1_hi, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C1_HI, parent, "Attach...");
  c1_hi_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C1_HI_OFFSET,
      parent, "Offset", 0, 16384, 16384, rom_c1_hi_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C1_HI, parent, "Detach");
  ui_menu_add_divider(parent);

  c2_lo_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
      "C2 LO:",0,rom_c2_lo,"");
  c2_lo_item->prefer_str = 1;
  strncpy(c2_lo_item->displayed_value, rom_c2_lo, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C2_LO, parent, "Attach...");
  c2_lo_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C2_LO_OFFSET,
      parent, "Offset", 0, 16384, 16384, rom_c2_lo_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C2_LO, parent, "Detach");
  ui_menu_add_divider(parent);

  c2_hi_item = ui_menu_add_button_with_value(MENU_TEXT, parent,
      "C2 HI:",0,rom_c2_hi,"");
  c2_hi_item->prefer_str = 1;
  strncpy(c2_hi_item->displayed_value, rom_c2_hi, MAX_DSP_VAL_LEN - 1);
  ui_menu_add_button(MENU_PLUS4_ATTACH_CART_C2_HI, parent, "Attach...");
  c2_hi_offset_item = ui_menu_add_range(MENU_PLUS4_ATTACH_CART_C2_HI_OFFSET,
      parent, "Offset", 0, 16384, 16384, rom_c2_hi_off);
  ui_menu_add_button(MENU_PLUS4_DETACH_CART_C2_HI, parent, "Detach");

  return parent;
}

void emux_set_warp(int warp) {
  ui_warp = warp;
}

void emux_change_palette(int display_num, int palette_index) {
  // Never called for Plus4Emu
}

void emux_handle_rom_change(struct menu_item* item, fullpath_func fullpath) {
}

void emux_set_iec_dir(int unit, char* dir) {
}

void emux_set_int(IntSetting setting, int value) {
  switch (setting) {
    case Setting_DatasetteResetWithCPU:
       reset_tape_with_cpu = value;
       break;
    default:
       printf ("Unhandled set int %d\n", setting);
  }
}

void emux_set_int_1(IntSetting setting, int value, int param) {
  // TODO
  printf ("Unhandled set int_1 %d\n", setting);
}

void emux_get_int(IntSetting setting, int* dest) {
   switch (setting) {
      case Setting_WarpMode:
          *dest = ui_warp;
          break;
      case Setting_DatasetteResetWithCPU:
          *dest = reset_tape_with_cpu;
      default:
          printf ("WARNING: Tried to get unsupported setting %d\n",setting);
          break;
   }
}

void emux_get_int_1(IntSetting setting, int* dest, int param) {
  // TODO
  printf ("Unhandled get int_1 %d\n", setting);
}

void emux_get_string_1(StringSetting setting, const char** dest, int param) {
  // TEMP for now to avoid menu crashing
  char* newstr = (char*) malloc(1);
  newstr[0] = '\0';
  *dest = newstr;
}

int emux_save_settings(void) {
  // All our  additional settings are handled by emux_save_additional_settings
  // Nothing to do here.
  return 0;
}

// Handle any menu item we've created for this emulator.
int emux_handle_menu_change(struct menu_item* item) {
  switch (item->id) {
    case MENU_SID_MODEL:
    case MENU_SID_WRITE_D400:
    case MENU_SID_DIGIBLASTER:
      apply_sid_config();
      return 1;
    case MENU_TAPE_FEEDBACK:
      Plus4VM_SetTapeFeedbackLevel(vm, item->value);
      return 1;
    case MENU_MEMORY:
printf ("Setting RAM To %d\n",ram_size_item->choice_ints[ram_size_item->value]);
      Plus4VM_SetRAMConfiguration(vm,
         ram_size_item->choice_ints[ram_size_item->value], 0x99999999UL);
      apply_rom_config();
      return 1;
  }
  return 0;
}

int emux_handle_quick_func(int button_func) {
  return 0;
}

// For Plus4emu, we grab additional settings from the same txt file.
void emux_load_additional_settings() {
  // NOTE: This is called before any menu items have been constructed.

  strcpy(rom_c0_lo, "");
  strcpy(rom_c0_hi, "");
  strcpy(rom_c1_lo, "");
  strcpy(rom_c1_hi, "");
  strcpy(rom_c2_lo, "");
  strcpy(rom_c2_hi, "");

  FILE *fp;
  fp = fopen("/settings-plus4emu.txt", "r");
  if (fp == NULL) {
     return;
  }

  char name_value[256];
  size_t len;
  int value;
  int usb_btn_0_i = 0;
  int usb_btn_1_i = 0;
  while (1) {
    char *line = fgets(name_value, 255, fp);
    if (feof(fp) || line == NULL) break;

    strcpy(name_value, line);

    char *name;
    char *value_str;
    get_key_and_value(name_value, &name, &value_str);
    if (!name || !value_str ||
       strlen(name) == 0 ||
          strlen(value_str) == 0) {
       continue;
    }

    value = atoi(value_str);

    if (strcmp(name,"sid_model") == 0) {
       sid_model = value;
    } else if (strcmp(name,"sid_write_access") == 0) {
       sid_write_access = value;
    } else if (strcmp(name,"sid_digiblaster") == 0) {
       sid_digiblaster = value;
    } else if (strcmp(name,"reset_tape_with_cpu") == 0) {
       reset_tape_with_cpu = value;
    } else if (strcmp(name,"tape_feedback") == 0) {
       tape_feedback = value;
    } else if (strcmp(name,"ram_size") == 0) {
       ram_size = value;
    } else if (strcmp(name,"rom_c0_lo") == 0) {
       strcpy(rom_c0_lo, value_str);
    } else if (strcmp(name,"rom_c0_hi") == 0) {
       strcpy(rom_c0_hi, value_str);
    } else if (strcmp(name,"rom_c1_lo") == 0) {
       strcpy(rom_c1_lo, value_str);
    } else if (strcmp(name,"rom_c1_hi") == 0) {
       strcpy(rom_c1_hi, value_str);
    } else if (strcmp(name,"rom_c2_lo") == 0) {
       strcpy(rom_c2_lo, value_str);
    } else if (strcmp(name,"rom_c2_hi") == 0) {
       strcpy(rom_c2_hi, value_str);
    } else if (strcmp(name,"rom_c0_lo_off") == 0) {
       rom_c0_lo_off = value;
    } else if (strcmp(name,"rom_c0_hi_off") == 0) {
       rom_c0_hi_off = value;
    } else if (strcmp(name,"rom_c1_lo_off") == 0) {
       rom_c1_lo_off = value;
    } else if (strcmp(name,"rom_c1_hi_off") == 0) {
       rom_c1_hi_off = value;
    } else if (strcmp(name,"rom_c2_lo_off") == 0) {
       rom_c2_lo_off = value;
    } else if (strcmp(name,"rom_c2_hi_off") == 0) {
       rom_c2_hi_off = value;
    }
  }

  fclose(fp);
}

void emux_save_additional_settings(FILE *fp) {
  fprintf (fp,"sid_model=%d\n", sid_model_item->value);
  fprintf (fp,"sid_write_access=%d\n", sid_write_access_item->value);
  fprintf (fp,"sid_digiblaster=%d\n", sid_digiblaster_item->value);
  fprintf (fp,"reset_tape_with_cpu=%d\n", reset_tape_with_cpu);
  fprintf (fp,"tape_feedback=%d\n", tape_feedback_item->value);
  fprintf (fp,"ram_size=%d\n", ram_size_item->choice_ints[ram_size_item->value]);
  if (strlen(c0_lo_item->str_value) > 0) {
     fprintf (fp,"rom_c0_lo=%s\n", c0_lo_item->str_value);
  }
  if (strlen(c0_hi_item->str_value) > 0) {
     fprintf (fp,"rom_c0_hi=%s\n", c0_hi_item->str_value);
  }
  if (strlen(c1_lo_item->str_value) > 0) {
     fprintf (fp,"rom_c1_lo=%s\n", c1_lo_item->str_value);
  }
  if (strlen(c1_hi_item->str_value) > 0) {
     fprintf (fp,"rom_c1_hi=%s\n", c1_hi_item->str_value);
  }
  if (strlen(c2_lo_item->str_value) > 0) {
     fprintf (fp,"rom_c2_lo=%s\n", c2_lo_item->str_value);
  }
  if (strlen(c2_hi_item->str_value) > 0) {
     fprintf (fp,"rom_c2_hi=%s\n", c2_hi_item->str_value);
  }
  fprintf (fp,"rom_c0_lo_off=%d\n", c0_lo_offset_item->value);
  fprintf (fp,"rom_c0_hi_off=%d\n", c0_hi_offset_item->value);
  fprintf (fp,"rom_c1_lo_off=%d\n", c1_lo_offset_item->value);
  fprintf (fp,"rom_c1_hi_off=%d\n", c1_hi_offset_item->value);
  fprintf (fp,"rom_c2_lo_off=%d\n", c2_lo_offset_item->value);
  fprintf (fp,"rom_c2_hi_off=%d\n", c2_hi_offset_item->value);
}

void emux_get_default_color_setting(int *brightness, int *contrast,
                                    int *gamma, int *tint) {
    *brightness = 1000;
    *contrast = 666;
    *gamma = 800;
    *tint = 1000;
}

/*
  Need to call these:
  emux_enable_drive_status(st, drive_led_color);
  emux_display_drive_led(drive, pwm1, pwm2);
*/



