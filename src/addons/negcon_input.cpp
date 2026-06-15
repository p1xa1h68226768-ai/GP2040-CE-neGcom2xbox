#include "addons/negcon_input.h"
#include "storagemanager.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

// NeGcon SPI communication: Hardware SPI instance
// NOTE: Pin assignments must be valid for the selected SPI instance.
//   SPI0: SCK=GP2/6/18/22, TX(MOSI)=GP3/7/19/23, RX(MISO)=GP0/4/16/20
//   SPI1: SCK=GP10/14, TX(MOSI)=GP11/15, RX(MISO)=GP8/12
#ifndef NEGCON_SPI_INSTANCE
#define NEGCON_SPI_INSTANCE spi0
#endif

// RP2040 hardware SPI only supports MSB-first.
// NeGcon (PlayStation SPI) uses LSB-first, so bit reversal is required.
static inline uint8_t reverse_bits(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

bool NeGconInput::available() {
    return true;
}

void NeGconInput::setup() {
    // Hardware SPI init: PlayStation SPI = Mode 3 (CPOL=1, CPHA=1), 250kHz
    spi_init(NEGCON_SPI_INSTANCE, 250000);
    spi_set_format(NEGCON_SPI_INSTANCE, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // Configure SPI function pins
    gpio_set_function(NEGCON_PIN_CLK, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(NEGCON_PIN_CMD, GPIO_FUNC_SPI);    // TX (MOSI)
    gpio_set_function(NEGCON_PIN_DAT, GPIO_FUNC_SPI);    // RX (MISO)
    gpio_pull_up(NEGCON_PIN_DAT);

    // ATT (chip select) is manually controlled as GPIO
    gpio_init(NEGCON_PIN_ATT);
    gpio_set_dir(NEGCON_PIN_ATT, GPIO_OUT);
    gpio_put(NEGCON_PIN_ATT, 1);
}

uint8_t NeGconInput::spi_transfer(uint8_t data) {
    // LSB-first <-> MSB-first conversion
    uint8_t tx = reverse_bits(data);
    uint8_t rx = 0;

    // Full-duplex 1-byte transfer via hardware SPI
    // Does NOT disable interrupts, so USB (TinyUSB) communication is unaffected
    spi_write_read_blocking(NEGCON_SPI_INSTANCE, &tx, &rx, 1);

    sleep_us(20);  // Inter-byte delay for NeGcon protocol
    return reverse_bits(rx);
}

// Steering curve: non-linear mapping of twist input to analog stick value
static uint16_t apply_steering_curve(uint8_t twist_raw) {
    int32_t raw = twist_raw; 
    int32_t x = raw - 127;         

    const int32_t DEADZONE = 4;

    if (abs(x) < DEADZONE) {
        return 32768; // Center
    }

    int32_t sign = (x > 0) ? 1 : -1;
    int32_t nx = abs(x) - DEADZONE;          
    int32_t max_nx = 127 - DEADZONE;         

    // Clamp to prevent exceeding limits
    if (nx > max_nx) nx = max_nx;

    // Sensitivity curve calculation (precision 0-10000)
    int32_t t_scaled = (nx * 10000) / max_nx;
    // y = 2t - t^2 (sharp rise at start, gradual at end)
    int32_t curve_scaled = (2 * t_scaled) - ((t_scaled * t_scaled) / 10000);

    // Anti-deadzone application
    // Apply "3277" value from x360ce to eliminate initial play in twist
    const int32_t ANTI_DEADZONE = 3277;
    const int32_t MAX_AMPLITUDE = 32767 - ANTI_DEADZONE;

    // Calculate final amplitude and offset from center (32768)
    int32_t output_amplitude = ANTI_DEADZONE + ((curve_scaled * MAX_AMPLITUDE) / 10000);
    int32_t output = 32768 + (output_amplitude * sign);
    
    // Clipping for rounding errors
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
        } else {
            c_connected = false;
        }
        
        gpio_put(NEGCON_PIN_ATT, 1);
    }

    if (c_connected) {
        // D-Pad
        uint8_t d = 0;
        if (!(c_data1 & 0x10)) d |= GAMEPAD_MASK_UP;
        if (!(c_data1 & 0x20)) d |= GAMEPAD_MASK_RIGHT;
        if (!(c_data1 & 0x40)) d |= GAMEPAD_MASK_DOWN;
        if (!(c_data1 & 0x80)) d |= GAMEPAD_MASK_LEFT;

        // Clear stale data, then apply fresh input
        gamepad->state.dpad &= ~GAMEPAD_MASK_DPAD;
        gamepad->state.dpad |= d;

        gamepad->state.dpadOriginal &= ~GAMEPAD_MASK_DPAD;
        gamepad->state.dpadOriginal |= d;
        
        // Digital buttons: clear managed buttons first, then set active ones
        gamepad->state.buttons &= ~(GAMEPAD_MASK_S2 | GAMEPAD_MASK_B2 | GAMEPAD_MASK_B1 | GAMEPAD_MASK_R1);
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
        gamepad->state.ry = 32768 + (c_btn_l * 128);  // Intentional: L button mapped to RY axis, neutral when idle
        
        gamepad->state.ly = 32768; 
        gamepad->state.rx = 32768; 
    }
}
