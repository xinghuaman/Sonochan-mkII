/* -*- mode: c++; tab-width: 4; c-basic-offset: 4 -*- */
/* This source file is part of the ATMEL AVR32-SoftwareFramework-AT32UC3-1.5.0 Release */

/*This file is prepared for Doxygen automatic documentation generation.*/
/*! \file ******************************************************************
 *
 * \brief Management of the USB device Audio task.
 *
 * This file manages the USB device Audio task.
 *
 * - Compiler:           IAR EWAVR32 and GNU GCC for AVR32
 * - Supported devices:  All AVR32 devices with a USB module can be used.
 * - AppNote:
 *
 * \author               Atmel Corporation: http://www.atmel.com \n
 *                       Support and FAQ: http://support.atmel.no/
 *
 ***************************************************************************/

/* Copyright (c) 2009 Atmel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. The name of Atmel may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * 4. This software may only be redistributed and used in connection with an Atmel
 * AVR product.
 *
 * THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * EXPRESSLY AND SPECIFICALLY DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 *
 * Modified by Alex Lee and SDR-Widget team for the sdr-widget project.
 * See http://code.google.com/p/sdr-widget/
 * Copyright under GNU General Public License v2
 */

//_____  I N C L U D E S ___________________________________________________

#include <stdio.h>
#include "usart.h"     // Shall be included before FreeRTOS header files, since 'inline' is defined to ''; leading to
                       // link errors
#include "conf_usb.h"


#if USB_DEVICE_FEATURE == ENABLED

#include "board.h"

#ifdef FREERTOS_USED
#include "FreeRTOS.h"
#include "task.h"
#endif
#include "gpio.h"
#include "pdca.h"
#include "usb_drv.h"
#include "usb_descriptors.h"
#include "uac1_usb_descriptors.h"
#include "usb_standard_request.h"
#include "usb_specific_request.h"
#include "uac1_usb_specific_request.h"
#include "device_audio_task.h"
#include "uac1_device_audio_task.h"


#include "composite_widget.h"
#include "taskAK5394A.h"

//[Martin]
#include "sync_control.h"

#include "print_funcs.h"
#include <stdio.h>


//_____ M A C R O S ________________________________________________________


//_____ D E F I N I T I O N S ______________________________________________


#define FB_RATE_DELTA (1<<12)
#define FB_RATE_DELTA_NUM 2

//[Martin] For testing we can set high value. In real it should be about 1000
#define MAX_PLL_PPM     50000

//_____ D E C L A R A T I O N S ____________________________________________


//? why are these defined as statics?

static U32  index, spk_index;
//static U16  old_gap = SPK_BUFFER_SIZE;
static U8 audio_buffer_out, spk_buffer_in;  // the ID number of the buffer used for sending out to the USB
static volatile U32 *audio_buffer_ptr;
//static volatile U32 *spk_buffer_ptr;

static U8 ep_audio_in, ep_audio_out, ep_audio_out_fb;

//!
//! @brief This function initializes the hardware/software resources
//! required for device Audio task.
//!
void uac1_device_audio_task_init(U8 ep_in, U8 ep_out, U8 ep_out_fb)
{
  index     =0;
  audio_buffer_out = 0;
  audio_buffer_ptr = audio_buffer_0;
  spk_index = 0;
  spk_buffer_in = 0;
//  spk_buffer_ptr = spk_buffer_0;
  mute = FALSE;
  spk_mute = FALSE;
  volume = 0x5000;
  spk_volume = 0x5000;
  ep_audio_in = ep_in;
  ep_audio_out = ep_out;
  ep_audio_out_fb = ep_out_fb;

  xTaskCreate(uac1_device_audio_task,
        configTSK_USB_DAUDIO_NAME,
        configTSK_USB_DAUDIO_STACK_SIZE,
        NULL,
        configTSK_USB_DAUDIO_PRIORITY,
        NULL);
}

//!
//! @brief Entry point of the device Audio task management
//!

void uac1_device_audio_task(void *pvParameters)
{
  Bool playerStarted = FALSE;
  static U32  time=0;
  static Bool startup=TRUE;
  int i;
  int delta_num = 0;
  U16 num_samples, num_remaining, gap;
  U8 sample_HSB;
  U8 sample_MSB;
  U8 sample_SB;
  U8 sample_LSB;
  U32 sample;
  const U8 EP_AUDIO_IN = ep_audio_in;
  const U8 EP_AUDIO_OUT = ep_audio_out;
  const U8 EP_AUDIO_OUT_FB = ep_audio_out_fb;
  const U8 IN_LEFT = 0;
  const U8 IN_RIGHT = 1;
  const U8 OUT_LEFT = 0;
  const U8 OUT_RIGHT = 1;
  volatile avr32_pdca_channel_t *pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_RX);
  volatile avr32_pdca_channel_t *spk_pdca_channel = pdca_get_handler(PDCA_CHANNEL_SSC_TX);

  //[Martin]
  int i_pll_diff = 0;
  U16 old_num_samples = 0;
  char c[20];


  switch(current_freq.frequency)
  {
  case 48000:
    FB_rate = 48 << 14;
    break;
  case 44100:
    FB_rate = (44 << 14) + (1 << 14)/10;
    break;
  case 32000:
    FB_rate = (32 << 14);
    break;
  case 16000:
    FB_rate = (16 << 14);
    break;
  case 8000:
    FB_rate = (44 << 14);
    break;
  }

  portTickType xLastWakeTime;
  xLastWakeTime = xTaskGetTickCount();

  while (TRUE) {
    vTaskDelayUntil(&xLastWakeTime, UAC1_configTSK_USB_DAUDIO_PERIOD);

    // First, check the device enumeration state
    if (!Is_device_enumerated()) { time=0; startup=TRUE; continue; };

    if( startup ) {


      time+=UAC1_configTSK_USB_DAUDIO_PERIOD;
#define STARTUP_LED_DELAY  10000
      if ( time<= 1*STARTUP_LED_DELAY ) {
        LED_On( LED0 );
        pdca_disable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);
        pdca_disable(PDCA_CHANNEL_SSC_RX);
        //              LED_On( LED1 );
      } else if( time== 2*STARTUP_LED_DELAY ) LED_On( LED1 );
      else if( time== 3*STARTUP_LED_DELAY ) LED_On( LED2 );
      else if( time== 4*STARTUP_LED_DELAY ) LED_On( LED3 );
      else if( time== 5*STARTUP_LED_DELAY ) LED_Off( LED0 );
      else if( time== 6*STARTUP_LED_DELAY ) LED_Off( LED1 );
      else if( time== 7*STARTUP_LED_DELAY ) LED_Off( LED2 );
      else if( time== 8*STARTUP_LED_DELAY ) LED_Off( LED3 );
      else if( time >= 9*STARTUP_LED_DELAY ) {
        startup=FALSE;

        audio_buffer_in = 0;
        audio_buffer_out = 0;
        spk_buffer_in = 0;
        spk_buffer_out = 0;
        index = 0;


        // Wait for the next frame synchronization event
        // to avoid channel inversion.  Start with left channel - FS goes low
        while (!gpio_get_pin_value(AK5394_LRCK));
        while (gpio_get_pin_value(AK5394_LRCK));

        // Enable now the transfer.
        pdca_enable(PDCA_CHANNEL_SSC_RX);
        pdca_enable_interrupt_reload_counter_zero(PDCA_CHANNEL_SSC_RX);

      }
    }
    //else {

      /*[Martin]
       * When num_samples constant -> problem. Set according to actual
       * frequency.
       *
       */
      switch(current_freq.frequency)
      {
      case 48000:
        num_samples = 48;
        break;
      case 44100:
        num_samples = 44;

        break;
      case 32000:
        num_samples = 32;
        break;
      case 16000:
        num_samples = 16;
        break;
      case 8000:
        num_samples = 8;
        break;
      default:
        /* Unknown sampling frequency. This may happen when current_freq was
         * not initialized yet. So set to default (48 kHz). Do not print it
         * until work in debug mode. In Windows this may overflow UART.
         */
        /*
        print(DBG_USART,
            "uac1_device_audio_task() Unknown frequency in current_freq."
            "frequency.\n\n");*/
        num_samples = 48;
      }

      // Test if frequency was changed
      if(old_num_samples != num_samples)
      {
        // Set "old" value
        old_num_samples = num_samples;
        // And reset pll difference
        i_pll_diff = 0;
      }


      if (usb_alternate_setting == 1) {

        if (Is_usb_in_ready(EP_AUDIO_IN)) {  // Endpoint buffer free ?

          Usb_ack_in_ready(EP_AUDIO_IN);  // acknowledge in ready

          // Sync AK data stream with USB data stream
          // AK data is being filled into ~audio_buffer_in, ie if audio_buffer_in is 0
          // buffer 0 is set in the reload register of the pdca
          // So the actual loading is occuring in buffer 1
          // USB data is being taken from audio_buffer_out

          // find out the current status of PDCA transfer
          // gap is how far the audio_buffer_out is from overlapping audio_buffer_in

          num_remaining = pdca_channel->tcr;
          if (audio_buffer_in != audio_buffer_out) {

            // AK and USB using same buffer
            if ( index < (AUDIO_BUFFER_SIZE - num_remaining))
            {
              gap = AUDIO_BUFFER_SIZE - num_remaining - index;
            }
            else
            {
              gap = AUDIO_BUFFER_SIZE - index + AUDIO_BUFFER_SIZE - num_remaining + AUDIO_BUFFER_SIZE;
            }
          } else {
            // usb and pdca working on different buffers
            gap = (AUDIO_BUFFER_SIZE - index) + (AUDIO_BUFFER_SIZE - num_remaining);
          }


          // Sync the USB stream with the AK stream
          // throttle back
          if  (gap < AUDIO_BUFFER_SIZE/2) {
            num_samples--;
            //[Martin] Check number of samples
            if(num_samples < 1) // 1kHz is really low rate
            {
              num_samples = 1;
            }
          } else {
            // speed up
            if  (gap > (AUDIO_BUFFER_SIZE + AUDIO_BUFFER_SIZE/2)) {
              num_samples++;
              //[Martin] Check if number of samples fit to buffer
              if(num_samples >= (48+10))
              {
                num_samples = 48+9;
              }
            }
          }

          Usb_reset_endpoint_fifo_access(EP_AUDIO_IN);
          for( i=0 ; i < num_samples ; i++ ) {
            // Fill endpoint with sample raw
            if (mute==FALSE) {
              if (audio_buffer_out == 0) {
                sample_LSB = audio_buffer_0[index+IN_LEFT];
                sample_SB = audio_buffer_0[index+IN_LEFT] >> 8;
                sample_MSB = audio_buffer_0[index+IN_LEFT] >> 16;
              } else {
                sample_LSB = audio_buffer_1[index+IN_LEFT];
                sample_SB = audio_buffer_1[index+IN_LEFT] >> 8;
                sample_MSB = audio_buffer_1[index+IN_LEFT] >> 16;
              }

              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_LSB);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_SB);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_MSB);

              if (audio_buffer_out == 0) {
                sample_LSB = audio_buffer_0[index+IN_RIGHT];
                sample_SB = audio_buffer_0[index+IN_RIGHT] >> 8;
                sample_MSB = audio_buffer_0[index+IN_RIGHT] >> 16;
              } else {
                sample_LSB = audio_buffer_1[index+IN_RIGHT];
                sample_SB = audio_buffer_1[index+IN_RIGHT] >> 8;
                sample_MSB = audio_buffer_1[index+IN_RIGHT] >> 16;
              }

              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_LSB);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_SB);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, sample_MSB);

              index += 2;
              if (index >= AUDIO_BUFFER_SIZE) {
                index=0;
                audio_buffer_out = 1 - audio_buffer_out;
              }
            } else {
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);
              Usb_write_endpoint_data(EP_AUDIO_IN, 8, 0x00);

            }
          }
          Usb_send_in(EP_AUDIO_IN);    // send the current bank
        }
      } // end alt setting == 1

      if (usb_alternate_setting_out == 1) {

        if ( Is_usb_in_ready(EP_AUDIO_OUT_FB) )
        {
          Usb_ack_in_ready(EP_AUDIO_OUT_FB);  // acknowledge in ready

          Usb_reset_endpoint_fifo_access(EP_AUDIO_OUT_FB);
          // Sync CS4344 spk data stream by calculating gap and provide feedback
          num_remaining = spk_pdca_channel->tcr;
          if (spk_buffer_in != spk_buffer_out) {
            // CS4344 and USB using same buffer
            if ( spk_index < (SPK_BUFFER_SIZE - num_remaining)) gap = SPK_BUFFER_SIZE - num_remaining - spk_index;
            else gap = SPK_BUFFER_SIZE - spk_index + SPK_BUFFER_SIZE - num_remaining + SPK_BUFFER_SIZE;
          } else {
            // usb and pdca working on different buffers
            gap = (SPK_BUFFER_SIZE - spk_index) + (SPK_BUFFER_SIZE - num_remaining);
          }

          if (playerStarted){
            //if ((gap < (SPK_BUFFER_SIZE/2)) && (gap < old_gap)) {
            if ((gap < SPK_BUFFER_SIZE - 10) && (delta_num > -FB_RATE_DELTA_NUM)) {
              LED_Toggle(LED0);
              FB_rate -= FB_RATE_DELTA;
              delta_num--;

              //[Martin] This should be commented. Only for debug
              print_dbg("D-\n");

              //old_gap = gap;
            }
            else
            {
              //if ( (gap > (SPK_BUFFER_SIZE + (SPK_BUFFER_SIZE/2))) && (gap > old_gap)) {
              if ( (gap > SPK_BUFFER_SIZE + 10) && (delta_num < FB_RATE_DELTA_NUM)) {
                LED_Toggle(LED1);
                FB_rate += FB_RATE_DELTA;
                delta_num++;

                //[Martin] This should be commented. Only for debug
                print_dbg("D+\n");


                //old_gap = gap;
              }
            }
            /* [Martin] When Feedback not work (probably not supported)
             * then is last option change PLL frequency by few PPM and pray.
             * When frequency change is too high, then data process will be
             * desynchronized. So change PLL frequency only if buffer have
             * only 33% remaining.
             */
            if((gap < SPK_BUFFER_SIZE -(SPK_BUFFER_SIZE*2/3)) &&
                    (i_pll_diff < (MAX_PLL_PPM/10)) &&
                    (delta_num <= -FB_RATE_DELTA_NUM))
            {
              // We must change frequency - speed up
              if(cs2200_inc_PLL_freq() != 0)
              {
                print_dbg("INC PLL FAILED!\n");
              }
              i_pll_diff++;

              //[Martin] This should be commented. Only for debug
              print_dbg("I ");
              print_dbg_hex(gap);
              print_dbg("\n");
            }
            if((gap > SPK_BUFFER_SIZE +(SPK_BUFFER_SIZE*2/3)) &&
                    (i_pll_diff > -(MAX_PLL_PPM/10)) &&
                    (delta_num >= FB_RATE_DELTA_NUM))
            {
              // We must change frequency - slow down
              if(cs2200_dec_PLL_freq() != 0)
              {
                print_dbg("DEC PLL FAILED!\n");
              }

              i_pll_diff--;

              //[Martin] This should be commented. Only for debug
              print_dbg("D ");
              print_dbg_hex(gap);
              print_dbg("\n");

            }
          }

          if (Is_usb_full_speed_mode()) {      // FB rate is 3 bytes in 10.14 format
            sample_LSB = FB_rate;
            sample_SB = FB_rate >> 8;
            sample_MSB = FB_rate >> 16;
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_LSB);
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_SB);
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_MSB);
          } else {
            // HS mode
            // FB rate is 4 bytes in 12.14 format
            sample_LSB = FB_rate;
            sample_SB = FB_rate >> 8;
            sample_MSB = FB_rate >> 16;
            sample_HSB = FB_rate >> 24;
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_LSB);
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_SB);
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_MSB);
            Usb_write_endpoint_data(EP_AUDIO_OUT_FB, 8, sample_HSB);
            }

          Usb_send_in(EP_AUDIO_OUT_FB);
          }

        if (Is_usb_out_received(EP_AUDIO_OUT)) {
          //spk_usb_heart_beat++;      // indicates EP_AUDIO_OUT receiving data from host

          Usb_reset_endpoint_fifo_access(EP_AUDIO_OUT);
          num_samples = Usb_byte_count(EP_AUDIO_OUT) / 6;       // Bit resolution (6)

          if(!playerStarted)
          {
            playerStarted = TRUE;
            num_remaining = spk_pdca_channel->tcr;
            spk_buffer_in = spk_buffer_out;
            spk_index = SPK_BUFFER_SIZE - num_remaining;
            // BSB added 20120912 after UAC2 time bar pull noise analysis
//            if (spk_index & (U32)1)
//              print_dbg_char_char('s'); // BSB debug 20120912
            spk_index = spk_index & ~((U32)1); // Clear LSB in order to start with L sample
            delta_num = 0;
          }

          for (i = 0; i < num_samples; i++) {
            if (spk_mute) {
              sample_LSB = 0;
              sample_SB = 0;
              sample_MSB = 0;
            } else {
              sample_LSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
              sample_SB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
              sample_MSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
            }

            sample = (((U32) sample_MSB) << 16) + (((U32)sample_SB) << 8) + sample_LSB;
            if (spk_buffer_in == 0) spk_buffer_0[spk_index+OUT_LEFT] = sample;
            else spk_buffer_1[spk_index+OUT_LEFT] = sample;

            if (spk_mute) {
              sample_LSB = 0;
              sample_SB = 0;
              sample_MSB = 0;
            } else {
              sample_LSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
              sample_SB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
              sample_MSB = Usb_read_endpoint_data(EP_AUDIO_OUT, 8);
            };

            sample = (((U32) sample_MSB) << 16) + (((U32)sample_SB) << 8) + sample_LSB;
            if (spk_buffer_in == 0) spk_buffer_0[spk_index+OUT_RIGHT] = sample;
            else spk_buffer_1[spk_index+OUT_RIGHT] = sample;

            spk_index += 2;
            if (spk_index >= SPK_BUFFER_SIZE){
              spk_index = 0;
              spk_buffer_in = 1 - spk_buffer_in;
//              spk_buffer_ptr = spk_buffer_in ? spk_buffer_0 : spk_buffer_1;
            }
          }
          Usb_ack_out_received_free(EP_AUDIO_OUT);
        }  // end usb_out_received
      } // end usb_alternate_setting_out == 1
      else
        playerStarted = FALSE;
    //}  // end startup else
  } // end while vTask

}



#endif  // USB_DEVICE_FEATURE == ENABLED
