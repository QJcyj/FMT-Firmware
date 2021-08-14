/******************************************************************************
 * Copyright 2020 The Firmament Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include <firmament.h>

#include <board_device.h>
#include <bsp.h>
#include <shell.h>
#include <string.h>

#ifdef FMT_USING_CM_BACKTRACE
#include <cm_backtrace.h>
#endif

#include "board_device.h"
#include "driver/bmi055.h"
#include "driver/gpio.h"
#include "driver/gps_m8n.h"
#include "driver/icm20689.h"
#include "driver/ist8310.h"
#include "driver/ms5611.h"
#include "driver/ncp5623c.h"
#include "drv_adc.h"
#include "drv_gpio.h"
#include "drv_i2c.h"
#include "drv_sdio.h"
#include "drv_spi.h"
#include "drv_systick.h"
#include "drv_usart.h"
#include "drv_usbd_cdc.h"
#include "drv_pwm.h"
#include "led.h"

#include "module/console/console_config.h"
#include "module/controller/controller_model.h"
#include "module/file_manager/file_manager.h"
#include "module/fms/fms_model.h"
#include "module/ins/ins_model.h"
#include "module/mavproxy/mavproxy_config.h"
#include "module/param/param.h"
#include "module/pmu/power_manager.h"
#include "module/sensor/sensor_hub.h"
#include "module/sysio/actuator_cmd.h"
#include "module/sysio/actuator_config.h"
#include "module/sysio/pilot_cmd.h"
#include "module/sysio/pilot_cmd_config.h"
#include "module/task_manager/task_manager.h"
#include "module/toml/toml.h"
#include "module/utils/devmq.h"
#include "module/work_queue/workqueue_manager.h"
#ifdef FMT_USING_SIH
#include "module/plant/plant_model.h"
#endif

#define DEFAULT_TOML_SYS_CONFIG "target = \"Pixhawk4 FMUv5\"\n\
[console]\n\
	[[console.devices]]\n\
	type = \"serial\"\n\
	name = \"serial0\"\n\
	baudrate = 57600\n\
	auto-switch = true\n\
	[[console.devices]]\n\
	type = \"mavlink\"\n\
	name = \"mav_console\"\n\
	auto-switch = true\n\
[mavproxy]\n\
	[[mavproxy.devices]]\n\
	type = \"serial\"\n\
	name = \"serial1\"\n\
	baudrate = 57600\n\
	[[mavproxy.devices]]\n\
	type = \"usb\"\n\
	name = \"usbd0\"\n\
    auto-switch = true"

#define MATCH(a, b)     (strcmp(a, b) == 0)
#define SYS_CONFIG_FILE "/sys/sysconfig.toml"

static const struct dfs_mount_tbl mnt_table[] = {
    { "sd0", "/", "elm", 0, NULL },
    { NULL } /* NULL indicate the end */
};

static toml_table_t* __toml_root_tab = NULL;

static fmt_err_t bsp_parse_toml_sysconfig(toml_table_t* root_tab)
{
    fmt_err_t err = FMT_EOK;
    toml_table_t* sub_tab;
    const char* key;
    const char* raw;
    char* target;
    int i;

    if (root_tab == NULL) {
        return FMT_ERROR;
    }

    /* target should be defined and match with bsp */
    if ((raw = toml_raw_in(root_tab, "target")) != 0) {
        if (toml_rtos(raw, &target) != 0) {
            console_printf("Error: fail to parse type value\n");
            err = FMT_ERROR;
        }
        if (!MATCH(target, TARGET_NAME)) {
            /* check if target match */
            console_printf("Error: target name doesn't match\n");
            err = FMT_ERROR;
        }
        rt_free(target);
    } else {
        console_printf("Error: can not find target key\n");
        err = FMT_ERROR;
    }

    if (err == FMT_EOK) {
        /* traverse all sub-table */
        for (i = 0; 0 != (key = toml_key_in(root_tab, i)); i++) {
            /* handle all sub tables */
            if (0 != (sub_tab = toml_table_in(root_tab, key))) {
                if (MATCH(key, "console")) {
                    err = console_toml_config(sub_tab);
                } else if (MATCH(key, "mavproxy")) {
                    err = mavproxy_toml_config(sub_tab);
                } else if (MATCH(key, "pilot-cmd")) {
                    err = pilot_cmd_toml_config(sub_tab);
                } else if (MATCH(key, "actuator")) {
                    err = actuator_toml_config(sub_tab);
                }else {
                    console_printf("unknown table: %s\n", key);
                }
                if (err != FMT_EOK) {
                    console_printf("fail to parse %s\n", key);
                }
            }
        }
    }

    /* free toml root table */
    toml_free(root_tab);

    return err;
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
    console_printf("Enter Error_Handler\n");
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1) {
    }
    /* USER CODE END Error_Handler_Debug */
}

/*
* When enabling the D-cache there is cache coherency issue. 
* This matter crops up when multiple masters (CPU, DMAs...) 
* share the memory. If the CPU writes something to an area 
* that has a write-back cache attribute (example SRAM), the 
* write result is not seen on the SRAM as the access is 
* buffered, and then if the DMA reads the same memory area 
* to perform a data transfer, the values read do not match 
* the intended data. The issue occurs for DMA read as well.
* Currently not all drivers can ensure the data coherency 
* when D-Cache enabled, so disable it by default.
*/
/**
  * @brief  CPU L1-Cache enable.
  * @param  None
  * @retval None
  */
static void CPU_CACHE_Enable(void)
{
    /* Enable I-Cache */
    SCB_EnableICache();

    /* Enable D-Cache */
    // SCB_EnableDCache();
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_7);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_7) {
    }
    LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
    LL_PWR_EnableOverDriveMode();
    LL_RCC_HSE_Enable();

    /* Wait till HSE is ready */
    while (LL_RCC_HSE_IsReady() != 1) {
    }
    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_8, 216, LL_RCC_PLLP_DIV_2);
    LL_RCC_PLL_ConfigDomain_48M(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_8, 216, LL_RCC_PLLQ_DIV_9);
    LL_RCC_PLL_Enable();

    /* Wait till PLL is ready */
    while (LL_RCC_PLL_IsReady() != 1) {
    }
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_4);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_2);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);

    /* Wait till System clock is ready */
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {
    }
    LL_SetSystemCoreClock(216000000);

    /* Update the time base */
    if (HAL_InitTick(TICK_INT_PRIORITY) != HAL_OK) {
        Error_Handler();
    }
    LL_RCC_SetCK48MClockSource(LL_RCC_CK48M_CLKSOURCE_PLL);
    LL_RCC_SetUSBClockSource(LL_RCC_USB_CLKSOURCE_PLL);
    LL_RCC_SetSDMMCClockSource(LL_RCC_SDMMC1_CLKSOURCE_PLL48CLK);
    LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
    LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
    LL_RCC_SetUSARTClockSource(LL_RCC_USART3_CLKSOURCE_PCLK1);
    LL_RCC_SetUSARTClockSource(LL_RCC_USART6_CLKSOURCE_PCLK2);
    LL_RCC_SetUARTClockSource(LL_RCC_UART7_CLKSOURCE_PCLK1);
    LL_RCC_SetI2CClockSource(LL_RCC_I2C1_CLKSOURCE_PCLK1);
    LL_RCC_SetI2CClockSource(LL_RCC_I2C2_CLKSOURCE_PCLK1);
    LL_RCC_SetI2CClockSource(LL_RCC_I2C3_CLKSOURCE_PCLK1);
    LL_RCC_SetI2CClockSource(LL_RCC_I2C4_CLKSOURCE_PCLK1);
}

#define ITEM_LENGTH 42
void bsp_show_information(void)
{
    char buffer[50];

    console_printf("\n");
    console_println("   _____                               __ ");
    console_println("  / __(_)_____ _  ___ ___ _  ___ ___  / /_");
    console_println(" / _// / __/  ' \\/ _ `/  ' \\/ -_) _ \\/ __/");
    console_println("/_/ /_/_/ /_/_/_/\\_,_/_/_/_/\\__/_//_/\\__/ ");

    sprintf(buffer, "FMT FMU %s", FMT_VERSION);
    print_item_line("Firmware", buffer, '.', ITEM_LENGTH);
    sprintf(buffer, "RT-Thread v%ld.%ld.%ld", RT_VERSION, RT_SUBVERSION, RT_REVISION);
    print_item_line("Kernel", buffer, '.', ITEM_LENGTH);
    sprintf(buffer, "%d KB", SYSTEM_TOTAL_MEM_SIZE / 1024);
    print_item_line("RAM", buffer, '.', ITEM_LENGTH);
    print_item_line("Target", TARGET_NAME, '.', ITEM_LENGTH);
    print_item_line("Vehicle", VEHICLE_TYPE, '.', ITEM_LENGTH);
    print_item_line("INS Model", (char*)INS_EXPORT.model_info, '.', ITEM_LENGTH);
    print_item_line("FMS Model", (char*)FMS_EXPORT.model_info, '.', ITEM_LENGTH);
    print_item_line("Control Model", (char*)CONTROL_EXPORT.model_info, '.', ITEM_LENGTH);
#ifdef FMT_USING_SIH
    print_item_line("Plant Model", (char*)PLANT_EXPORT.model_info, '.', ITEM_LENGTH);
#endif

    console_println("Task Initialize:");
    fmt_task_desc_t task_tab = get_task_table();
    for (uint32_t i = 0; i < get_task_num(); i++) {
        sprintf(buffer, "  %s", task_tab[i].name);
        /* task status must be okay to reach here */
        print_item_line(buffer, task_tab[i].status == TASK_OK ? "OK" : "Fail", '.', ITEM_LENGTH);
    }
}

/* this function will be called before rtos start, which is not in the thread context */
void bsp_early_initialize(void)
{
    /* Enable CPU L1-cache */
    CPU_CACHE_Enable();

    /* HAL library initialization */
    HAL_Init();

    /* System clock initialization */
    SystemClock_Config();

    /* systick driver init */
    RT_CHECK(drv_systick_init());

    /* usart driver init */
    RT_CHECK(drv_usart_init());

    /* system time module init */
    FMT_CHECK(systime_init());

    /* init console to enable console output */
    FMT_CHECK(console_init());

    /* gpio driver init */
    RT_CHECK(drv_gpio_init());

    /* spi driver init */
    RT_CHECK(spi_drv_init());

    /* i2c driver init */
    RT_CHECK(drv_i2c_init());

    /* pwm driver init */
    RT_CHECK(drv_pwm_init());

    /* system statistic module */
    FMT_CHECK(sys_stat_init());
}

/* this function will be called after rtos start, which is in thread context */
void bsp_initialize(void)
{
    /* start recording boot log */
    FMT_CHECK(boot_log_init());

    /* init uMCN */
    FMT_CHECK(mcn_init());

    /* create workqueue */
    FMT_CHECK(workqueue_manager_init());

    /* init storage devices */
    RT_CHECK(drv_sdio_init());
    /* init file system */
    FMT_CHECK(file_manager_init(mnt_table));

    /* init usbd_cdc */
    RT_CHECK(drv_usb_cdc_init());

    /* adc driver init */
    RT_CHECK(drv_adc_init());

    RT_CHECK(drv_icm20689_init());

    RT_CHECK(ms5611_drv_init(MS5611_SPI_DEVICE_NAME));

    RT_CHECK(drv_bmi055_init());

    /* ist8310 and ncp5623c are on gps module and possibly it is not connected */
    drv_ist8310_init("i2c1_dev1");
    drv_ncp5623c_init("i2c1_dev2");

    RT_CHECK(gps_m8n_init("serial3"));

    /* init parameter system */
    FMT_CHECK(param_init());

    /* register sensor to sensor hub */
    FMT_CHECK(register_sensor_imu("gyro0", "accel0", 0));
    if (rt_device_find("mag0") != NULL) {
        FMT_CHECK(register_sensor_mag("mag0", 0));
    }
    FMT_CHECK(register_sensor_barometer("barometer"));

    /* init finsh */
    finsh_system_init();
    /* Mount finsh to console after finsh system init */
    FMT_CHECK(console_enable_input());

#ifdef FMT_USING_CM_BACKTRACE
    /* cortex-m backtrace */
    cm_backtrace_init("fmt_fmu-v5", TARGET_NAME, FMT_VERSION);
#endif
}

void bsp_post_initialize(void)
{
    FMT_CHECK(pilot_cmd_init());

    /* toml system configure */
    __toml_root_tab = toml_parse_config_file(SYS_CONFIG_FILE);
    if (!__toml_root_tab) {
        /* use default system configuration */
        __toml_root_tab = toml_parse_config_string(DEFAULT_TOML_SYS_CONFIG);
    }
    FMT_CHECK(bsp_parse_toml_sysconfig(__toml_root_tab));

#if defined(FMT_HIL_WITH_ACTUATOR) || !defined(FMT_USING_HIL)
    /* init actuator */
    FMT_CHECK(actuator_init());
#endif

    /* start device message queue work */
    FMT_CHECK(devmq_start_work());

    /* initialize led */
    FMT_CHECK(led_control_init());

    /* initialize power management unit */
    FMT_CHECK(pmu_init());

    /* show system information */
    bsp_show_information();

    /* dump boot log to file */
    boot_log_dump();
}

/**
 * This function will initial STM32 board.
 */
void rt_hw_board_init()
{
    bsp_early_initialize();
}