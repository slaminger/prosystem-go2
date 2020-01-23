/*
prosystem - ProSystem port for the ODROID-GO Advance
Copyright (C) 2020  OtherCrashOverride

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <go2/display.h>
#include <go2/audio.h>
#include <go2/input.h>
#include <drm/drm_fourcc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <stdbool.h>

// Prosystem
#include "Bios.h"
#include "Cartridge.h"
#include "Database.h"
#include "Maria.h"
#include "Palette.h"
#include "Pokey.h"
#include "Region.h"
#include "ProSystem.h"
#include "Tia.h"



static go2_display_t* display;
static go2_presenter_t* presenter;
static go2_audio_t* audio;
static go2_input_t* input;
static go2_gamepad_state_t gamepadState;

static volatile bool isRunning = true;


#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 292
uint16_t* framebuffer;
static int videoWidth  = 320;
static int videoHeight = 240;
static uint16_t display_palette16[256] = {0};
static uint8_t keyboard_data[17] = {0};


#define SOUND_FREQUENCY 48000
#define SOUND_CHANNEL_COUNT (2)
static int16_t* sampleBuffer;

static void InitSound()
{
    printf("Sound: SOUND_FREQUENCY=%d\n", SOUND_FREQUENCY);

    audio = go2_audio_create(SOUND_FREQUENCY);
}

static void ProcessAudio(uint8_t* samples, int frameCount)
{
    go2_audio_submit(audio, (const short*)samples, frameCount);
}

static void InitJoysticks()
{
    input = go2_input_create();
}

static void ReadJoysticks()
{
    go2_input_gamepad_read(input, &gamepadState);

    if (gamepadState.buttons.f1)
        isRunning = false;
}

static void display_ResetPalette16(void)
{
    unsigned index;

#if 1
    uint8_t* paldata = palette_data;
#else
    // http://atariage.com/forums/topic/210082-colorswhat-do-you-want/?p=2763592
    FILE* fp = fopen("NTSC_A7800_GCC3REV3.pal", "rb");
    if (!fp) abort();

    uint8_t* paldata = malloc(3 * 256);
    if (!paldata) abort();

    size_t size = fread(paldata, 1, 3 * 256, fp);
    if (size != (3 * 256)) abort();

    fclose(fp);
#endif

    for(index = 0; index < 256; index++)
    {
        uint16_t r = paldata[(index * 3) + 0];
        uint16_t g = paldata[(index * 3) + 1];
        uint16_t b = paldata[(index * 3) + 2];

        //rrrr rggg gggb bbbb
        uint16_t rgb565 = ((r << 8) & 0xf800) | ((g << 3) & 0x07e0) | (b >> 3);
        display_palette16[index] = rgb565;
    }
}

void game_init(const char* filename)
{
    memset(keyboard_data, 0, sizeof(keyboard_data));

    /* Difficulty switches:
    * Left position = (B)eginner, Right position = (A)dvanced
    * Left difficulty switch defaults to left position, "(B)eginner"
    */
    keyboard_data[15] = 1;

    /* Right difficulty switch defaults to right position,
    * "(A)dvanced", which fixes Tower Toppler
    */
    keyboard_data[16] = 0;

    FILE* fp = fopen(filename, "rb");

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    void* data = malloc(size);
    if (!data) abort();

    size_t count = fread(data, 1, size, fp);
    if (count != size) abort();

    fclose(fp); fp = NULL;


    if (!cartridge_Load((const uint8_t*)data, size))
    {
        abort();
    }

    const char* biospath = "";
    if (strlen(biospath) > 0)
    {
        /* BIOS is optional */
        if (bios_Load(biospath))
        {
            printf("%s: BIOS loaded.\n", __func__);
        }
    }

    database_Load(cartridge_digest);
    prosystem_Reset();

    display_ResetPalette16();
}


static void sound_Resample(const uint8_t* source, uint8_t* target, int length)
{
   int measurement = SOUND_FREQUENCY;
   int sourceIndex = 0;
   int targetIndex = 0;

   int max = ((prosystem_frequency * prosystem_scanlines) << 1);

   while(targetIndex < length)
   {
      if(measurement >= max)
      {
         target[targetIndex++] = source[sourceIndex];
         measurement -= max;
      }
      else
      {
         sourceIndex++;
         measurement += SOUND_FREQUENCY;
      }
   }
}

#define MAX_BUFFER_SIZE 8192

void game_step()
{

    // SetInput
    // +----------+--------------+-------------------------------------------------
    // | Offset   | Controller   | Control
    // +----------+--------------+-------------------------------------------------
    // | 00       | Joystick 1   | Right
    // | 01       | Joystick 1   | Left
    // | 02       | Joystick 1   | Down
    // | 03       | Joystick 1   | Up
    // | 04       | Joystick 1   | Button 1
    // | 05       | Joystick 1   | Button 2
    // | 06       | Joystick 2   | Right
    // | 07       | Joystick 2   | Left
    // | 08       | Joystick 2   | Down
    // | 09       | Joystick 2   | Up
    // | 10       | Joystick 2   | Button 1
    // | 11       | Joystick 2   | Button 2
    // | 12       | Console      | Reset
    // | 13       | Console      | Select
    // | 14       | Console      | Pause
    // | 15       | Console      | Left Difficulty
    // | 16       | Console      | Right Difficulty
    // +----------+--------------+-------------------------------------------------
    const float TRIM = 0.35f;

    keyboard_data[0] = gamepadState.dpad.right || (gamepadState.thumb.x > TRIM);
    keyboard_data[1] = gamepadState.dpad.left || (gamepadState.thumb.x < -TRIM);
    keyboard_data[2] = gamepadState.dpad.down || (gamepadState.thumb.y > TRIM);
    keyboard_data[3] = gamepadState.dpad.up || (gamepadState.thumb.y < -TRIM);
    keyboard_data[4] = gamepadState.buttons.a;
    keyboard_data[5] = gamepadState.buttons.b;
    keyboard_data[13] = gamepadState.buttons.f3;
    keyboard_data[14] = gamepadState.buttons.f4;
    keyboard_data[12] = gamepadState.buttons.f6;

    // Emulate
    prosystem_ExecuteFrame(keyboard_data); // wants input


    // Video
    videoWidth  = Rect_GetLength(&maria_visibleArea);
    videoHeight = Rect_GetHeight(&maria_visibleArea);
    const uint8_t* buffer = maria_surface + ((maria_visibleArea.top - maria_displayArea.top) * Rect_GetLength(&maria_visibleArea));
    uint16_t* surface = framebuffer;
    int pitch = 320;

    for(int y = 0; y < videoHeight; y++)
    {
        for(int x = 0; x < videoWidth; ++x)
        {
            surface[x] = display_palette16[buffer[x]];
        }

        surface += pitch;
        buffer  += videoWidth;
    }

    // Audio
    int length = SOUND_FREQUENCY / prosystem_frequency;
    if (!sampleBuffer)
    {
        size_t sampleBufferLength = length * sizeof(uint16_t) * SOUND_CHANNEL_COUNT;
        sampleBuffer = malloc(sampleBufferLength);
        if (!sampleBuffer) abort();

        printf("%s: Allocated sampleBuffer. length=%d, sampleBufferLength=%d\n",
            __func__, length, sampleBufferLength);
    }

    uint8_t sample[MAX_BUFFER_SIZE];
    memset(sample, 0, MAX_BUFFER_SIZE);

    sound_Resample(tia_buffer, sample, length);

    /* Ballblazer, Commando, various homebrew and hacks */
   if(cartridge_pokey)
   {
      uint32_t index;
      uint8_t pokeySample[MAX_BUFFER_SIZE];
      memset(pokeySample, 0, MAX_BUFFER_SIZE);
      sound_Resample(pokey_buffer, pokeySample, length);
      for(index = 0; index < length; index++)
      {
         sample[index] += pokeySample[index];
         sample[index] = sample[index] / 2;
      }
   }

   /* Convert 8u to 16s */
   uint32_t* framePtr = (uint32_t*)sampleBuffer;
   for(int i = 0; i < length; i ++)
   {
      int16_t sample16 = (sample[i] << 8);
      framePtr[i] = (sample16 << 16) | sample16;
    }

    ProcessAudio((uint8_t*)sampleBuffer, length);
}

int main (int argc, char **argv)
{
    display = go2_display_create();
    presenter = go2_presenter_create(display, DRM_FORMAT_RGB565, 0xff080808);

    go2_surface_t* fbsurface = go2_surface_create(display, VIDEO_WIDTH, VIDEO_HEIGHT, DRM_FORMAT_RGB565);
    framebuffer = (uint16_t*)go2_surface_map(fbsurface); //malloc(VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
    if (!framebuffer) abort();


    // Print help if no game specified
    if(argc < 2)
    {
        printf("USAGE: %s romfile\n", argv[0]);
        return 1;
    }


    InitSound();
    InitJoysticks();


    const char* romfile = argv[1];
    game_init(romfile);


    int totalFrames = 0;
    double totalElapsed = 0.0;

    //Stopwatch_Reset();
    //Stopwatch_Start();


    while(isRunning)
    {
        ReadJoysticks();

        game_step();

        go2_presenter_post(presenter,
                           fbsurface,
                           0, 0, videoWidth, videoHeight,
                           0, ((480 - 426) / 2), 320, 426,
                           GO2_ROTATION_DEGREES_270);

#if 0
        ++totalFrames;


        // Measure FPS
        totalElapsed += Stopwatch_Elapsed();

        if (totalElapsed >= 1.0)
        {
            int fps = (int)(totalFrames / totalElapsed);
            fprintf(stderr, "FPS: %i\n", fps);

            totalFrames = 0;
            totalElapsed = 0;
        }

        Stopwatch_Reset();
#endif
    }

    return 0;
}
