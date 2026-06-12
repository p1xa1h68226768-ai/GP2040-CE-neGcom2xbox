#include "addons/negcon_input.h"
#include "drivermanager.h"
#include "storage.h"

bool NeGconInput::available() {
    // 常にこのアドオンを有効化する
    return true;
}

void NeGconInput::setup() {
    // ピンの初期化
    gpio_init(NEGCON_PIN_DAT);
    gpio_set_dir(NEGCON_PIN_DAT, GPIO_IN);
    gpio_pull_up(NEGCON_PIN_DAT); // Data線はプルアップ必須

    gpio_init(NEGCON_PIN_CMD);
    gpio_set_dir(NEGCON_PIN_CMD, GPIO_OUT);
    
    gpio_init(NEGCON_PIN_ATT);
    gpio_set_dir(NEGCON_PIN_ATT, GPIO_OUT);
    gpio_put(NEGCON_PIN_ATT, 1); // 待機時はHigh
    
    gpio_init(NEGCON_PIN_CLK);
    gpio_set_dir(NEGCON_PIN_CLK, GPIO_OUT);
    gpio_put(NEGCON_PIN_CLK, 1); // クロックはHigh待機(Mode 3)
}

// ソフトウェアSPI通信 (ビットバンギング LSB First)
uint8_t NeGconInput::spi_transfer(uint8_t data) {
    uint8_t rx = 0;
    for(int i = 0; i < 8; i++) {
        gpio_put(NEGCON_PIN_CMD, (data & (1 << i)) ? 1 : 0);
        sleep_us(2); // PS1の約250kHzクロックに合わせる
        gpio_put(NEGCON_PIN_CLK, 0); // クロック立ち下がり
        sleep_us(2);
        if(gpio_get(NEGCON_PIN_DAT)) {
            rx |= (1 << i); // データ読み取り
        }
        gpio_put(NEGCON_PIN_CLK, 1); // クロック立ち上がり
        sleep_us(2);
    }
    return rx;
}

void NeGconInput::process() {
    Gamepad* gamepad = Storage::getInstance().GetGamepad();

    gpio_put(NEGCON_PIN_ATT, 0); // 通信開始 (CS Low)
    sleep_us(10);

    spi_transfer(0x01); // Startコマンド
    uint8_t id = spi_transfer(0x42); // リクエスト送信＆ID受信

    if (id == 0x23) { // 0x23 = ネジコン接続確認
        spi_transfer(0x00); // 0x5A(ACK)を空読み
        
        uint8_t data1 = spi_transfer(0x00); // デジタルボタン1
        uint8_t data2 = spi_transfer(0x00); // デジタルボタン2
        uint8_t twist = spi_transfer(0x00); // ステアリング(捻り)
        uint8_t btn_i = spi_transfer(0x00); // Iボタン(アナログ)
        uint8_t btn_ii = spi_transfer(0x00); // IIボタン(アナログ)
        uint8_t btn_l = spi_transfer(0x00); // Lボタン(アナログ)

        // --- XInputへのマッピング処理 ---
        
        // 1. デジタルボタン (Active Lowなので ~ で反転して判定)
        if (~data1 & 0x10) gamepad->state.dpad |= GAMEPAD_MASK_UP;
        if (~data1 & 0x20) gamepad->state.dpad |= GAMEPAD_MASK_RIGHT;
        if (~data1 & 0x40) gamepad->state.dpad |= GAMEPAD_MASK_DOWN;
        if (~data1 & 0x80) gamepad->state.dpad |= GAMEPAD_MASK_LEFT;
        if (~data1 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_S2; // START

        if (~data2 & 0x10) gamepad->state.buttons |= GAMEPAD_MASK_B2; // Aボタン
        if (~data2 & 0x20) gamepad->state.buttons |= GAMEPAD_MASK_B1; // Bボタン
        if (~data2 & 0x08) gamepad->state.buttons |= GAMEPAD_MASK_R1; // Rボタン

        // 2. ステアリング (左0xFF〜右0x00) -> 左スティックX軸(0x0000〜0xFFFF)へ変換
        // ネジコンは右が0なので、反転させてスケーリングする
        gamepad->state.lx = (0xFF - twist) * 257; 

        // 3. アナログボタン (Iをアクセル[RT]、IIをブレーキ[LT]に割当)
        gamepad->hasAnalogTriggers = true;
        gamepad->state.rt = btn_i;
        gamepad->state.lt = btn_ii;

        // Lボタン(アナログ)の処理が必要ならここに追加
    }

    gpio_put(NEGCON_PIN_ATT, 1); // 通信終了 (CS High)
}
