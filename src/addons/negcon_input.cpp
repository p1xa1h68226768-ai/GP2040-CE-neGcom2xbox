#include "addons/negcon_input.h"
#include "storagemanager.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h" 

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
    uint32_t interrupts = save_and_disable_interrupts(); 
    
    for(int i = 0; i < 8; i++) {
        gpio_put(NEGCON_PIN_CMD, (data & (1 << i)) ? 1 : 0);
        sleep_us(2);
        gpio_put(NEGCON_PIN_CLK, 0);
        sleep_us(2);
        if(gpio_get(NEGCON_PIN_DAT)) rx |= (1 << i);
        gpio_put(NEGCON_PIN_CLK, 1);
        sleep_us(2);
    }
    
    restore_interrupts(interrupts); 
    gpio_put(NEGCON_PIN_CMD, 1); 
    sleep_us(20); 
    return rx;
}

uint16_t apply_steering_curve(uint8_t twist_raw) {
    // 【修正1】ねじりの向きを反転 (以前の 255 - twist_raw を撤廃)
    int32_t raw = twist_raw; 
    int32_t x = raw - 127;         

    const int32_t DEADZONE = 4;

    if (abs(x) < DEADZONE) {
        return 32768; // センター
    }

    int32_t sign = (x > 0) ? 1 : -1;
    int32_t nx = abs(x) - DEADZONE;          
    int32_t max_nx = 127 - DEADZONE;         

    // 限界値をはみ出さないように安全対策
    if (nx > max_nx) nx = max_nx;

    // 【修正2】中心感度を高くするカーブ計算 (0〜10000の精度で計算)
    int32_t t_scaled = (nx * 10000) / max_nx;
    // y = 2t - t^2 (立ち上がりが鋭く、後半なだらかになるカーブ)
    int32_t curve_scaled = (2 * t_scaled) - ((t_scaled * t_scaled) / 10000);

    // 【修正3】アンチデッドゾーンの適用
    // x360ceの画像にあった「3277」を正確に適用し、ねじり始めの遊びを消去
    const int32_t ANTI_DEADZONE = 3277;
    const int32_t MAX_AMPLITUDE = 32767 - ANTI_DEADZONE;

    // 最終的な振幅を計算し、センター(32768)に足し引きする
    int32_t output_amplitude = ANTI_DEADZONE + ((curve_scaled * MAX_AMPLITUDE) / 10000);
    int32_t output = 32768 + (output_amplitude * sign);
    
    // 丸め誤差のクリッピング
    if (output < 0) output = 0;
    if (output > 65535) output = 65535;

    return (uint16_t)output;
}

void NeGconInput::process() {
    Gamepad* gamepad = Storage::getInstance().GetGamepad();

    static uint32_t last_poll_time = 0;
    static uint8_t c_data1 = 0xFF;
    static uint8_t c_data2 = 0xFF;
    static uint8_t c_twist = 127;
    static uint8_t c_btn_i = 0;
    static uint8_t c_btn_ii = 0;
    static uint8_t c_btn_l = 128;
    static bool c_connected = false;

    uint32_t current_time = time_us_32();
    
    if (current_time - last_poll_time >= 16000) { 
        last_poll_time = current_time;

        gpio_put(NEGCON_PIN_ATT, 0);
        sleep_us(50); 
        spi_transfer(0x01);
        uint8_t id = spi_transfer(0x42);

        if (id == 0x23) {
            c_connected = true;
            spi_transfer(0x00);
            c_data1 = spi_transfer(0x00);
            c_data2 = spi_transfer(0x00);
            c_twist = spi_transfer(0x00);
            c_btn_i = spi_transfer(0x00);
            c_btn_ii = spi_transfer(0x00);
            c_btn_l = spi_transfer(0x00); 
            spi_transfer(0x00); 
        }
        
        gpio_put(NEGCON_PIN_ATT, 1);
    }

    if (c_connected) {
        // 【修正】有識者のアドバイスに完全準拠し、dpad と dpadOriginal の両方にデータを書き込む
        uint8_t d = 0;
        if (!(c_data1 & 0x10)) d |= GAMEPAD_MASK_UP;
        if (!(c_data1 & 0x20)) d |= GAMEPAD_MASK_RIGHT;
        if (!(c_data1 & 0x40)) d |= GAMEPAD_MASK_DOWN;
        if (!(c_data1 & 0x80)) d |= GAMEPAD_MASK_LEFT;

        // ゴミデータを完全に消去してから純粋な入力を代入
        gamepad->state.dpad &= ~GAMEPAD_MASK_DPAD;
        gamepad->state.dpad |= d;

        gamepad->state.dpadOriginal &= ~GAMEPAD_MASK_DPAD;
        gamepad->state.dpadOriginal |= d;
        
        // デジタルボタン
        if (!(c_data1 & 0x08)) gamepad->state.buttons |= GAMEPAD_MASK_S2; 
        if (!(c_data2 & 0x10)) gamepad->state.buttons |= GAMEPAD_MASK_B2; 
        if (!(c_data2 & 0x20)) gamepad->state.buttons |= GAMEPAD_MASK_B1; 
        if (!(c_data2 & 0x08)) gamepad->state.buttons |= GAMEPAD_MASK_R1; 

        gamepad->hasAnalogTriggers = true;
        gamepad->hasLeftAnalogStick = true;
        gamepad->hasRightAnalogStick = true;

        gamepad->state.lx = apply_steering_curve(c_twist); 
        gamepad->state.rt = c_btn_i * 257;
        gamepad->state.lt = c_btn_ii * 257;
        gamepad->state.ry = 32768 + (c_btn_l * 128); 
        
        gamepad->state.ly = 32768; 
        gamepad->state.rx = 32768; 
    }
}
