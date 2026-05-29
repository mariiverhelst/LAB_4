#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

// RFID + I2C + RTC (asumiendo ports Arduino compatibles o libs adaptadas)
#include "MFRC522.h"
#include "LiquidCrystal_I2C.h"
#include "RTClib.h"

// BLE
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

// ── PINES ─────────────────────────────────────────────────────
#define PIN_LED_ROJO    2
#define PIN_LED_VERDE   25
#define PIN_LED_AZUL    4
#define PIN_BUZZER      32
#define PIN_RFID_SS      5
#define PIN_RFID_RST    27

// ── OBJETOS ───────────────────────────────────────────────────
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;

// ── TARJETAS ──────────────────────────────────────────────────
uint8_t tarjetaAzul[4]   = {0xC7, 0x25, 0x1C, 0x07};
uint8_t tarjetaBlanca[4] = {0xC1, 0x50, 0x16, 0x07};

// ── ESTADOS ───────────────────────────────────────────────────
typedef enum { BLOQUEADO, ACTIVO } Estado;
Estado estadoActual = BLOQUEADO;

// ── VARIABLES ──────────────────────────────────────────────────
char ultimoMensajeBLE[20] = "";
uint64_t ultimaActHora = 0;

// ─────────────────────────────────────────────────────────────
// COMPARAR UID
// ─────────────────────────────────────────────────────────────
bool esTarjeta(uint8_t* uid, uint8_t* ref) {
  for (int i = 0; i < 4; i++) {
    if (uid[i] != ref[i]) return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────
// HORA RTC
// ─────────────────────────────────────────────────────────────
char* obtenerHora() {
  static char buf[9];
  DateTime now = rtc.now();
  sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return buf;
}

// ─────────────────────────────────────────────────────────────
// BLOQUEO
// ─────────────────────────────────────────────────────────────
void mostrarBloqueado() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Panel bloqueado");
  lcd.setCursor(0, 1);
  lcd.print("Acerque credenc");

  gpio_set_level(PIN_LED_ROJO, 1);
  gpio_set_level(PIN_LED_VERDE, 0);
  gpio_set_level(PIN_LED_AZUL, 0);
}

// ─────────────────────────────────────────────────────────────
// BEEP
// ─────────────────────────────────────────────────────────────
void beep(int f, int t) {
  ledc_set_freq(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0, f);
  ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, 128);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);

  vTaskDelay(pdMS_TO_TICKS(t));

  ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, 0);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);
}

// ─────────────────────────────────────────────────────────────
// ACCESO OK
// ─────────────────────────────────────────────────────────────
void accesoOK() {

  gpio_set_level(PIN_LED_ROJO, 0);
  gpio_set_level(PIN_LED_VERDE, 1);

  beep(1000, 500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Acceso concedido");
  lcd.setCursor(0, 1);
  lcd.print(obtenerHora());

  vTaskDelay(pdMS_TO_TICKS(1000));

  gpio_set_level(PIN_LED_VERDE, 0);
  gpio_set_level(PIN_LED_AZUL, 1);

  estadoActual = ACTIVO;
  strcpy(ultimoMensajeBLE, "");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Mensajes");
  lcd.setCursor(0, 1);
  lcd.print(obtenerHora());
}

// ─────────────────────────────────────────────────────────────
// DENEGADO
// ─────────────────────────────────────────────────────────────
void accesoDenegado() {

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Acceso denegado");
  lcd.setCursor(0, 1);
  lcd.print("UID no registrado");

  beep(500, 2000);

  mostrarBloqueado();
}

// ─────────────────────────────────────────────────────────────
// CERRAR SESIÓN
// ─────────────────────────────────────────────────────────────
void cerrarSesion() {

  beep(1000, 500);

  gpio_set_level(PIN_LED_AZUL, 0);
  gpio_set_level(PIN_LED_ROJO, 1);

  estadoActual = BLOQUEADO;
  strcpy(ultimoMensajeBLE, "");

  mostrarBloqueado();
}

// ─────────────────────────────────────────────────────────────
// LOOP (TASK)
// ─────────────────────────────────────────────────────────────
void app_task(void *arg) {

  while (1) {

    uint64_t now = esp_timer_get_time() / 1000;

    if (estadoActual == ACTIVO && now - ultimaActHora > 1000) {
      ultimaActHora = now;

      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      lcd.print(obtenerHora());
    }

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint8_t* uid = rfid.uid.uidByte;

    bool blanca = esTarjeta(uid, tarjetaBlanca);
    bool azul   = esTarjeta(uid, tarjetaAzul);

    if (estadoActual == BLOQUEADO) {
      if (blanca) accesoOK();
      else accesoDenegado();
    }
    else {
      if (blanca) cerrarSesion();
      if (azul) cerrarSesion();
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ─────────────────────────────────────────────────────────────
// INIT
// ─────────────────────────────────────────────────────────────
void app_main(void) {

  // GPIO
  gpio_set_direction(PIN_LED_ROJO, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_LED_VERDE, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_LED_AZUL, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);

  gpio_set_level(PIN_LED_ROJO, 1);

  // PWM buzzer
  ledc_timer_config_t timer = {
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 1000,
    .duty_resolution = LEDC_TIMER_8_BIT
  };
  ledc_timer_config(&timer);

  ledc_channel_config_t channel = {
    .gpio_num = PIN_BUZZER,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 0
  };
  ledc_channel_config(&channel);

  // LCD
  lcd.init();
  lcd.backlight();

  // RTC
  if (!rtc.begin()) {
    lcd.print("RTC ERROR");
    while (1);
  }

  rtc.adjust(DateTime(__DATE__, __TIME__));

  // RFID
  rfid.PCD_Init();

  mostrarBloqueado();

  // TASK
  xTaskCreate(app_task, "app_task", 4096, NULL, 5, NULL);
}