// ============================================================================
//  CAU HINH PERSISTENT (luu trong NVS) - che do AP cau hinh
// ----------------------------------------------------------------------------
//  Muc dich: khi ESP32 CHUA ket noi duoc WiFi (sai mat khau, chua co mang,
//  doi dia chi server...) thi tu tao mang WiFi rieng (AP) de ket noi vao va
//  mo trang web cau hinh tai http://192.168.4.1/ :
//     - mang WiFi (SSID + mat khau) + IP tinh (optional)
//     - URL server nhan dien (ten mien hoac IP : cong duong dan)
//     - huong camera (lat dọc / lat ngang)
//     - ten + mat khau cua mang AP tu tao
//
//  Tat ca duoc luu trong NVS (Preferences, namespace "camcfg") -> con giu sau
//  khi nguon/cap dien. Lan dau chua co gi luu thi lay mac dinh tu file .ino
//  (ssid/password/PUSH_URL dang hard-code).
//
//  Cach dung trong .ino:
//     cfg_load(ssid, password, PUSH_URL);   // dau setup()
//     WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
//     if (cfg.use_static_ip) cfg_apply_static_ip();
//
//  Trang cau hinh: /setup  (xem setup_page.h), API: /api/cfg, /api/scan
// ============================================================================
#pragma once
#include <Arduino.h>

struct CamCfg {
  // --- Mang WiFi (che do STA) ---
  char wifi_ssid[33];
  char wifi_pass[65];

  // --- IP tinh (0 = dung DHCP) ---
  uint8_t use_static_ip;   // 0/1
  char static_ip[16];
  char static_gw[16];
  char static_mask[16];
  char static_dns[16];

  // --- Server nhan dien (day anh diem danh) ---
  //  VD: http://192.168.1.9:5001/api/esp32/frame  hoac
  //      http://may-chu-truong.local:5001/api/esp32/frame
  char push_url[128];

  // --- Huong camera ---
  int8_t vflip;    // -1 = mac dinh cua cam bien, 0 = tat, 1 = bat
  int8_t hmirror;  // -1 = mac dinh cua cam bien, 0 = tat, 1 = bat

  // --- Mang AP tu tao khi khong ket noi duoc ---
  char ap_ssid[33];
  char ap_pass[65];   // rong hoac < 8 ky tu -> AP mo (khong mat khau)
};

extern CamCfg cfg;
extern bool cfg_setup_mode;   // true = dang chay o che do AP cau hinh

// Doc cau hinh tu NVS; thieu key nao thi lay mac dinh tu .ino (def_*)
// (mac dinh duoc giu lai de cfg_reset() dung khi nguoi dung khoi phuc)
void cfg_load(const char *def_ssid, const char *def_pass, const char *def_push);
// Ghi toan bo cfg xuong NVS
void cfg_save();
// Xoa het cau hinh da luu + ve mac dinh (can reset nguoi dung khi bi loi)
void cfg_reset();
// Goi WiFi.config() neu use_static_ip = 1 -> true = da cai IP tinh
bool cfg_apply_static_ip();
