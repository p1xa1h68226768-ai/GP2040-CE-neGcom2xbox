#include "addons/negcon_input.h"
#include "storagemanager.h"

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
        
        // 変数に入れず空読みすることで、通信を維持しつつエラー警告を消滅させます
        spi_transfer(0x00); 

        if (~data1 & 0x10) gamepad->state.dpad |= GAMEPAD_MASK_UP;
        if (~data1 & 0x20) gamepad->state.dpad |= GAMEPAD_MASK_RIGHT;
        if (~data1 & 0x40) gamepad->state.dpad |= GAMEPAD_MASK_DOWN;
        if (~data1 & 0x80) gamepad->state.dpad |= GAMEPAD_MASK_LEFT;
        if (~data1 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_S2;

        if (~data2 & 0x10) gamepad->state.buttons |= GAMEPAD_MASK_B2;
        if (~data2 & 0x20) gamepad->state.buttons |= GAMEPAD_MASK_B1;
        if (~data2 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_R1;

        gamepad->state.lx = (0xFF - twist) * 257; 

        gamepad->hasAnalogTriggers = true;
        gamepad->state.rt = btn_i;
        gamepad->state.lt = btn_ii;
    }

    gpio_put(NEGCON_PIN_ATT, 1);
}
