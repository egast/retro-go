// Target definition
#define RG_TARGET_NAME "ESP32-S3-TOUCH-LCD-1.9"

// Storage
#define RG_STORAGE_ROOT "/sd"
// #define RG_STORAGE_SDSPI_HOST  SPI3_HOST
// #define RG_STORAGE_SDSPI_SPEED SDMMC_FREQ_DEFAULT
#define RG_STORAGE_SDMMC_HOST  SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_FLASH_PARTITION  "vfs"

// Audio
#define RG_AUDIO_USE_INT_DAC 0 // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC 0 // 0 = Disable, 1 = Enable

// Video
#define RG_SCREEN_DRIVER       0 // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST         SPI2_HOST
#define RG_SCREEN_SPEED        SPI_MASTER_FREQ_40M // SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT    1
#define RG_SCREEN_WIDTH        320
#define RG_SCREEN_HEIGHT       170
#define RG_SCREEN_ROTATE       0
#define RG_SCREEN_VISIBLE_AREA {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA    {0, 0, 0, 0}
#define RG_SCREEN_INIT()                                                                            \
    ILI9341_CMD(0x36, 0x70)                                                                         \
    ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33); /* Porch setting (front/back porch) */         \
    ILI9341_CMD(0xB7, 0x35);                         /* Gate control (VGH/VGL) */                   \
    ILI9341_CMD(0xBB, 0x13);                         /* VCOMS setting (~0.725V) */                  \
    ILI9341_CMD(0xC0, 0x2C);                         /* LCM control (VRH) */                        \
    ILI9341_CMD(0xC2, 0x01);                         /* VDV/VRH enable */                           \
    ILI9341_CMD(0xC3, 0x0B);                         /* VHR setting */                              \
    ILI9341_CMD(0xC4, 0x20);                         /* VDV setting */                              \
    ILI9341_CMD(0xC6, 0x0F);                         /* Frame rate control (~60Hz) */               \
    ILI9341_CMD(0xD0, 0xA4, 0xA1);                   /* Power control 1 / AVDD/AVCL */              \
    ILI9341_CMD(0xD6, 0xA1);                         /* Power control 3 (HBM) */                    \
    ILI9341_CMD(0xE0, 0x00, 0x03, 0x07, 0x08, 0x07, 0x15, 0x2A, 0x44, 0x42, 0x0A, 0x17, 0x18, 0x25, \
                0x27); /* Positive gamma */                                                         \
    ILI9341_CMD(0xE1, 0x00, 0x03, 0x08, 0x07, 0x07, 0x23, 0x2A, 0x43, 0x42, 0x09, 0x18, 0x17, 0x25, \
                0x27);       /* Negative gamma */                                                   \
    ILI9341_CMD(0x21, 0x21); /* Display inversion ON (needed for IPS panel) */                      \
    ILI9341_CMD(0x11, 0x11); /* Sleep OUT */                                                        \
    ILI9341_CMD(0x29, 0x29);


#define RG_GAMEPAD_GPIO_MAP                                           \
    {                                                                 \
        {RG_KEY_UP,     .num = GPIO_NUM_1,  .pullup = 1, .level = 0}, \
        {RG_KEY_RIGHT,  .num = GPIO_NUM_38, .pullup = 1, .level = 0}, \
        {RG_KEY_DOWN,   .num = GPIO_NUM_42, .pullup = 1, .level = 0}, \
        {RG_KEY_LEFT,   .num = GPIO_NUM_45, .pullup = 1, .level = 0}, \
        {RG_KEY_SELECT, .num = GPIO_NUM_16, .pullup = 1, .level = 0}, \
        {RG_KEY_START,  .num = GPIO_NUM_19, .pullup = 1, .level = 0}, \
        {RG_KEY_MENU,   .num = GPIO_NUM_18, .pullup = 1, .level = 0}, \
        {RG_KEY_OPTION, .num = GPIO_NUM_6,  .pullup = 1, .level = 0}, \
        {RG_KEY_A,      .num = GPIO_NUM_15, .pullup = 1, .level = 0}, \
        {RG_KEY_B,      .num = GPIO_NUM_5,  .pullup = 1, .level = 0}, \
}

// Battery
#define RG_BATTERY_DRIVER            1
#define RG_BATTERY_ADC_UNIT          ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL       ADC_CHANNEL_3
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3500.f) / (4200.f - 3500.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)


// Status LED
#define RG_GPIO_LED GPIO_NUM_38

// SPI Display (back up working)
#define RG_GPIO_LCD_MISO GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI GPIO_NUM_13
#define RG_GPIO_LCD_CLK  GPIO_NUM_10
#define RG_GPIO_LCD_CS   GPIO_NUM_12
#define RG_GPIO_LCD_DC   GPIO_NUM_11
#define RG_GPIO_LCD_BCKL GPIO_NUM_14
#define RG_GPIO_LCD_RST  GPIO_NUM_9

// I2C BUS
#define RG_GPIO_I2C_SDA GPIO_NUM_47
#define RG_GPIO_I2C_SCL GPIO_NUM_48

// #define RG_GPIO_SDSPI_MISO GPIO_NUM_40
// #define RG_GPIO_SDSPI_MOSI GPIO_NUM_39
// #define RG_GPIO_SDSPI_CLK  GPIO_NUM_41
// #define RG_GPIO_SDSPI_CS   GPIO_NUM_46 // not defined in schema

// External I2S DAC
// #define RG_GPIO_SND_I2S_BCK  41
// #define RG_GPIO_SND_I2S_WS   42
// #define RG_GPIO_SND_I2S_DATA 40
#define RG_GPIO_SND_AMP_ENABLE      18

#define RG_GPIO_SDSPI_CLK 41
#define RG_GPIO_SDSPI_CMD 39
#define RG_GPIO_SDSPI_D0  40