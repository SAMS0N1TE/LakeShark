#include <cstdio>
#include <memory>
#include "bus/i2c/software_i2c.h"
#include "bus/spi/hardware_spi.h"
#include "chip/i2c/xl95x5.h"
#include "chip/spi/cc1101.h"
#include "chip/spi/nrf24l01x.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    using namespace cpp_bus_driver;
    vTaskDelay(pdMS_TO_TICKS(2000));
    puts("MIXRF ISOLATION: vendor drivers; no radio transmissions");
    /* Hold every shared-bus CS inactive, including the onboard SX1262. */
    for(int pin : {24,36,54,27}) {
        gpio_set_level((gpio_num_t)pin,1);
        gpio_set_direction((gpio_num_t)pin,GPIO_MODE_OUTPUT);
    }
    gpio_set_level(GPIO_NUM_53,0);
    gpio_set_direction(GPIO_NUM_53,GPIO_MODE_OUTPUT);
    auto i2c=std::make_shared<SoftwareI2c>(46,45);
    Xl95x5 io(i2c,0x20);
    bool power=io.Init();
    printf("ISOLATE expander=%d\n",power);
    power=power && io.GpioWrite(Xl95x5::Pin::kIo0,0) &&
        io.SetGpioMode(Xl95x5::Pin::kIo0,Xl95x5::Mode::kOutput);
    vTaskDelay(pdMS_TO_TICKS(20));
    power=power && io.GpioWrite(Xl95x5::Pin::kIo0,1);
    bool route=io.SetGpioMode(Xl95x5::Pin::kIo1,Xl95x5::Mode::kOutput) &&
        io.SetGpioMode(Xl95x5::Pin::kIo2,Xl95x5::Mode::kOutput) &&
        io.GpioWrite(Xl95x5::Pin::kIo1,1) && io.GpioWrite(Xl95x5::Pin::kIo2,0);
    vTaskDelay(pdMS_TO_TICKS(100));
    printf("ISOLATE enable=%d readback=%u route=%d\n",power,io.GpioRead(Xl95x5::Pin::kIo0),route);
    auto shared=std::make_shared<HardwareSpi>(3,2,4,SPI2_HOST,0);
    auto cc_bus=std::make_shared<HardwareSpi>(shared,0);
    Cc1101 cc(cc_bus,36,4,25,33);
    bool cc_ok=power && cc.Init(4000000);
    printf("ISOLATE CC1101=%d\n",cc_ok);
    if(cc_ok)cc.Sleep();
    auto nrf_bus=std::make_shared<HardwareSpi>(shared,0);
    Nrf24l01x nrf(nrf_bus,54,53,32);
    bool nrf_ok=power && nrf.Init(10000000);
    printf("ISOLATE NRF24=%d\n",nrf_ok);
    if(nrf_ok)nrf.PowerDown();
    for(;;) {
        printf("ISOLATE RESULT enable=%d CC1101=%d NRF24=%d\n",power,cc_ok,nrf_ok);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
