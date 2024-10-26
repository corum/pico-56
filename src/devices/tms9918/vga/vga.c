/*
 * Project: pico-56 - vga
 *
 * Copyright (c) 2023 Troy Schrapel
 *
 * This code is licensed under the MIT license
 *
 * https://github.com/visrealm/pico-56
 *
 */

#include "vga.h"
#include "vga.pio.h"
#include "pio_utils.h"

#include "pico/divider.h"
#include "pico/multicore.h"

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SYNC_PINS_START 0        // first sync pin gpio number
#define SYNC_PINS_COUNT 2        // number of sync pins (h and v)

#define RGB_PINS_START  2        // first rgb pin gpio number
#define RGB_PINS_COUNT 12        // number of rgb pins

#define VGA_PIO         pio0_hw  // which pio are we using for vga?
#define SYNC_SM         0        // vga sync state machine index
#define RGB_SM          1        // vga rgb state machine index


#define END_OF_SCANLINE_MSG 0x40000000
#define END_OF_FRAME_MSG 0x80000000

 /*
  * sync pio dma data buffers
  */
uint32_t __aligned(4) syncDataActive[4];  // active display area
uint32_t __aligned(4) syncDataPorch[4];   // vertical porch
uint32_t __aligned(4) syncDataSync[4];    // vertical sync

uint16_t* rgbDataBufferEven = NULL;
uint16_t* rgbDataBufferOdd = NULL;

/*
 * file scope
 */
static int syncDmaChan = 0;
static int rgbDmaChan = 0;
static VgaInitParams vgaParams;

uint32_t vgaMinimumPioClockKHz(VgaParams* params)
{
  if (params)
  {
    // we need a minimum of two clock cycles for each pixel
    // if you're pixel doubling, you can go slower by a multiple
    // of your doubling factor

    return params->pixelClockKHz * vga_rgb_LOOP_TICKS / params->hPixelScale;
  }
}


/*
 * build the sync data buffers
 */
static bool buildSyncData()
{
  uint32_t sysClockKHz = clock_get_hz(clk_sys) / 1000;
  uint32_t minClockKHz = vgaMinimumPioClockKHz(&vgaParams.params);
  if (minClockKHz < 50000) minClockKHz *= 2;

  if (sysClockKHz < minClockKHz)
  {
    printf("Error: System clock is %d KHz. Minimum required is %d KHz\n", sysClockKHz, minClockKHz);
    return false;
  }

  if (!rgbDataBufferEven) rgbDataBufferEven = malloc(vgaParams.params.hVirtualPixels * sizeof(uint16_t));
  if (!rgbDataBufferOdd) rgbDataBufferOdd = malloc(vgaParams.params.hVirtualPixels * sizeof(uint16_t));

  vgaParams.params.pioDivider = round(sysClockKHz / (float)minClockKHz);
  vgaParams.params.pioFreqKHz = sysClockKHz / vgaParams.params.pioDivider;

  vgaParams.params.pioClocksPerPixel = vgaParams.params.pioFreqKHz / (float)vgaParams.params.pixelClockKHz;
  vgaParams.params.pioClocksPerScaledPixel = vgaParams.params.pioFreqKHz * vgaParams.params.hPixelScale / (float)vgaParams.params.pixelClockKHz;

  const uint32_t activeTicks = round(vgaParams.params.pioClocksPerPixel * (float)vgaParams.params.hSyncParams.displayPixels) - vga_sync_SETUP_OVERHEAD;
  const uint32_t fPorchTicks = round(vgaParams.params.pioClocksPerPixel * (float)vgaParams.params.hSyncParams.frontPorchPixels) - vga_sync_SETUP_OVERHEAD;
  const uint32_t syncTicks = round(vgaParams.params.pioClocksPerPixel * (float)vgaParams.params.hSyncParams.syncPixels) - vga_sync_SETUP_OVERHEAD;
  const uint32_t bPorchTicks = round(vgaParams.params.pioClocksPerPixel * (float)vgaParams.params.hSyncParams.backPorchPixels) - vga_sync_SETUP_OVERHEAD;

  uint32_t rgbCyclesPerPixel = round(vgaParams.params.pioClocksPerScaledPixel);


  // compute sync bits
  const uint32_t hSyncOff = (vgaParams.params.hSyncParams.syncHigh ? 0 : 1) << vga_sync_WORD_HSYNC_OFFSET;
  const uint32_t hSyncOn = (vgaParams.params.hSyncParams.syncHigh ? 1 : 0) << vga_sync_WORD_HSYNC_OFFSET;
  const uint32_t vSyncOff = (vgaParams.params.vSyncParams.syncHigh ? 0 : 1) << vga_sync_WORD_VSYNC_OFFSET;
  const uint32_t vSyncOn = (vgaParams.params.vSyncParams.syncHigh ? 1 : 0) << vga_sync_WORD_VSYNC_OFFSET;

  // compute exec instructions
  const uint32_t instIrq = pio_encode_irq_set(false, vga_rgb_RGB_IRQ) << vga_sync_WORD_EXEC_OFFSET;
  const uint32_t instNop = pio_encode_nop() << vga_sync_WORD_EXEC_OFFSET;

  // sync data for an active display scanline
  const int SYNC_LINE_ACTIVE = 0;
  const int SYNC_LINE_FPORCH = 1;
  const int SYNC_LINE_HSYNC = 2;
  const int SYNC_LINE_BPORCH = 3;

  syncDataActive[SYNC_LINE_ACTIVE] = instIrq | vSyncOff | hSyncOff | activeTicks;
  syncDataActive[SYNC_LINE_FPORCH] = instNop | vSyncOff | hSyncOff | fPorchTicks;
  syncDataActive[SYNC_LINE_HSYNC] = instNop | vSyncOff | hSyncOn | syncTicks;
  syncDataActive[SYNC_LINE_BPORCH] = instNop | vSyncOff | hSyncOff | bPorchTicks;

  // sync data for a front or back porch scanline
  syncDataPorch[SYNC_LINE_ACTIVE] = instNop | vSyncOff | hSyncOff | activeTicks;
  syncDataPorch[SYNC_LINE_FPORCH] = instNop | vSyncOff | hSyncOff | fPorchTicks;
  syncDataPorch[SYNC_LINE_HSYNC] = instNop | vSyncOff | hSyncOn | syncTicks;
  syncDataPorch[SYNC_LINE_BPORCH] = instNop | vSyncOff | hSyncOff | bPorchTicks;

  // sync data for a vsync scanline
  syncDataSync[SYNC_LINE_ACTIVE] = instNop | vSyncOn | hSyncOff | activeTicks;
  syncDataSync[SYNC_LINE_FPORCH] = instNop | vSyncOn | hSyncOff | fPorchTicks;
  syncDataSync[SYNC_LINE_HSYNC] = instNop | vSyncOn | hSyncOn | syncTicks;
  syncDataSync[SYNC_LINE_BPORCH] = instNop | vSyncOn | hSyncOff | bPorchTicks;

  return true;
}


/*
 * initialise the vga sync pio
 */
static void vgaInitSync()
{
  buildSyncData();

  // initalize sync pins for pio
  for (uint i = 0; i < SYNC_PINS_COUNT; ++i)
  {
    pio_gpio_init(VGA_PIO, SYNC_PINS_START + i);
  }

  // add sync pio program
  uint syncProgOffset = pio_add_program(VGA_PIO, &vga_sync_program);
  pio_sm_set_consecutive_pindirs(VGA_PIO, SYNC_SM, SYNC_PINS_START, SYNC_PINS_COUNT, true);

  // configure sync pio
  pio_sm_config syncConfig = vga_sync_program_get_default_config(syncProgOffset);
  sm_config_set_out_pins(&syncConfig, SYNC_PINS_START, 2);
  sm_config_set_clkdiv(&syncConfig, vgaParams.params.pioDivider);
  sm_config_set_out_shift(&syncConfig, true, true, 32); // R shift, autopull @ 32 bits
  sm_config_set_fifo_join(&syncConfig, PIO_FIFO_JOIN_TX); // Join FIFOs together to get an 8 entry TX FIFO
  pio_sm_init(VGA_PIO, SYNC_SM, syncProgOffset, &syncConfig);

  // initialise sync dma
  syncDmaChan = dma_claim_unused_channel(true);
  dma_channel_config syncDmaChanConfig = dma_channel_get_default_config(syncDmaChan);
  channel_config_set_transfer_data_size(&syncDmaChanConfig, DMA_SIZE_32);           // transfer 32 bits at a time
  channel_config_set_read_increment(&syncDmaChanConfig, true);                       // increment read
  channel_config_set_write_increment(&syncDmaChanConfig, false);                     // don't increment write 
  channel_config_set_dreq(&syncDmaChanConfig, pio_get_dreq(VGA_PIO, SYNC_SM, true)); // transfer when there's space in fifo

  // setup the dma channel and set it going
  dma_channel_configure(syncDmaChan, &syncDmaChanConfig, &VGA_PIO->txf[SYNC_SM], syncDataSync, 4, false);
  dma_channel_set_irq0_enabled(syncDmaChan, true);
}


/*
 * initialise the vga sync pio
 */
static void vgaInitRgb()
{
  const uint32_t rgbCyclesPerPixel = round(vgaParams.params.pioClocksPerScaledPixel);

  // copy the rgb program and set the appropriate pixel delay
  uint16_t rgbProgramInstr[vga_rgb_program.length];
  memcpy(rgbProgramInstr, vga_rgb_program.instructions, sizeof(rgbProgramInstr));
  rgbProgramInstr[vga_rgb_DELAY_INSTR] |= pio_encode_delay(rgbCyclesPerPixel - vga_rgb_LOOP_TICKS);

  pio_program_t rgbProgram = {
    .instructions = rgbProgramInstr,
    .length = vga_rgb_program.length,
    .origin = vga_rgb_program.origin
  };

  // initalize sync pins for pio
  for (uint i = 0; i < RGB_PINS_COUNT; ++i)
  {
    pio_gpio_init(VGA_PIO, RGB_PINS_START + i);
  }

  // add rgb pio program
  pio_sm_set_consecutive_pindirs(VGA_PIO, RGB_SM, RGB_PINS_START, RGB_PINS_COUNT, true);
  pio_set_y(VGA_PIO, RGB_SM, vgaParams.params.hVirtualPixels - 1);

  uint rgbProgOffset = pio_add_program(VGA_PIO, &rgbProgram);
  pio_sm_config rgbConfig = vga_rgb_program_get_default_config(rgbProgOffset);

  sm_config_set_out_pins(&rgbConfig, RGB_PINS_START, RGB_PINS_COUNT);
  sm_config_set_clkdiv(&rgbConfig, vgaParams.params.pioDivider);

  sm_config_set_fifo_join(&rgbConfig, PIO_FIFO_JOIN_TX);

  sm_config_set_out_shift(&rgbConfig, true, true, 16); // R shift, autopull @ 16 bits
  pio_sm_init(VGA_PIO, RGB_SM, rgbProgOffset, &rgbConfig);

  // initialise rgb dma
  rgbDmaChan = dma_claim_unused_channel(true);
  dma_channel_config rgbDmaChanConfig = dma_channel_get_default_config(rgbDmaChan);
  channel_config_set_transfer_data_size(&rgbDmaChanConfig, DMA_SIZE_16);  // transfer 16 bits at a time
  channel_config_set_read_increment(&rgbDmaChanConfig, true);             // increment read
  channel_config_set_write_increment(&rgbDmaChanConfig, false);           // don;t increment write
  channel_config_set_dreq(&rgbDmaChanConfig, pio_get_dreq(VGA_PIO, RGB_SM, true));

  // setup the dma channel and set it going
  dma_channel_configure(rgbDmaChan, &rgbDmaChanConfig, &VGA_PIO->txf[RGB_SM], rgbDataBufferEven, vgaParams.params.hVirtualPixels, false);
  dma_channel_set_irq0_enabled(rgbDmaChan, true);
}

/*
 * dma interrupt handler
 */
static void __time_critical_func(dmaIrqHandler)(void)
{
  static int currentTimingLine = -1;
  static int currentDisplayLine = -1;

  if (dma_hw->ints0 & (1u << syncDmaChan))
  {
    dma_hw->ints0 = 1u << syncDmaChan;

    if (++currentTimingLine >= vgaParams.params.vSyncParams.totalPixels)
    {
      currentTimingLine = 0;
      currentDisplayLine = 0;
    }

    if (currentTimingLine < vgaParams.params.vSyncParams.syncPixels)
    {
      dma_channel_set_read_addr(syncDmaChan, syncDataSync, true);
    }
    else if (currentTimingLine < (vgaParams.params.vSyncParams.syncPixels + vgaParams.params.vSyncParams.backPorchPixels))
    {
      dma_channel_set_read_addr(syncDmaChan, syncDataPorch, true);
    }
    else if (currentTimingLine < (vgaParams.params.vSyncParams.totalPixels - vgaParams.params.vSyncParams.frontPorchPixels))
    {
      dma_channel_set_read_addr(syncDmaChan, syncDataActive, true);
    }
    else
    {
      dma_channel_set_read_addr(syncDmaChan, syncDataPorch, true);
    }
    multicore_fifo_push_timeout_us(END_OF_SCANLINE_MSG | currentTimingLine, 0);
  }


  if (dma_hw->ints0 & (1u << rgbDmaChan))
  {
    dma_hw->ints0 = 1u << rgbDmaChan;

    divmod_result_t pxLineVal = divmod_u32u32(currentDisplayLine++, vgaParams.params.vPixelScale);
    uint32_t pxLine = to_quotient_u32(pxLineVal);
    uint32_t pxLineRpt = to_remainder_u32(pxLineVal);

    dma_channel_set_read_addr(rgbDmaChan, (pxLine & 1) ? rgbDataBufferOdd : rgbDataBufferEven, true);

    // need a new line every X display lines
    if ((pxLineRpt == 0))
    {
      uint32_t requestLine = pxLine + 1;
      if (requestLine >= vgaParams.params.vVirtualPixels) requestLine -= vgaParams.params.vVirtualPixels;

      multicore_fifo_push_timeout_us(requestLine, 0);

      if (requestLine == vgaParams.params.vVirtualPixels - 1)
      {
        multicore_fifo_push_timeout_us(END_OF_FRAME_MSG, 0);
      }
    }
  }
}

/*
 * initialise the pio dma
 */
static void initDma()
{
  irq_set_exclusive_handler(DMA_IRQ_0, dmaIrqHandler);
  irq_set_enabled(DMA_IRQ_0, true);

  dma_channel_start(syncDmaChan);
  dma_channel_start(rgbDmaChan);
}

/*
 * main vga loop
 */
static void vgaLoop()
{
  uint64_t frameNumber = 0;
  while (1)
  {
    uint32_t message = multicore_fifo_pop_blocking();

    if (message == END_OF_FRAME_MSG)
    {
      if (vgaParams.endOfFrameFn)
      {
        vgaParams.endOfFrameFn(frameNumber);
      }
      ++frameNumber;
    }
    else if ((message & END_OF_SCANLINE_MSG) != 0)
    {
      if (vgaParams.endOfScanlineFn)
      {
        vgaParams.endOfScanlineFn();
      }
    }
    else if (message & 0x01)
    {
      vgaParams.scanlineFn(message & 0xfff, &vgaParams.params, rgbDataBufferOdd);
    }
    else
    {
      vgaParams.scanlineFn(message & 0xfff, &vgaParams.params, rgbDataBufferEven);
    }
  }
}


/*
 * initialise the vga
 */
void vgaInit(VgaInitParams params)
{
  vgaParams = params;

  vgaInitSync();
  vgaInitRgb();

  initDma();

  pio_sm_set_enabled(VGA_PIO, SYNC_SM, true);
  pio_sm_set_enabled(VGA_PIO, RGB_SM, true);

  multicore_launch_core1(vgaLoop);
}

VgaInitParams vgaCurrentParams()
{
  return vgaParams;
}