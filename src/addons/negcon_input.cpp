#include "addons/negcon_input.h"
#include "storagemanager.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

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

// 高速な整数ステアリングカーブ演算
uint16_t apply_steering_curve(uint8_t twist_raw) {
    int32_t raw = 255 - twist_raw; 
    int32_t x = raw - 127;         

    const int32_t DEADZONE = 4;        
    const int32_t ANTI_DEADZONE = 20;  

    if (abs(x) < DEADZONE) {
        return 32768; 
    }

    int32_t sign = (x > 0) ? 1 : -1;
    int32_t nx = abs(x) - DEADZONE;          
    int32_t max_nx = 127 - DEADZONE;         

    int32_t y = (nx * nx) / max_nx;
    y = ANTI_DEADZONE + (y * (max_nx - ANTI_DEADZONE)) / max_nx;

    int32_t output = 32768 + (y * sign * 266);
    
    if (output < 0) output = 0;
    if (output > 65535) output = 65535;

    return (uint16_t)output;
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

        // 十字キー (クリーンなビット反転判定に最適化)
        if (~data1 & 0x10) gamepad->state.dpad |= GAMEPAD_MASK_UP;
        if (~data1 & 0x20) gamepad->state.dpad |= GAMEPAD_MASK_RIGHT;
        if (~data1 & 0x40) gamepad->state.dpad |= GAMEPAD_MASK_DOWN;
        if (~data1 & 0x80) gamepad->state.dpad |= GAMEPAD_MASK_LEFT;
        
        // デジタルボタン
        if (~data1 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_S2; // START
        if (~data2 & 0x10) gamepad->state.buttons |= GAMEPAD_MASK_B2; // A
        if (~data2 & 0x20) gamepad->state.buttons |= GAMEPAD_MASK_B1; // B
        if (~data2 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_R1; // RB

        gamepad->hasAnalogTriggers = true;

        // 1. ねじり -> 左スティックX軸
        gamepad->state.lx = apply_steering_curve(twist); 
        
        // 2. 1ボタン -> RT (反転を解除：未入力で0、最大で65535)
        gamepad->state.rt = btn_i * 257;

        // 3. 2ボタン -> LT (反転を解除：未入力で0、最大で65535)
        gamepad->state.lt = btn_ii * 257;
        
        // 4. アナログLボタン -> 右スティックY軸 (反転を解除：未入力で中心、最大で下端)
        gamepad->state.ry = 32768 + (btn_l * 128); 
    }

    gpio_put(NEGCON_PIN_ATT, 1);
}
