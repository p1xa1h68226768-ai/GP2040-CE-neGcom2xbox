#ifndef NEGCON_INPUT_H_
#define NEGCON_INPUT_H_

#include "gpaddon.h"
#include "gamepad.h"

// ネジコンのピン設定
#define NEGCON_PIN_DAT 0
#define NEGCON_PIN_CMD 1
#define NEGCON_PIN_ATT 2
#define NEGCON_PIN_CLK 3

// システムのお約束（GPAddon）に従う宣言
class NeGconInput : public GPAddon {
public:
    virtual bool available() override;
    virtual void setup() override;
    virtual void process() override;
    virtual void preprocess() override {}
    // ↓これが足りないと怒られていた「名前」の宣言です
    virtual std::string name() override { return "NeGconInput"; }

private:
    uint8_t spi_transfer(uint8_t data);
};

#endif // NEGCON_INPUT_H_
