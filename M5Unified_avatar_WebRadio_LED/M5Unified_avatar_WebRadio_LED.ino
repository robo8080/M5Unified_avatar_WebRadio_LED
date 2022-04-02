/*
  WebRadio Example
  Very simple HTML app to control web streaming
  
  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <M5Unified.h>
#include <Arduino.h>
#include <WiFi.h>

//#define USE_SD_UPDATER
#ifdef USE_SD_UPDATER
#define SDU_APP_NAME "M5Unified_WebRadio_LED"
#include <M5StackUpdater.h>
#endif

#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorAAC.h"
#include "AudioOutputI2S.h"
#include <AudioOutput.h>
#include <EEPROM.h>
#include <M5UnitOLED.h>
#include <M5UnitLCD.h>
#include <FastLED.h>

LGFX_Device* gfx2 = nullptr;

#define USE_AVATAR
#ifdef USE_AVATAR
#include "Avatar.h"
#endif

// How many leds in your strip?
#define NUM_LEDS 10
#if defined(ARDUINO_M5STACK_Core2)
#define DATA_PIN 25
#else
#define DATA_PIN 15
#endif

// Define the array of leds
CRGB leds[NUM_LEDS];
CRGB led_table[NUM_LEDS/2] = {CRGB::Blue,CRGB::Green,CRGB::Yellow,CRGB::Orange,CRGB::Red};

void turn_off_led() {
  // Now turn the LED off, then pause
  for(int i=0;i<NUM_LEDS;i++) leds[i] = CRGB::Black;
  FastLED.show();  
}

void fill_led_buff(CRGB color) {
  // Now turn the LED off, then pause
  for(int i=0;i<NUM_LEDS;i++) leds[i] =  color;
}

void clear_led_buff() {
  // Now turn the LED off, then pause
  for(int i=0;i<NUM_LEDS;i++) leds[i] =  CRGB::Black;
}

void level_led(int level1, int level2) {
  if(level1>NUM_LEDS/2) level1 = NUM_LEDS/2;
  if(level2>NUM_LEDS/2) level2 = NUM_LEDS/2;
  clear_led_buff(); 
  for(int i=0;i<level1;i++){
    leds[NUM_LEDS/2-1-i] = led_table[i];
  }
  for(int i=0;i<level2;i++){
    leds[i+NUM_LEDS/2] = led_table[i];
  }
  FastLED.show();  
}

// Custom web server that doesn't need much RAM
#include "web.h"

// To run, set your ESP8266 build to 160MHz, update the SSID info, and upload.

// Enter your WiFi setup here:
#ifndef STASSID
#define STASSID "************"
#define STAPSK  "************"
#endif

const char* ssid = STASSID;
const char* password = STAPSK;

WiFiServer server(80);

AudioGenerator *decoder = NULL;
AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
int volume = 100;
char title[64];
char url[96];
char status[64];
bool newUrl = false;
bool isAAC = false;
int retryms = 0;

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

class AudioOutputM5Speaker : public AudioOutput
{
  public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0)
    {
      _m5sound = m5sound;
      _virtual_ch = virtual_sound_channel;
    }
    virtual ~AudioOutputM5Speaker(void) {};
    virtual bool begin(void) override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override
    {
      if (_tri_buffer_index < tri_buf_size)
      {
        _tri_buffer[_tri_index][_tri_buffer_index  ] = sample[0];
        _tri_buffer[_tri_index][_tri_buffer_index+1] = sample[1];
        _tri_buffer_index += 2;

        return true;
      }

      flush();
      return false;
    }
    virtual void flush(void) override
    {
      if (_tri_buffer_index)
      {
        /// If there is no room in the play queue, playRAW will return false, so repeat until true is returned.
        _m5sound->playRAW(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
        _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
        _tri_buffer_index = 0;
      }
    }
    virtual bool stop(void) override
    {
      flush();
      _m5sound->stop(_virtual_ch);
      return true;
    }

    const int16_t* getBuffer(void) const { return _tri_buffer[(_tri_index + 2) % 3]; }

  protected:
    m5::Speaker_Class* _m5sound;
    uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 1536;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0;
    size_t _tri_index = 0;
};

#define FFT_SIZE 256
class fft_t
{
  float _wr[FFT_SIZE + 1];
  float _wi[FFT_SIZE + 1];
  float _fr[FFT_SIZE + 1];
  float _fi[FFT_SIZE + 1];
  uint16_t _br[FFT_SIZE + 1];
  size_t _ie;

public:
  fft_t(void)
  {
#ifndef M_PI
#define M_PI 3.141592653
#endif
    _ie = logf( (float)FFT_SIZE ) / log(2.0) + 0.5;
    static constexpr float omega = 2.0f * M_PI / FFT_SIZE;
    static constexpr int s4 = FFT_SIZE / 4;
    static constexpr int s2 = FFT_SIZE / 2;
    for ( int i = 1 ; i < s4 ; ++i)
    {
    float f = cosf(omega * i);
      _wi[s4 + i] = f;
      _wi[s4 - i] = f;
      _wr[     i] = f;
      _wr[s2 - i] = -f;
    }
    _wi[s4] = _wr[0] = 1;

    size_t je = 1;
    _br[0] = 0;
    _br[1] = FFT_SIZE / 2;
    for ( size_t i = 0 ; i < _ie - 1 ; ++i )
    {
      _br[ je << 1 ] = _br[ je ] >> 1;
      je = je << 1;
      for ( size_t j = 1 ; j < je ; ++j )
      {
        _br[je + j] = _br[je] + _br[j];
      }
    }
  }

  void exec(const int16_t* in)
  {
    memset(_fi, 0, sizeof(_fi));
    for ( size_t j = 0 ; j < FFT_SIZE / 2 ; ++j )
    {
      float basej = 0.25 * (1.0-_wr[j]);
      size_t r = FFT_SIZE - j - 1;

      /// perform han window and stereo to mono convert.
      _fr[_br[j]] = basej * (in[j * 2] + in[j * 2 + 1]);
      _fr[_br[r]] = basej * (in[r * 2] + in[r * 2 + 1]);
    }

    size_t s = 1;
    size_t i = 0;
    do
    {
      size_t ke = s;
      s <<= 1;
      size_t je = FFT_SIZE / s;
      size_t j = 0;
      do
      {
        size_t k = 0;
        do
        {
          size_t l = s * j + k;
          size_t m = ke * (2 * j + 1) + k;
          size_t p = je * k;
          float Wxmr = _fr[m] * _wr[p] + _fi[m] * _wi[p];
          float Wxmi = _fi[m] * _wr[p] - _fr[m] * _wi[p];
          _fr[m] = _fr[l] - Wxmr;
          _fi[m] = _fi[l] - Wxmi;
          _fr[l] += Wxmr;
          _fi[l] += Wxmi;
        } while ( ++k < ke) ;
      } while ( ++j < je );
    } while ( ++i < _ie );
  }

  uint32_t get(size_t index)
  {
    return (index < FFT_SIZE / 2) ? (uint32_t)sqrtf(_fr[ index ] * _fr[ index ] + _fi[ index ] * _fi[ index ]) : 0u;
  }
};

static constexpr size_t WAVE_SIZE = 320;
static AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
static fft_t fft;
static bool fft_enabled = false;
static bool wave_enabled = false;
static uint16_t prev_y[(FFT_SIZE / 2)+1];
static uint16_t peak_y[(FFT_SIZE / 2)+1];
static int16_t wave_y[WAVE_SIZE];
static int16_t wave_h[WAVE_SIZE];
static int16_t raw_data[WAVE_SIZE * 2];
static int header_height = 0;
static size_t fileindex = 0;

uint32_t bgcolor(LGFX_Device* gfx, int y)
{
  auto h = gfx->height();
  auto dh = h - header_height;
  int v = ((h - y)<<5) / dh;
  if (dh > 44)
  {
    int v2 = ((h - y - 1)<<5) / dh;
    if ((v >> 2) != (v2 >> 2))
    {
      return 0x666666u;
    }
  }
  return gfx->color888(v + 2, v, v + 6);
}

void gfxSetup(LGFX_Device* gfx)
{
  if (gfx == nullptr) { return; }
  if (gfx->width() < gfx->height())
  {
    gfx->setRotation(gfx->getRotation()^1);
  }
  gfx->setFont(&fonts::lgfxJapanGothic_12);
  gfx->setEpdMode(epd_mode_t::epd_fastest);
  gfx->setCursor(0, 8);
  gfx->println("M5Stack Web Radio");
  gfx->setTextWrap(false);
  gfx->fillRect(0, 6, gfx->width(), 2, TFT_BLACK);

  header_height = 45;
  fft_enabled = !gfx->isEPD();
  if (fft_enabled)
  {
    wave_enabled = (gfx->getBoard() != m5gfx::board_M5UnitLCD);

    for (int y = header_height; y < gfx->height(); ++y)
    {
      gfx->drawFastHLine(0, y, gfx->width(), bgcolor(gfx, y));
    }
  }

  for (int x = 0; x < (FFT_SIZE/2)+1; ++x)
  {
    prev_y[x] = INT16_MAX;
    peak_y[x] = INT16_MAX;
  }
  for (int x = 0; x < WAVE_SIZE; ++x)
  {
    wave_y[x] = gfx->height();
    wave_h[x] = 0;
  }
}

void gfxLoop(LGFX_Device* gfx)
{
  if (gfx == nullptr) { return; }

  if (!gfx->displayBusy())
  { // draw volume bar
    static int px;
    uint8_t v = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
    int x = v * (gfx->width()) >> 8;
    if (px != x)
    {
      gfx->fillRect(x, 6, px - x, 2, px < x ? 0xAAFFAAu : 0u);
      gfx->display();
      px = x;
    }
  }

  if (fft_enabled && !gfx->displayBusy() && M5.Speaker.isPlaying(m5spk_virtual_channel) > 1)
  {
    static int prev_x[2];
    static int peak_x[2];

    auto buf = out.getBuffer();
    if (buf)
    {
      level_led(abs(buf[1])*10/INT16_MAX,abs(buf[0])*10/INT16_MAX);
      memcpy(raw_data, buf, WAVE_SIZE * 2 * sizeof(int16_t)); // stereo data copy
      gfx->startWrite();

      // draw stereo level meter
      for (size_t i = 0; i < 2; ++i)
      {
        int32_t level = 0;
        for (size_t j = i; j < 640; j += 32)
        {
          uint32_t lv = abs(raw_data[j]);
          if (level < lv) { level = lv; }
        }

        int32_t x = (level * gfx->width()) / INT16_MAX;
        int32_t px = prev_x[i];
        if (px != x)
        {
          gfx->fillRect(x, i * 3, px - x, 2, px < x ? 0xFF9900u : 0x330000u);
          prev_x[i] = x;
        }
        px = peak_x[i];
        if (px > x)
        {
          gfx->writeFastVLine(px, i * 3, 2, TFT_BLACK);
          px--;
        }
        else
        {
          px = x;
        }
        if (peak_x[i] != px)
        {
          peak_x[i] = px;
          gfx->writeFastVLine(px, i * 3, 2, TFT_WHITE);
        }
      }
      gfx->display();

      // draw FFT level meter
      fft.exec(raw_data);
      size_t bw = gfx->width() / 60;
      if (bw < 3) { bw = 3; }
      int32_t dsp_height = gfx->height();
      int32_t fft_height = dsp_height - header_height - 1;
      size_t xe = gfx->width() / bw;
      if (xe > (FFT_SIZE/2)) { xe = (FFT_SIZE/2); }
      int32_t wave_next = ((header_height + dsp_height) >> 1) + (((256 - (raw_data[0] + raw_data[1])) * fft_height) >> 17);

      uint32_t bar_color[2] = { 0x000033u, 0x99AAFFu };

      for (size_t bx = 0; bx <= xe; ++bx)
      {
        size_t x = bx * bw;
        if ((x & 7) == 0) { gfx->display(); taskYIELD(); }
        int32_t f = fft.get(bx);
        int32_t y = (f * fft_height) >> 18;
        if (y > fft_height) { y = fft_height; }
        y = dsp_height - y;
        int32_t py = prev_y[bx];
        if (y != py)
        {
          gfx->fillRect(x, y, bw - 1, py - y, bar_color[(y < py)]);
          prev_y[bx] = y;
        }
        py = peak_y[bx] + 1;
        if (py < y)
        {
          gfx->writeFastHLine(x, py - 1, bw - 1, bgcolor(gfx, py - 1));
        }
        else
        {
          py = y - 1;
        }
        if (peak_y[bx] != py)
        {
          peak_y[bx] = py;
          gfx->writeFastHLine(x, py, bw - 1, TFT_WHITE);
        }


        if (wave_enabled)
        {
          for (size_t bi = 0; bi < bw; ++bi)
          {
            size_t i = x + bi;
            if (i >= gfx->width() || i >= WAVE_SIZE) { break; }
            y = wave_y[i];
            int32_t h = wave_h[i];
            bool use_bg = (bi+1 == bw);
            if (h>0)
            { /// erase previous wave.
              gfx->setAddrWindow(i, y, 1, h);
              h += y;
              do
              {
                uint32_t bg = (use_bg || y < peak_y[bx]) ? bgcolor(gfx, y)
                            : (y == peak_y[bx]) ? 0xFFFFFFu
                            : bar_color[(y >= prev_y[bx])];
                gfx->writeColor(bg, 1);
              } while (++y < h);
            }
            size_t i2 = i << 1;
            int32_t y1 = wave_next;
            wave_next = ((header_height + dsp_height) >> 1) + (((256 - (raw_data[i2] + raw_data[i2 + 1])) * fft_height) >> 17);
            int32_t y2 = wave_next;
            if (y1 > y2)
            {
              int32_t tmp = y1;
              y1 = y2;
              y2 = tmp;
            }
            y = y1;
            h = y2 + 1 - y;
            wave_y[i] = y;
            wave_h[i] = h;
            if (h>0)
            { /// draw new wave.
              gfx->setAddrWindow(i, y, 1, h);
              h += y;
              do
              {
                uint32_t bg = (y < prev_y[bx]) ? 0xFFCC33u : 0xFFFFFFu;
                gfx->writeColor(bg, 1);
              } while (++y < h);
            }
          }
        }
      }
      gfx->display();
      gfx->endWrite();
    }
  }
}

// Get the Split String Value Used for Band or Track
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

 void gfxSetup1(LGFX_Device* gfx)
{
  header_height = 46;
  fft_enabled = !gfx->isEPD();
  if (fft_enabled)
  {
    for (int y = header_height; y < gfx->height(); ++y)
    {
      gfx->drawFastHLine(0, y, gfx->width(), bgcolor(gfx, y));
    }
  }
  for (int x = 0; x < (FFT_SIZE/2)+1; ++x)
  {
    prev_y[x] = INT16_MAX;
    peak_y[x] = INT16_MAX;
  }
  for (int x = 0; x < WAVE_SIZE; ++x)
  {
    wave_y[x] = gfx->height();
    wave_h[x] = 0;
  }
}

const int stations = 9;// Change Number here if you add feeds!
int currentStationNumber = 0;
char * stationList[stations][2] = {
  {"Classic FM", "http://media-ice.musicradio.com:80/ClassicFMMP3"},
  {"Asia Dream", "https://igor.torontocast.com:1025/;.-mp3"},
  {"MAXXED Out", "http://149.56.195.94:8015/steam"},
  {"thejazzstream", "http://wbgo.streamguys.net/thejazzstream"},
  {"181-beatles_128k", "http://listen.181fm.com/181-beatles_128k.mp3"},
  {"illstreet-128-mp3", "http://ice1.somafm.com/illstreet-128-mp3"},
  {"bootliquor-128-mp3", "http://ice1.somafm.com/bootliquor-128-mp3"},
  {"SomaFM", "http://ice2.somafm.com/christmas-128-mp3"},
//  {"Charlie FM", "http://24083.live.streamtheworld.com:80/KYCHFM_SC"},
//  {"Orig. Top 40", "http://ais-edge09-live365-dal02.cdnstream.com/a25710"},
//  {"Smooth Jazz", "http://sj32.hnux.com/stream?type=http&nocache=3104"},
//  {"Smooth Lounge", "http://sl32.hnux.com/stream?type=http&nocache=1257"},
//  {"KPop Way Radio", "http://streamer.radio.co/s06b196587/listen"},
  {"Lite Favorites", "http://naxos.cdnstream.com:80/1255_128"}
};

void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
  (void)cbData;
  if (string[0] == 0) { return; }
  if (strcmp(type, "eof") == 0)
  {
    M5.Display.display();
    return;
  }

  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) isUnicode; // Punt this ball for now
  (void) ptr;
  if (strstr_P(type, PSTR("Title"))) { 
    strncpy(title, string, sizeof(title));
    title[sizeof(title)-1] = 0;
  } else {
    // Who knows what to do?  Not me!
  }
  // Note that the type and string may be in PROGMEM, so copy them to RAM for printf
  char s1[32], s2[64];
  strncpy_P(s1, type, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  strncpy_P(s2, string, sizeof(s2));
  s2[sizeof(s2) - 1] = 0;
//printf("s1=%s\r\n",s1);
//printf("s2=%s\r\n",s2);
  String band  = getValue(s2, '-', 0);
  band.trim();
  String track = getValue(s2, '-', 1);
  track.trim();
 
  M5.Display.setCursor(0, 21 ); 
  int y = M5.Display.getCursorY();
//  printf("y=%d\r\n",y);
  if (y+1 >= header_height) { return; }
  M5.Display.fillRect(0, y, M5.Display.width(), 12, M5.Display.getBaseColor());
//  M5.Display.printf("%s: ", type);
//  M5.Display.println(string);
  M5.Display.print(band + " : ");
  M5.Display.println(track);
  M5.Display.setCursor(0, y+12);
  gfxSetup1(&M5.Display);
}

typedef struct {
  char url[96];
  bool isAAC;
  int16_t volume;
  int16_t checksum;
} Settings;

// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<head>
<title>M5Stack Web Radio</title>
<script type="text/javascript">
  function updateTitle() {
    var x = new XMLHttpRequest();
    x.open("GET", "title");
    x.onload = function() { document.getElementById("titlespan").innerHTML=x.responseText; setTimeout(updateTitle, 5000); }
    x.onerror = function() { setTimeout(updateTitle, 5000); }
    x.send();
  }
  setTimeout(updateTitle, 1000);
  function showValue(n) {
    document.getElementById("volspan").innerHTML=n;
    var x = new XMLHttpRequest();
    x.open("GET", "setvol?vol="+n);
    x.send();
  }
  function updateStatus() {var x = new XMLHttpRequest();
    x.open("GET", "status");
    x.onload = function() { document.getElementById("statusspan").innerHTML=x.responseText; setTimeout(updateStatus, 5000); }
    x.onerror = function() { setTimeout(updateStatus, 5000); }
    x.send();
  }
  setTimeout(updateStatus, 2000);
</script>
</head>)KEWL";

static const char BODY[] PROGMEM = R"KEWL(
<body>
M5Stack Web Radio!
<hr>
Currently Playing: <span id="titlespan">%s</span><br>
Volume: <input type="range" name="vol" min="1" max="150" steps="10" value="%d" onchange="showValue(this.value)"/> <span id="volspan">%d</span>%%
<hr>
Status: <span id="statusspan">%s</span>
<hr>
<form action="changeurl" method="GET">
Current URL: %s<br>
Change URL: <input type="text" name="url">
<select name="type"><option value="mp3">MP3</option><option value="aac">AAC</option></select>
<input type="submit" value="Change"></form>
<form action="stop" method="POST"><input type="submit" value="Stop"></form>
</body>)KEWL";

void HandleIndex(WiFiClient *client)
{
  char buff[sizeof(BODY) + sizeof(title) + sizeof(status) + sizeof(url) + 3*2];
  
  Serial.printf_P(PSTR("Sending INDEX...Free mem=%d\n"), ESP.getFreeHeap());
  WebHeaders(client, NULL);
  WebPrintf(client, DOCTYPE);
  client->write_P( PSTR("<html>"), 6 );
  client->write_P( HEAD, strlen_P(HEAD) );
  sprintf_P(buff, BODY, title, volume, volume, status, url);
  client->write(buff, strlen(buff) );
  client->write_P( PSTR("</html>"), 7 );
  Serial.printf_P(PSTR("Sent INDEX...Free mem=%d\n"), ESP.getFreeHeap());
}

void HandleStatus(WiFiClient *client)
{
  WebHeaders(client, NULL);
  client->write(status, strlen(status));
}

void HandleTitle(WiFiClient *client)
{
  WebHeaders(client, NULL);
  client->write(title, strlen(title));
}

void HandleVolume(WiFiClient *client, char *params)
{
  char *namePtr;
  char *valPtr;
  
  while (ParseParam(&params, &namePtr, &valPtr)) {
    ParamInt("vol", volume);
  }
  Serial.printf_P(PSTR("Set volume: %d\n"), volume);
//  out.SetGain(((float)volume)/100.0);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, (int)(((float)volume)/150.0*255.0));
  RedirectToIndex(client);
}

void HandleChangeURL(WiFiClient *client, char *params)
{
  char *namePtr;
  char *valPtr;
  char newURL[sizeof(url)];
  char newType[4];

  newURL[0] = 0;
  newType[0] = 0;
  while (ParseParam(&params, &namePtr, &valPtr)) {
    ParamText("url", newURL);
    ParamText("type", newType);
  }
  if (newURL[0] && newType[0]) {
    newUrl = true;
    strncpy(url, newURL, sizeof(url)-1);
    url[sizeof(url)-1] = 0;
    if (!strcmp_P(newType, PSTR("aac"))) {
      isAAC = true;
    } else {
      isAAC = false;
    }
    strcpy_P(status, PSTR("Changing URL..."));
    Serial.printf_P(PSTR("Changed URL to: %s(%s)\n"), url, newType);
    RedirectToIndex(client);
    M5.Display.setCursor(0, 8 );
    M5.Display.fillRect(0, 8, M5.Display.width(), 12, M5.Display.getBaseColor());
    M5.Display.println(url);
  } else {
    WebError(client, 404, NULL, false);
  }
}

void RedirectToIndex(WiFiClient *client)
{
  WebError(client, 301, PSTR("Location: /\r\n"), true);
}

void StopPlaying()
{
  if (decoder) {
    decoder->stop();
    delete decoder;
    decoder = NULL;
  }
  if (buff) {
    buff->close();
    delete buff;
    buff = NULL;
  }
  if (file) {
    file->close();
    delete file;
    file = NULL;
  }
  strcpy_P(status, PSTR("Stopped"));
  strcpy_P(title, PSTR("Stopped"));
}

void HandleStop(WiFiClient *client)
{
  Serial.printf_P(PSTR("HandleStop()\n"));
  StopPlaying();
  RedirectToIndex(client);
}

void StatusCallback(void *cbData, int code, const char *string)
{
  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) code;
  (void) ptr;
  strncpy_P(status, string, sizeof(status)-1);
  status[sizeof(status)-1] = 0;
}

const int preallocateBufferSize = 16*1024;
const int preallocateCodecSize = 85332; // AAC+SBR codec max mem needed
void *preallocateBuffer = NULL;
void *preallocateCodec = NULL;

void drawLoop(void*)
{
  for (;;) {
  gfxLoop(&M5.Display);
  M5.update();
  if (M5.BtnA.wasClicked())
  {
    char newURL[sizeof(url)];
    newURL[0] = 0;
    M5.Speaker.tone(1000, 100);
    currentStationNumber++;
    if (currentStationNumber >= stations) currentStationNumber = 0;
    M5.Display.setCursor(0, 8 );
    M5.Display.fillRect(0, 8, M5.Display.width(), 12, M5.Display.getBaseColor());
    M5.Display.println(stationList[currentStationNumber][0]);
    newUrl = true;
    strncpy(url, stationList[currentStationNumber][1], sizeof(url)-1);
    url[sizeof(url)-1] = 0;
    isAAC = false;
    SaveSettings();
  }
  else
  if (M5.BtnA.isHolding() || M5.BtnB.isPressed() || M5.BtnC.isPressed())
  {
    size_t v = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
    if (M5.BtnB.isPressed()) { --v; } else { ++v; }
    if (v <= 255 || M5.BtnA.isHolding())
    {
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, v);
      volume = (int)((float)v/255.0*150.0);
    }
  }
  delay(50);
  }
  vTaskDelete(NULL);
}

#ifdef USE_AVATAR
using namespace m5avatar;
Avatar* avatar;

void lipSync(void *args)
{
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
   for (;;)
  {
    level = abs(*out.getBuffer());
    if(level<1500) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
    avatar->setRotation(gazeX * 5);
     delay(50);
  }
}
#endif

void setup()
{
  // First, preallocate all the memory needed for the buffering and codecs, never to be freed
  preallocateBuffer = ps_malloc(preallocateBufferSize);
  preallocateCodec = ps_malloc(preallocateCodecSize);
  if (!preallocateBuffer || !preallocateCodec) {
    Serial.begin(115200);
    Serial.printf_P(PSTR("FATAL ERROR:  Unable to preallocate %d bytes for app\n"), preallocateBufferSize+preallocateCodecSize);
    while (1) delay(1000); // Infinite halt
  }
  auto cfg = M5.config();

  cfg.external_spk = true;    /// use external speaker (SPK HAT / ATOMIC SPK)
//cfg.external_spk_detail.omit_atomic_spk = true; // exclude ATOMIC SPK
//cfg.external_spk_detail.omit_spk_hat    = true; // exclude SPK HAT

  M5.begin(cfg);

#ifdef USE_SD_UPDATER
  checkSDUpdater(
    SD,           // filesystem (default=SD)
    MENU_BIN,     // path to binary (default=/menu.bin, empty string=rollback only)
    3000,         // wait delay, (default=0, will be forced to 2000 upon ESP.restart() )
    TFCARD_CS_PIN // (usually default=4 but your mileage may vary)
  );
#endif

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 48000; // default:48000 (48kHz)  e.g. 50000 , 80000 , 96000 , 100000 , 144000 , 192000
//    spk_cfg.task_pinned_core = APP_CPU_NUM;
    spk_cfg.task_pinned_core = 1;
    spk_cfg.task_priority = configMAX_PRIORITIES -2;
    spk_cfg.dma_buf_count = 16;
    spk_cfg.dma_buf_len = 256;
    M5.Speaker.config(spk_cfg);
  }

  M5.Speaker.begin();

  FastLED.addLeds<SK6812, DATA_PIN, GRB>(leds, NUM_LEDS);  // GRB ordering is typical
  FastLED.setBrightness(32);
  level_led(5, 5);
  FastLED.show();

  delay(1000);
  Serial.printf_P(PSTR("Connecting to WiFi\n"));

  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  
  WiFi.begin(ssid, password);

  // Try forever
  M5.Lcd.setTextSize(2);
  M5.Lcd.print("Connecting to WiFi");
  Serial.printf_P(PSTR("Connecting to WiFi"));
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    M5.Lcd.print(".");
    delay(250);
  }
  Serial.printf_P(PSTR("\nConnected\n"));
  M5.Lcd.println("");
  M5.Lcd.println("Connected");
  
  Serial.printf_P(PSTR("Go to http://"));
  M5.Lcd.print("Go to http://");
  Serial.print(WiFi.localIP());
  M5.Lcd.print(WiFi.localIP());
  Serial.printf_P(PSTR("/ to control the web radio.\n"));
  M5.Lcd.print("/\nto control the web radio.\n");
  delay(3000);
  M5.Lcd.fillScreen((uint16_t)TFT_BLACK);  
  M5.Lcd.setTextSize(1);
  turn_off_led();
  
  server.begin();
  
  strcpy_P(url, PSTR("none"));
  strcpy_P(status, PSTR("OK"));
  strcpy_P(title, PSTR("Idle"));

  audioLogger = &Serial;
  file = NULL;
  buff = NULL;
  decoder = NULL;

  LoadSettings();
  
#ifdef USE_AVATAR
  gfx2 = new M5UnitLCD();
  if(!gfx2->init()) {
    delete gfx2;
    gfx2 = new M5UnitOLED();
    gfx2->init();
  }
  gfx2->setRotation(3);
  gfx2->fillScreen((uint16_t)TFT_BLACK);  

  gfxSetup(&M5.Display);
  xTaskCreatePinnedToCore(drawLoop, "drawLoop", 2048, NULL, 5, NULL, 1);

  avatar = new Avatar(gfx2);
  if(gfx2->width() >= 135) {
    avatar->setScale(0.5);    //LCD
    avatar->setOffset(-40, -50);
  } else {
    avatar->setScale(0.4);    //OLED
    avatar->setOffset(-95, -90);
  }
  avatar->init(); // start drawing
  avatar->addTask(lipSync, "lipSync");
#else
  gfxSetup(&M5.Display);
  xTaskCreatePinnedToCore(drawLoop, "drawLoop", 2048, NULL, 5, NULL, 1);
#endif
}

void StartNewURL()
{
  Serial.printf_P(PSTR("Changing URL to: %s, vol=%d\n"), url, volume);

  newUrl = false;
  // Stop and free existing ones
  Serial.printf_P(PSTR("Before stop...Free mem=%d\n"), ESP.getFreeHeap());
  StopPlaying();
  Serial.printf_P(PSTR("After stop...Free mem=%d\n"), ESP.getFreeHeap());
  SaveSettings();
  Serial.printf_P(PSTR("Saved settings\n"));
  
//  file = new AudioFileSourceICYStream(stationList[currentStationNumber][1]);
  file = new AudioFileSourceICYStream(url);
  Serial.printf_P(PSTR("created icystream\n"));
  file->RegisterMetadataCB(MDCallback, NULL);
  buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
  Serial.printf_P(PSTR("created buffer\n"));
  buff->RegisterStatusCB(StatusCallback, NULL);
  decoder = isAAC ? (AudioGenerator*) new AudioGeneratorAAC(preallocateCodec, preallocateCodecSize) : (AudioGenerator*) new AudioGeneratorMP3(preallocateCodec, preallocateCodecSize);
  Serial.printf_P(PSTR("created decoder\n"));
  decoder->RegisterStatusCB(StatusCallback, NULL);
  Serial.printf_P("Decoder start...\n");
  decoder->begin(buff, &out);
//  out.SetGain(((float)volume)/100.0);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, (int)(((float)volume)/100.0*255.0));
  if (!decoder->isRunning()) {
    Serial.printf_P(PSTR("Can't connect to URL"));
    StopPlaying();
    strcpy_P(status, PSTR("Unable to connect to URL"));
    retryms = millis() + 2000;
  }
  Serial.printf_P("Done start new URL\n");
}

void LoadSettings()
{
  // Restore from EEPROM, check the checksum matches
  Settings s;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&s);
  EEPROM.begin(sizeof(s));
  for (size_t i=0; i<sizeof(s); i++) {
    ptr[i] = EEPROM.read(i);
  }
  EEPROM.end();
  int16_t sum = 0x1234;
  for (size_t i=0; i<sizeof(url); i++) sum += s.url[i];
  sum += s.isAAC;
  sum += s.volume;
  if (s.checksum == sum) {
    strcpy(url, s.url);
    isAAC = s.isAAC;
    volume = s.volume;
   M5.Speaker.setChannelVolume(m5spk_virtual_channel, (int)(((float)volume)/150.0*255.0));
   Serial.printf_P(PSTR("Resuming stream from EEPROM: %s, type=%s, vol=%d\n"), url, isAAC?"AAC":"MP3", volume);
    newUrl = true;
  }
}

void SaveSettings()
{
  // Store in "EEPROM" to restart automatically
  Settings s;
  memset(&s, 0, sizeof(s));
  strcpy(s.url, url);
  s.isAAC = isAAC;
  s.volume = volume;
  s.checksum = 0x1234;
  for (size_t i=0; i<sizeof(url); i++) s.checksum += s.url[i];
  s.checksum += s.isAAC;
  s.checksum += s.volume;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&s);
  EEPROM.begin(sizeof(s));
  for (size_t i=0; i<sizeof(s); i++) {
    EEPROM.write(i, ptr[i]);
  }
  EEPROM.commit();
  EEPROM.end();
}

void PumpDecoder()
{
  if (decoder && decoder->isRunning()) {
    strcpy_P(status, PSTR("Playing")); // By default we're OK unless the decoder says otherwise
    if (!decoder->loop()) {
      Serial.printf_P(PSTR("Stopping decoder\n"));
      StopPlaying();
      retryms = millis() + 2000;
    }
  }
}

void loop()
{
  static int lastms = 0;
  if (millis()-lastms > 1000) {
    lastms = millis();
    Serial.printf_P(PSTR("Running for %d seconds%c...Free mem=%d\n"), lastms/1000, !decoder?' ':(decoder->isRunning()?'*':' '), ESP.getFreeHeap());
  }

  if (retryms && millis()-retryms>0) {
    retryms = 0;
    newUrl = true;
  }
  
  if (newUrl) {
    StartNewURL();
  }

  PumpDecoder();

  char *reqUrl;
  char *params;
  WiFiClient client = server.available();
  PumpDecoder();
  char reqBuff[384];
  if (client && WebReadRequest(&client, reqBuff, 384, &reqUrl, &params)) {
    PumpDecoder();
    if (IsIndexHTML(reqUrl)) {
      HandleIndex(&client);
    } else if (!strcmp_P(reqUrl, PSTR("stop"))) {
      HandleStop(&client);
    } else if (!strcmp_P(reqUrl, PSTR("status"))) {
      HandleStatus(&client);
    } else if (!strcmp_P(reqUrl, PSTR("title"))) {
      HandleTitle(&client);
    } else if (!strcmp_P(reqUrl, PSTR("setvol"))) {
      HandleVolume(&client, params);
    } else if (!strcmp_P(reqUrl, PSTR("changeurl"))) {
      HandleChangeURL(&client, params);
    } else {
      WebError(&client, 404, NULL, false);
    }
    // web clients hate when door is violently shut
    while (client.available()) {
      PumpDecoder();
      client.read();
    }
  }
  PumpDecoder();
  if (client) {
    client.flush();
    client.stop();
  }
//  gfxLoop(&M5.Display);
//  M5.update();
//  if (M5.BtnA.wasClicked())
//  {
//    M5.Speaker.tone(1000, 100);
////    stop();
////    if (++fileindex >= filecount) { fileindex = 0; }
////    play(filename[fileindex]);
//  }
//  else
//  if (M5.BtnA.isHolding() || M5.BtnB.isPressed() || M5.BtnC.isPressed())
//  {
//    size_t v = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
//    if (M5.BtnB.isPressed()) { --v; } else { ++v; }
//    if (v <= 255 || M5.BtnA.isHolding())
//    {
//      M5.Speaker.setChannelVolume(m5spk_virtual_channel, v);
//    }
//  }
}
