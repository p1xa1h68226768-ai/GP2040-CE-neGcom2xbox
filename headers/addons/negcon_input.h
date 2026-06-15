#ifndef NEGCON_INPUT_H_
#define NEGCON_INPUT_H_

#include <string>
#include <stdint.h>
#include "gpaddon.h"
#include "gamepad.h"

// NeGcon pin assignments
// Matched to RP2040 SPI0 hardware function mapping:
//   GP0 = SPI0 RX  (MISO) -> DAT
//   GP1 = GPIO     (CS)   -> ATT (manual chip select)
//   GP2 = SPI0 SCK        -> CLK
//   GP3 = SPI0 TX  (MOSI) -> CMD
#define NEGCON_PIN_DAT 0
#define NEGCON_PIN_ATT 1
#define NEGCON_PIN_CLK 2
#define NEGCON_PIN_CMD 3

class NeGconInput : public GPAddon {
public:
    virtual bool available();
    virtual void setup();
    virtual void process();
    virtual void preprocess() {}
    virtual void postprocess(bool sent) {}
    virtual void reinit() {}
    virtual std::string name() { return "NeGconInput"; }

private:
    uint8_t spi_transfer(uint8_t data);
};

#endif // NEGCON_INPUT_H_
