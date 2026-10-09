// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include "sdkconfig.h"
#include "camera_index.h"
#include "board_config.h"

#include <lwip/sockets.h>
#include <lwip/tcp.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// LED FLASH setup
#if defined(LED_GPIO_NUM)
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;
bool isStreaming = false;

#endif

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
// Gui boundary + header trong MOT lan send -> it syscall, khung hinh ra man hon
static const char *_STREAM_PART = "\r\n--" PART_BOUNDARY "\r\n"
                                 "Content-Type: image/jpeg\r\n"
                                 "Content-Length: %u\r\n"
                                 "X-Timestamp: %d.%06d\r\n\r\n";

// Cam khong cache de browser/luong luon lay frame moi nhat
static const char *_NO_CACHE_HDRS[] = {"Cache-Control", "no-store, no-cache, must-revalidate, max-age=0", "Pragma", "no-cache"};

// Tinh chinh tung socket: giam do tre phan manh khi stream anh
static esp_err_t httpd_open_tune(httpd_handle_t hd, int sockfd) {
  (void)hd;
  int flag = 1;
  // 1) Tat Nagle: header multipart nho gui ngay, khong cho doi ACK
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  // 2) Tang bo dem gui TCP (mac dinh chi 5760B < kich thuoc 1 khung JPEG)
  //    -> khung anh gui het trong 1 lan, khong phai cho ACK nhieu lan
  int sndbuf = 16384;
  setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  return ESP_OK;
}

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

// Trang thai day luong live (dinh nghia trong .ino)
extern volatile int g_push_state;
extern volatile uint32_t g_push_ok;
extern volatile uint32_t g_push_err;
extern const char *g_push_url;

// Trang thai camera (dinh nghia trong .ino) — che do nut nhan giu cam OFF khi nghi.
// app_httpd.cpp khong nhin thay #define BUTTON_PIN (nam trong .ino) nen dung bien.
extern volatile bool g_cam_on;       // true = camera da init/san sang chup
extern volatile bool g_stream_active; // true = stream :81 dang chay -> camSleep() tu choi
extern int g_button_pin;             // GPIO nut nhan (-1 = tat che do nut nhan)
bool camWake();                      // bat camera (init lai neu can) -> false = loi
void camSleep();                     // tat camera (chi khi khong stream)

typedef struct {
  size_t size;   //number of values used for filtering
  size_t index;  //current value index
  size_t count;  //value count
  int sum;
  int *values;  //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
  memset(filter, 0, sizeof(ra_filter_t));

  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values) {
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));

  filter->size = sample_size;
  return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value) {
  if (!filter->values) {
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size) {
    filter->count++;
  }
  return filter->sum / filter->count;
}
#endif

#if defined(LED_GPIO_NUM)
void enable_led(bool en) {  // Turn LED On or Off
  int duty = en ? led_duty : 0;
  if (en && isStreaming && (led_duty > CONFIG_LED_MAX_INTENSITY)) {
    duty = CONFIG_LED_MAX_INTENSITY;
  }
  ledcWrite(LED_GPIO_NUM, duty);
  //ledc_set_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL, duty);
  //ledc_update_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL);
  log_i("Set LED intensity to %d", duty);
}
#endif

static esp_err_t bmp_handler(httpd_req_t *req) {
  // Cam co the dang tat (che do nut nhan) -> tu day len truoc khi chup
  if (!g_cam_on && !camWake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_start = esp_timer_get_time();
#endif
  fb = esp_camera_fb_get();
  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/x-windows-bmp");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  uint8_t *buf = NULL;
  size_t buf_len = 0;
  bool converted = frame2bmp(fb, &buf, &buf_len);
  esp_camera_fb_return(fb);
  if (!converted) {
    log_e("BMP Conversion failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  res = httpd_resp_send(req, (const char *)buf, buf_len);
  free(buf);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_end = esp_timer_get_time();
#endif
  log_i("BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), buf_len);
  return res;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  // Cam co the dang tat (che do nut nhan) -> tu day len truoc khi chup (CAM_WAKE_ON_HTTP)
  if (!g_cam_on && !camWake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_start = esp_timer_get_time();
#endif

#if defined(LED_GPIO_NUM)
  enable_led(true);
  vTaskDelay(150 / portTICK_PERIOD_MS);  // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
  fb = esp_camera_fb_get();              // or it won't be visible in the frame. A better way to do this is needed.
  enable_led(led_duty > 0);              // giu lai trang thai flash neu nguoi dung dang bat
#else
  fb = esp_camera_fb_get();
#endif

  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  size_t fb_len = 0;
#endif
  if (fb->format == PIXFORMAT_JPEG) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = fb->len;
#endif
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = jchunk.len;
#endif
  }
  esp_camera_fb_return(fb);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_end = esp_timer_get_time();
#endif
  log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  // Cam co the dang tat (che do nut nhan) -> tu day len truoc khi stream
  if (!g_cam_on && !camWake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }
  // Danh dau stream dang chay -> camSleep() (tu /live?state=0 hoac nhip tu
  // tat cam cua nut nhan) KHONG duoc esp_camera_deinit giua khung hinh.
  // Bat dau tu day (truoc loop) de khong co khoang rong; moi duong ra deu
  // phai dat lai false.
  g_stream_active = true;

  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[192];

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    g_stream_active = false;
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");
  httpd_resp_set_hdr(req, _NO_CACHE_HDRS[0], _NO_CACHE_HDRS[1]);
  httpd_resp_set_hdr(req, _NO_CACHE_HDRS[2], _NO_CACHE_HDRS[3]);

#if defined(LED_GPIO_NUM)
  isStreaming = true;
  enable_led(true);
#endif

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          log_e("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      // boundary + header goi chung trong 1 lan send (truoc kia la 2 lan)
      size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, (unsigned)_jpg_buf_len, (int)_timestamp.tv_sec, (int)_timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      log_e("Send frame failed");
      break;
    }
    int64_t fr_end = esp_timer_get_time();

    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;

    frame_time /= 1000;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
#endif
    // Tat log moi khung (log_i 10fps lam nghet UART 115200 -> lag stream).
    // Muon do fps thi mo log_d hoac xem tren web /status.
    log_d(
      "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)", (uint32_t)(_jpg_buf_len), (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time, avg_frame_time,
      1000.0 / avg_frame_time
    );
  }

  g_stream_active = false;   // loop stream da ket thuc -> camSleep() phep chay lai

#if defined(LED_GPIO_NUM)
  isStreaming = false;
  enable_led(led_duty > 0);  // khong tat flash neu nguoi dung van muon giu sang
#endif

  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf = NULL;
  char variable[32];
  char value[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK || httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int val = atoi(value);
  log_i("%s = %d", variable, val);
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    // cam dang tat (che do nut nhan) -> khong co sensor de dieu chinh
    return httpd_resp_send_500(req);
  }
  int res = 0;

  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "contrast")) {
    res = s->set_contrast(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else if (!strcmp(variable, "saturation")) {
    res = s->set_saturation(s, val);
  } else if (!strcmp(variable, "gainceiling")) {
    res = s->set_gainceiling(s, (gainceiling_t)val);
  } else if (!strcmp(variable, "colorbar")) {
    res = s->set_colorbar(s, val);
  } else if (!strcmp(variable, "awb")) {
    res = s->set_whitebal(s, val);
  } else if (!strcmp(variable, "agc")) {
    res = s->set_gain_ctrl(s, val);
  } else if (!strcmp(variable, "aec")) {
    res = s->set_exposure_ctrl(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "awb_gain")) {
    res = s->set_awb_gain(s, val);
  } else if (!strcmp(variable, "agc_gain")) {
    res = s->set_agc_gain(s, val);
  } else if (!strcmp(variable, "aec_value")) {
    res = s->set_aec_value(s, val);
  } else if (!strcmp(variable, "aec2")) {
    res = s->set_aec2(s, val);
  } else if (!strcmp(variable, "dcw")) {
    res = s->set_dcw(s, val);
  } else if (!strcmp(variable, "bpc")) {
    res = s->set_bpc(s, val);
  } else if (!strcmp(variable, "wpc")) {
    res = s->set_wpc(s, val);
  } else if (!strcmp(variable, "raw_gma")) {
    res = s->set_raw_gma(s, val);
  } else if (!strcmp(variable, "lenc")) {
    res = s->set_lenc(s, val);
  } else if (!strcmp(variable, "special_effect")) {
    res = s->set_special_effect(s, val);
  } else if (!strcmp(variable, "wb_mode")) {
    res = s->set_wb_mode(s, val);
  } else if (!strcmp(variable, "ae_level")) {
    res = s->set_ae_level(s, val);
  }
#if defined(LED_GPIO_NUM)
  else if (!strcmp(variable, "led_intensity")) {
    led_duty = val;
    if (isStreaming) {
      enable_led(true);
    }
  }
#endif
  else {
    log_i("Unknown command: %s", variable);
    res = -1;
  }

  if (res < 0) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask) {
  return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1600];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;

  if (s == NULL) {
    // Cam dang tat (che do nut nhan chua day / da tat) -> JSON toi gian,
    // trang web van doc duoc (JS tren trang co kiem tra tung truong).
    int n = snprintf(json_response, sizeof(json_response),
                     "{\"cam_on\":0,\"button\":%d,\"push\":%d,\"push_ok\":%u,\"push_err\":%u,\"push_url\":\"%s\"}",
                     g_button_pin, g_push_state, (unsigned)g_push_ok, (unsigned)g_push_err, g_push_url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, n);
  }

  *p++ = '{';

  if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
    for (int reg = 0x3400; reg < 0x3406; reg += 2) {
      p += print_reg(p, s, reg, 0xFFF);  //12 bit
    }
    p += print_reg(p, s, 0x3406, 0xFF);

    p += print_reg(p, s, 0x3500, 0xFFFF0);  //16 bit
    p += print_reg(p, s, 0x3503, 0xFF);
    p += print_reg(p, s, 0x350a, 0x3FF);   //10 bit
    p += print_reg(p, s, 0x350c, 0xFFFF);  //16 bit

    for (int reg = 0x5480; reg <= 0x5490; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5380; reg <= 0x538b; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5580; reg < 0x558a; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    p += print_reg(p, s, 0x558a, 0x1FF);  //9 bit
  } else if (s->id.PID == OV2640_PID) {
    p += print_reg(p, s, 0xd3, 0xFF);
    p += print_reg(p, s, 0x111, 0xFF);
    p += print_reg(p, s, 0x132, 0xFF);
  }

  p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
  p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p += sprintf(p, "\"awb\":%u,", s->status.awb);
  p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p += sprintf(p, "\"aec\":%u,", s->status.aec);
  p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p += sprintf(p, "\"agc\":%u,", s->status.agc);
  p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
  p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if defined(LED_GPIO_NUM)
  p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
  p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
  p += sprintf(p, ",\"push\":%d,\"push_ok\":%u,\"push_err\":%u,\"push_url\":\"%s\"", g_push_state, (unsigned)g_push_ok, (unsigned)g_push_err, g_push_url);
  // cam_on = trang thai nguon sensor; button = GPIO nut nhan (-1 = tat che do nhan)
  p += sprintf(p, ",\"cam_on\":1,\"button\":%d", g_button_pin);
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t xclk_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _xclk[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int xclk = atoi(_xclk);
  log_i("Set XCLK: %d MHz", xclk);

  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    return httpd_resp_send_500(req);
  }
  int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];
  char _val[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK
      || httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  int val = atoi(_val);
  log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    return httpd_resp_send_500(req);
  }
  int res = s->set_reg(s, reg, mask, val);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    return httpd_resp_send_500(req);
  }
  int res = s->get_reg(s, reg, mask);
  if (res < 0) {
    return httpd_resp_send_500(req);
  }
  log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

  char buffer[20];
  const char *val = itoa(res, buffer, 10);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, val, strlen(val));
}

static int parse_get_var(char *buf, const char *key, int def) {
  char _int[16];
  if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
    return def;
  }
  return atoi(_int);
}

static esp_err_t pll_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int bypass = parse_get_var(buf, "bypass", 0);
  int mul = parse_get_var(buf, "mul", 0);
  int sys = parse_get_var(buf, "sys", 0);
  int root = parse_get_var(buf, "root", 0);
  int pre = parse_get_var(buf, "pre", 0);
  int seld5 = parse_get_var(buf, "seld5", 0);
  int pclken = parse_get_var(buf, "pclken", 0);
  int pclk = parse_get_var(buf, "pclk", 0);
  free(buf);

  log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d", bypass, mul, sys, root, pre, seld5, pclken, pclk);
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    return httpd_resp_send_500(req);
  }
  int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int startX = parse_get_var(buf, "sx", 0);
  int startY = parse_get_var(buf, "sy", 0);
  int endX = parse_get_var(buf, "ex", 0);
  int endY = parse_get_var(buf, "ey", 0);
  int offsetX = parse_get_var(buf, "offx", 0);
  int offsetY = parse_get_var(buf, "offy", 0);
  int totalX = parse_get_var(buf, "tx", 0);
  int totalY = parse_get_var(buf, "ty", 0);  // codespell:ignore totaly
  int outputX = parse_get_var(buf, "ox", 0);
  int outputY = parse_get_var(buf, "oy", 0);
  bool scale = parse_get_var(buf, "scale", 0) == 1;
  bool binning = parse_get_var(buf, "binning", 0) == 1;
  free(buf);

  log_i(
    "Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u", startX, startY, endX, endY, offsetX, offsetY,
    totalX, totalY, outputX, outputY, scale, binning  // codespell:ignore totaly
  );
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    return httpd_resp_send_500(req);
  }
  int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);  // codespell:ignore totaly
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

#if defined(LED_GPIO_NUM)
// Bat/tat den Flash: /flash?state=1 hoac /flash?state=0
static esp_err_t flash_handler(httpd_req_t *req) {
  char *buf = NULL;
  int state = 1;

  if (parse_get(req, &buf) == ESP_OK) {
    state = parse_get_var(buf, "state", 1) ? 1 : 0;
    free(buf);
  }

  led_duty = state ? CONFIG_LED_MAX_INTENSITY : 0;
  enable_led(state != 0);

  char resp[32];
  int len = snprintf(resp, sizeof(resp), "{\"flash\":%d}", state);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, len);
}
#endif

// Dieu khien nguon camera tu ben ngoai (panel debug cua server Python):
//   GET /live?state=1 -> camWake() (bat camera)
//   GET /live?state=0 -> camSleep() (tat camera; tu choi neu stream dang chay)
// Tra ve {"ok":0|1,"cam":0|1} (cam = trang thai THUC TE sau khi xu ly).
static esp_err_t live_handler(httpd_req_t *req) {
  char *buf = NULL;
  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;   // parse_get da gui 404 (thieu query string)
  }
  int state = parse_get_var(buf, "state", 1) ? 1 : 0;
  free(buf);

  if (state) {
    if (!camWake()) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      return httpd_resp_send(req, NULL, 0);
    }
  } else {
    camSleep();   // neu stream dang chay -> camSleep() tu choi, cam van = 1
  }

  char resp[48];
  int len = snprintf(resp, sizeof(resp), "{\"ok\":1,\"cam\":%d}", g_cam_on ? 1 : 0);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, len);
}

// Trang web gon, nhe -> stream muot hon, co nut bat/tat Flash
static const char INDEX_HTML[] = R"HTMLDOC(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-CAM AI Thinker</title>
<style>
  *{box-sizing:border-box}
  body{background:#101216;color:#e8eaed;font-family:system-ui,Arial,sans-serif;margin:0;padding:14px;text-align:center}
  h1{font-size:17px;margin:4px 0 12px;font-weight:600}
  #box{position:relative;margin:0 auto 4px;background:#000;border:1px solid #2b2f36;border-radius:10px;overflow:hidden}
  #stream{position:absolute;left:50%;top:50%;display:block;transform-origin:50% 50%}
  .row{margin:10px auto;max-width:680px;display:flex;flex-wrap:wrap;gap:8px;justify-content:center;align-items:center}
  .lbl{font-size:13px;color:#9aa0a6}
  button,select{padding:11px 16px;font-size:14px;border-radius:9px;border:0;cursor:pointer}
  button{background:#2f6df6;color:#fff;font-weight:600}
  button.off{background:#3a3f47;color:#cfd2d6}
  button.grey{background:#3a3f47;color:#fff}
  button.on{background:#22b573;color:#04170d}
  button.sm{padding:8px 11px;font-size:13px}
  select{background:#1c1f24;color:#e8eaed;border:1px solid #333}
  #status{margin-top:8px;font-size:12px;color:#8fd18f;min-height:16px}
  #push{margin-top:3px;font-size:12px;color:#8ab4ff;word-break:break-all;min-height:16px}
</style>
</head>
<body>
  <h1>ESP32-CAM AI Thinker</h1>
  <div id="box"><img id="stream" alt="stream"></div>
  <div class="row">
    <button id="flashOn">BẬT FLASH</button>
    <button id="flashOff" class="off">TẮT FLASH</button>
    <button id="capture" class="grey">Chụp ảnh</button>
  </div>
  <div class="row">
    <span class="lbl">Xoay khung hình</span>
    <button class="rot sm grey" data-rot="0">0&deg;</button>
    <button class="rot sm grey" data-rot="90">90&deg;</button>
    <button class="rot sm grey" data-rot="180">180&deg;</button>
    <button class="rot sm grey" data-rot="270">270&deg;</button>
    <button id="vflip" class="sm grey">Lật dọc</button>
    <button id="hmirror" class="sm grey">Lật ngang</button>
    <button id="rot180" class="sm grey">Xoay 180&deg; (cả ảnh chụp)</button>
  </div>
  <div class="row">
    <span class="lbl">Độ phân giải</span>
    <select id="size">
      <option value="5" selected>QVGA 320x240 (siêu mượt)</option>
      <option value="6">CIF 400x296</option>
      <option value="7">HVGA 480x320</option>
      <option value="8">VGA 640x480 (nhận xa, lag hơn)</option>
      <option value="9">SVGA 800x600 (nét, cần WiFi khỏe)</option>
      <option value="10">XGA 1024x768</option>
      <option value="11">HD 1280x720</option>
      <option value="12">SXGA 1280x1024</option>
      <option value="13">UXGA 1600x1200</option>
    </select>
    <span class="lbl">Chất lượng (nhỏ hơn = nét hơn)</span>
    <select id="quality">
      <option value="10">10 (nét, nặng)</option>
      <option value="12">12</option>
      <option value="15" selected>15 (cân bằng mượt)</option>
      <option value="20">20 (nhẹ/nhanh)</option>
      <option value="30">30 (siêu nhẹ)</option>
    </select>
  </div>
  <div class="row">
    <span class="lbl">Tùy chọn nhanh</span>
    <button class="preset sm grey" data-fs="5" data-q="15">Siêu mượt (QVGA)</button>
    <button class="preset sm grey" data-fs="8" data-q="18">Cân bằng (VGA)</button>
    <button class="preset sm grey" data-fs="9" data-q="15">Nét (SVGA)</button>
  </div>
  <div id="status">Đang tải luồng video...</div>
  <div id="push">Đang kiểm tra push...</div>
<script>
  var host = location.hostname;
  var box = document.getElementById('box');
  var stream = document.getElementById('stream');
  var statusEl = document.getElementById('status');
  var sizeSel = document.getElementById('size');
  var qualSel = document.getElementById('quality');
  var pushEl = document.getElementById('push');

  // Kich thuoc theo tung do phan giai (de tinh ti le khung hinh cho phep xoay)
  var FRAMES = {0:[96,96],1:[160,120],2:[176,144],3:[240,176],4:[240,240],5:[320,240],6:[400,296],7:[480,320],
                8:[640,480],9:[800,600],10:[1024,768],11:[1280,720],12:[1280,1024],13:[1600,1200]};
  var fs = 5, rot = 0, vflip = 0, hmirror = 0;

  stream.src = 'http://' + host + ':81/stream';
  stream.onload = function(){ statusEl.textContent = 'Đang truyền video'; };
  stream.onerror = function(){
    statusEl.textContent = 'Mất kết nối video, đang thử lại...';
    setTimeout(function(){ stream.src = 'http://' + host + ':81/stream?' + Date.now(); }, 1500);
  };

  // Tinh lai kich thuoc hop khi co xoay 90/270 -> anh khong bi cat
  function layout(){
    var d = FRAMES[fs] || FRAMES[5];
    var ar = d[0] / d[1];
    var maxW = Math.min(680, document.documentElement.clientWidth - 30);
    var maxH = Math.max(180, Math.min(600, window.innerHeight - 280));
    var rot90 = (rot === 90 || rot === 270);
    var X = rot90 ? Math.min(maxW * ar, maxH) : Math.min(maxW, maxH * ar);
    var iw = Math.round(X), ih = Math.round(X / ar);
    box.style.width = (rot90 ? ih : iw) + 'px';
    box.style.height = (rot90 ? iw : ih) + 'px';
    stream.style.width = iw + 'px';
    stream.style.height = ih + 'px';
    stream.style.transform = 'translate(-50%,-50%) rotate(' + rot + 'deg)';
  }

  function ctl(v, val){
    return fetch('/control?var=' + v + '&val=' + val).catch(function(){});
  }

  function setRot(r){
    rot = r;
    var all = document.querySelectorAll('.rot');
    for (var i = 0; i < all.length; i++) {
      all[i].className = 'rot sm ' + (+all[i].getAttribute('data-rot') === r ? 'on' : 'grey');
    }
    layout();
  }

  function updateFlipUI(){
    var b1 = document.getElementById('vflip'), b2 = document.getElementById('hmirror');
    b1.className = 'sm ' + (vflip ? 'on' : 'grey');
    b2.className = 'sm ' + (hmirror ? 'on' : 'grey');
    document.getElementById('rot180').className = 'sm ' + ((vflip && hmirror) ? 'on' : 'grey');
  }

  function applyFs(v, send){
    fs = v;
    sizeSel.value = v;
    if (send !== false) ctl('framesize', v);
    layout();
  }
  function applyQuality(v, send){
    qualSel.value = v;
    if (send !== false) ctl('quality', v);
  }

  sizeSel.onchange = function(e){ applyFs(+e.target.value); };
  qualSel.onchange = function(e){ applyQuality(+e.target.value); };

  var rots = document.querySelectorAll('.rot');
  for (var i = 0; i < rots.length; i++) {
    rots[i].onclick = function(){ setRot(+this.getAttribute('data-rot')); };
  }
  var presets = document.querySelectorAll('.preset');
  for (var i = 0; i < presets.length; i++) {
    presets[i].onclick = function(){
      applyQuality(+this.getAttribute('data-q'));
      applyFs(+this.getAttribute('data-fs'));
    };
  }

  document.getElementById('vflip').onclick = function(){ vflip = vflip ? 0 : 1; updateFlipUI(); ctl('vflip', vflip); };
  document.getElementById('hmirror').onclick = function(){ hmirror = hmirror ? 0 : 1; updateFlipUI(); ctl('hmirror', hmirror); };
  document.getElementById('rot180').onclick = function(){
    var on = !(vflip && hmirror);
    vflip = on ? 1 : 0;
    hmirror = on ? 1 : 0;
    updateFlipUI();
    ctl('vflip', vflip);
    ctl('hmirror', hmirror);
  };
  document.getElementById('flashOn').onclick = function(){ fetch('/flash?state=1').catch(function(){}); };
  document.getElementById('flashOff').onclick = function(){ fetch('/flash?state=0').catch(function(){}); };
  document.getElementById('capture').onclick = function(){ window.open('/capture', '_blank'); };

  setRot(0);
  window.onresize = layout;

  // Dong bo trang thai tu may chiu
  fetch('/status').then(function(r){ return r.json(); }).then(function(j){
    if (typeof j.framesize === 'number' && FRAMES[j.framesize]) applyFs(j.framesize, false);
    if (typeof j.quality === 'number') {
      var opt = qualSel.querySelector('option[value="' + j.quality + '"]');
      if (opt) applyQuality(j.quality, false);
    }
    vflip = j.vflip ? 1 : 0;
    hmirror = j.hmirror ? 1 : 0;
    updateFlipUI();
    layout();
  }).catch(function(){});

  // Hien thi trang thai day luong len may chu
  function refreshPush(){
    fetch('/status').then(function(r){ return r.json(); }).then(function(j){
      if (typeof j.push_url !== 'string') return;
      var head = (j.push === 1) ? 'Đang push luồng live → ' : 'Chưa kết nối push → ';
      var cnt = (typeof j.push_ok === 'number') ? '  (' + j.push_ok + ' khung ok, ' + (j.push_err || 0) + ' lỗi)' : '';
      pushEl.textContent = head + j.push_url + cnt;
      pushEl.style.color = (j.push === 1) ? '#8ab4ff' : '#e0a060';
    }).catch(function(){});
  }
  refreshPush();
  setInterval(refreshPush, 10000);
</script>
</body>
</html>
)HTMLDOC";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// Trang cau hinh day du (goc cua Espressif) van giu lai tai /advanced
static esp_err_t advanced_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    if (s->id.PID == OV3660_PID) {
      return httpd_resp_send(req, (const char *)index_ov3660_html_gz, index_ov3660_html_gz_len);
    } else if (s->id.PID == OV5640_PID) {
      return httpd_resp_send(req, (const char *)index_ov5640_html_gz, index_ov5640_html_gz_len);
    } else {
      return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
    }
  } else {
    log_e("Camera sensor not found");
    return httpd_resp_send_500(req);
  }
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 16;
  config.max_open_sockets = 4;          // 4 (thay vi 7): moi viewer them 1 fb_get tranh nhau
                                        // -> gioi han client de stream muot, LRU tu dong da client cu
  config.lru_purge_enable = true;       // tu dong dong ket noi cu khi day
  config.stack_size = 8192;             // stack lon hon -> on dinh hon khi stream
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;
  config.keep_alive_enable = true;
  config.open_fn = httpd_open_tune;   // TCP_NODELAY cho moi ket noi

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t advanced_uri = {
    .uri = "/advanced",
    .method = HTTP_GET,
    .handler = advanced_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

#if defined(LED_GPIO_NUM)
  httpd_uri_t flash_uri = {
    .uri = "/flash",
    .method = HTTP_GET,
    .handler = flash_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };
#endif

  httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t live_uri = {
    .uri = "/live",
    .method = HTTP_GET,
    .handler = live_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t cmd_uri = {
    .uri = "/control",
    .method = HTTP_GET,
    .handler = cmd_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t bmp_uri = {
    .uri = "/bmp",
    .method = HTTP_GET,
    .handler = bmp_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t xclk_uri = {
    .uri = "/xclk",
    .method = HTTP_GET,
    .handler = xclk_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t reg_uri = {
    .uri = "/reg",
    .method = HTTP_GET,
    .handler = reg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t greg_uri = {
    .uri = "/greg",
    .method = HTTP_GET,
    .handler = greg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t pll_uri = {
    .uri = "/pll",
    .method = HTTP_GET,
    .handler = pll_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t win_uri = {
    .uri = "/resolution",
    .method = HTTP_GET,
    .handler = win_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  ra_filter_init(&ra_filter, 20);

  log_i("Starting web server on port: '%d'", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &advanced_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &live_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &bmp_uri);
#if defined(LED_GPIO_NUM)
    httpd_register_uri_handler(camera_httpd, &flash_uri);
#endif

    httpd_register_uri_handler(camera_httpd, &xclk_uri);
    httpd_register_uri_handler(camera_httpd, &reg_uri);
    httpd_register_uri_handler(camera_httpd, &greg_uri);
    httpd_register_uri_handler(camera_httpd, &pll_uri);
    httpd_register_uri_handler(camera_httpd, &win_uri);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  log_i("Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, 5000, 8);
#else
  log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}
