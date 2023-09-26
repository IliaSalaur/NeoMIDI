#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "USBMidi/USBMidi.hpp"
#include "utils.hpp"
#include "sdkconfig.h"

#include "led_strip.h"


#define NUM_PIXELS 116

TaskHandle_t usbTask = nullptr;

extern "C" void led_task(void* pvParams)
{
  // led_strip_t strip{
  //   LED_STRIP_WS2812,
  //   false,
  //   100,
  //   NUM_PIXELS,
  //   GPIO_NUM_8,
  //   RMT_CHANNEL_1,
  //   nullptr
  // };

  led_strip_handle_t led_strip;

  led_strip_config_t strip_config = {
    8,
    NUM_PIXELS,
    LED_PIXEL_FORMAT_GRB,
    LED_MODEL_WS2812,
    {false}
  };


  led_strip_rmt_config_t rmt_config = {
    RMT_CLK_SRC_DEFAULT,
    10 * 1000 * 1000,
    0,
    {false}
  };

  // led_strip_install();

  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

  // ESP_ERROR_CHECK(led_strip_init(&strip));

  // led_strip_fill(&strip, 0, NUM_PIXELS, rgb_t{0, 0, 0});
  // led_strip_flush(&strip);

  led_strip_clear(led_strip);

  bool sustainPressed = false;
  uint8_t notes[NUM_PIXELS]{0};
  rgb_t pixels[NUM_PIXELS]{};


  int lastWheelColor = 1;
  uint8_t flushTicker = 0;

  while(1)
  {
    MIDI::midi_event_t midi_event/* = {MIDI::StatusEnum::NOTE_ON, 0, {random8_between(40, 80), random8_to(127)}}*/;
    USBMidi::getEventFromQueue(&midi_event, pdMS_TO_TICKS(10));

    switch (midi_event.statusEnum)
    {
    case MIDI::StatusEnum::NOTE_ON :
      {
        uint8_t idx = noteToPix(96 - midi_event.note);        

        lastWheelColor += random8_to(250);
        lastWheelColor -= lastWheelColor < 1530 ? 10 : 1530;

        if(midi_event.velocity > 0 && notes[idx] == 0)
        {
          // ESP_ERROR_CHECK(led_strip_set_pixel(&strip, idx, mWheel(lastWheelColor, 255)));
          rgb_t col = mWheel(lastWheelColor, 255);
          pixels[idx] = col;
          led_strip_set_pixel(led_strip, idx, col.r, col.g, col.b);
        }          
        notes[idx] = midi_event.velocity;
      }        
      break;
    
    case MIDI::StatusEnum::CONTROL_CHANGE :
      if(midi_event.controlChangeEnum == MIDI::ControlChangeEnum::SUSTAIN_PEDAL)
        sustainPressed = midi_event.data;
      break;

    default:
      break;
    }

    for(size_t i = 0; i < NUM_PIXELS; i++)
    {
      if(notes[i] > 0) continue;

      rgb_t col = pixels[i];
      // ESP_ERROR_CHECK(led_strip_get_pixel(&strip, i, &col));     

      uint8_t minus = sustainPressed ? 2 : 6;
      uint8_t def = 0;

      col.r = col.r < minus + def ? def : col.r - minus;
      col.g = col.g < minus + def ? def : col.g - minus;
      col.b = col.b < minus + def ? def : col.b - minus;

      pixels[i] = col;
      led_strip_set_pixel(led_strip, i, col.r, col.g, col.b);
      // ESP_ERROR_CHECK(led_strip_set_pixel(&strip, i, col));
    }

    if(flushTicker++ > 1)
    {
      // led_strip_wait(&strip, pdMS_TO_TICKS(5));
      // ESP_ERROR_CHECK(led_strip_flush(&strip));

      // ESP_LOGI("led_task", "awaiting semphr");
      USBMidi::takeSemphr(portMAX_DELAY);
      // ESP_LOGI("led_task", "semphr taken");
      led_strip_refresh(led_strip);      
      USBMidi::giveSemphr();
      // ESP_LOGI("led_task", "semphr given");

      flushTicker = 0;
    }    
  }
}

extern "C" void app_main(){
  ESP_ERROR_CHECK(USBMidi::install());
  ESP_ERROR_CHECK(USBMidi::registerClient());

  ESP_LOGI("main", "Starting USB MIDI task");
  usbTask = USBMidi::startUSBMidiTask();

  ESP_LOGI("main", "Starting LED task");
  xTaskCreatePinnedToCore(&led_task, "led_task", 4096, nullptr, 4, nullptr, 0);
}