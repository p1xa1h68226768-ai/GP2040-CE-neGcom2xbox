#include "addons/negcon_input.h"
#include "storagemanager.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <math.h>

// 256パターンのステアリング計算結果を保存する一覧表（ルックアップテーブル）
static uint16_t steering_lut[256];

bool NeGconInput::available() {
    return true;
}

void NeGconInput::setup() {
    gpio_init(NEGCON_PIN_DAT);
    gpio_set_dir(NEGCON_PIN_DAT, GPIO_IN);
    gpio_pull_up(NEGCON_PIN_DAT);

    gpio_init(NEGCON_PIN_CMD);
    gpio_set_dir(NEGCON_PIN_CMD, GPIO_OUT);
    
    gpio_init(NEGCON_PIN_ATT);
    gpio_set_dir(NEGCON_PIN_ATT, GPIO_OUT);
    gpio_put(NEGCON_PIN_ATT, 1);
    
    gpio_init(NEGCON_PIN_CLK);
    gpio_set_dir(NEGCON_PIN_CLK, GPIO_OUT);
    gpio_put(NEGCON_PIN_CLK, 1);

    // ===== ステアリングカーブの事前計算 =====
    // マイコン起動時に1回だけ計算を行い、全256パターンの答えを配列に記録します。
    // これにより、プレイ中の計算負荷がゼロになりフリーズを完全に防ぎます。
    
    const float DEADZONE = 0.03f;        // デッドゾーン: 3%
    const float ANTI_DEADZONE = 0.15f;   // アンチデッドゾーン: 15%
    const float SENSITIVITY_CURVE = 0.7f; // カーブ: 0.7で中心感度アップ

    for(int i = 0; i < 256; i++) {
        float raw = 255.0f - (float)i;
        float x = (raw - 127.5f) / 127.5f;

        if (fabs(x) < DEADZONE) {
            steering_lut[i] = 32768; // 遊び領域は完全な中心
            continue;
        }

        float sign = (x > 0.0f) ? 1.0f : -1.0f;
        float nx = (fabs(x) - DEADZONE) / (1.0f - DEADZONE);
        if (nx < 0.0f) nx = 0.0f;

        // powf を使って計算 (起動時のみなので重くても問題ありません)
        float y = powf(nx, SENSITIVITY_CURVE);
        y = ANTI_DEADZONE + y * (1.0f - ANTI_DEADZONE);

        float output = (y * sign * 32767.5f) + 32767.5f;
        
        if (output < 0.0f) output = 0.0f;
        if (output > 65535.0f) output = 65535.0f;

        steering_lut[i] = (uint16_t)output; // 結果を記録
    }

    // ネジコンIC起動待ち時間
    sleep_ms(100); 
}

uint8_t NeGconInput::spi_transfer(uint8_t data) {
    uint8_t rx = 0;
    for(int i = 0; i < 8; i++) {
        gpio_put(NEGCON_PIN_CMD, (data & (1 << i)) ? 1 : 0);
        sleep_us(2);
        gpio_put(NEGCON_PIN_CLK, 0);
        sleep_us(2);
        if(gpio_get(NEGCON_PIN_DAT)) rx |= (1 << i);
        gpio_put(NEGCON_PIN_CLK, 1);
        sleep_us(2);
    }
    gpio_put(NEGCON_PIN_CMD, 1); 
    sleep_us(20); 
    return rx;
}

void NeGconInput::process() {
    Gamepad* gamepad = Storage::getInstance().GetGamepad();

    gpio_put(NEGCON_PIN_ATT, 0);
    sleep_us(10);

    spi_transfer(0x01);
    uint8_t id = spi_transfer(0x42);

    if (id == 0x23) {
        spi_transfer(0x00);
        
        uint8_t data1 = spi_transfer(0x00);
        uint8_t data2 = spi_transfer(0x00);
        uint8_t twist = spi_transfer(0x00);
        uint8_t btn_i = spi_transfer(0x00);
        uint8_t btn_ii = spi_transfer(0x00);
        uint8_t btn_l = spi_transfer(0x00); 
        
        spi_transfer(0x00); 

        // 十字キー
        if ((data1 & 0x10) == 0) gamepad->state.dpad |= GAMEPAD_MASK_UP;
        if ((data1 & 0x20) == 0) gamepad->state.dpad |= GAMEPAD_MASK_RIGHT;
        if ((data1 & 0x40) == 0) gamepad->state.dpad |= GAMEPAD_MASK_DOWN;
        if ((data1 & 0x80) == 0) gamepad->state.dpad |= GAMEPAD_MASK_LEFT;
        
        // デジタルボタン
        if ((data1 & 0x08) == 0) gamepad->state.buttons |= GAMEPAD_MASK_S2; // START
        if ((data2 & 0x10) == 0) gamepad->state.buttons |= GAMEPAD_MASK_B2; // A
        if ((data2 & 0x20) == 0) gamepad->state.buttons |= GAMEPAD_MASK_B1; // B
        if ((data2 & 0x08) == 0) gamepad->state.buttons |= GAMEPAD_MASK_R1; // RB

        // ===== 究極のカスタムマッピング =====
        gamepad->hasAnalogTriggers = true;

        // 1. ねじり -> 左スティックX軸 (事前計算した配列から答えを読むだけ。負荷ゼロ)
        gamepad->state.lx = steering_lut[twist]; 
        
        // 2. 1ボタン -> RT (アクセルとして完全独立)
        gamepad->state.rt = btn_i;

        // 3. 2ボタン -> LT (ブレーキとして完全独立)
        gamepad->state.lt = btn_ii;
        
        // 4. アナログLボタン -> 右スティックY軸
        gamepad->state.ry = 32768 + (btn_l * 128); 
    }

    gpio_put(NEGCON_PIN_ATT, 1);
}
