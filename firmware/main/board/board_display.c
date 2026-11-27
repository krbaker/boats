// The boards with a screen: see board.h. The Waveshare BSP does it all.

#include "board.h"

#include "bsp/esp-bsp.h"

esp_err_t board_sdcard_mount(void)
{
    return bsp_sdcard_mount();
}

esp_err_t board_usb_host_start(void)
{
    return bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
}

bool board_ap_requested(void)
{
    return false;
}
