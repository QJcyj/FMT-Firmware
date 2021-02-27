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

#include <bsp.h>
#include <shell.h>
#include <string.h>

#ifdef FMT_USING_CM_BACKTRACE
#include <cm_backtrace.h>
#endif

#include "driver/gpio.h"
#include "driver/usart.h"

#include "drv_gpio.h"
#include "drv_systick.h"

static void _print_line(const char* name, const char* content, uint32_t len)
{
    int pad_len = len - strlen(name) - strlen(content);

    if (pad_len < 1) {
        pad_len = 1;
    }

    console_printf("%s", name);

    while (pad_len--)
        console_write(".", 1);

    console_printf("%s\n", content);
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
    LL_Init1msTick(216000000);
    LL_SetSystemCoreClock(216000000);
    LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
    LL_RCC_SetUARTClockSource(LL_RCC_UART7_CLKSOURCE_PCLK1);
}

void bsp_show_information(void)
{
    char buffer[50];
    uint32_t str_len = 42;

    console_println("   _____                               __ ");
    console_println("  / __(_)_____ _  ___ ___ _  ___ ___  / /_");
    console_println(" / _// / __/  ' \\/ _ `/  ' \\/ -_) _ \\/ __/");
    console_println("/_/ /_/_/ /_/_/_/\\_,_/_/_/_/\\__/_//_/\\__/ ");

    sprintf(buffer, "FMT FMU v%d.%d.%d", FMT_VERSION, FMT_SUBVERSION, FMT_REVISION);
    _print_line("Firmware", buffer, str_len);
    sprintf(buffer, "RT-Thread v%ld.%ld.%ld", RT_VERSION, RT_SUBVERSION, RT_REVISION);
    _print_line("Kernel", buffer, str_len);
    sprintf(buffer, "%d KB", SYSTEM_TOTAL_MEM_SIZE / 1024);
    _print_line("RAM", buffer, str_len);
    _print_line("Target", TARGET_NAME, str_len);
    _print_line("Vehicle", VEHICLE_TYPE, str_len);
}

/* this function will be called before rtos start, which is not in the thread context */
void bsp_early_initialize(void)
{
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

    NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    /* System clock initialization */
    SystemClock_Config();

    /* systick driver init */
    RTT_CHECK(drv_systick_init());
    /* usart driver init */
    RTT_CHECK(usart_drv_init());
    /* system time module init */
    FMT_CHECK(systime_init());

    /* init console to enable console output */
    FMT_CHECK(console_init());

    /* gpio driver init */
    RTT_CHECK(drv_gpio_init());
}

/* this function will be called after rtos start, which is in thread context */
void bsp_initialize(void)
{
    /* init uMCN */
    FMT_CHECK(mcn_init());

#ifdef RT_USING_FINSH
    /* init finsh */
    finsh_system_init();
    /* Mount finsh to console after finsh system init */
    FMT_CHECK(console_enable_shell(NULL));
#endif

#ifdef FMT_USING_CM_BACKTRACE
    /* cortex-m backtrace */
    cm_backtrace_init("fmt_fmu", TARGET_NAME, "v0.1");
#endif
}

void bsp_post_initialize(void)
{
    /* show system information */
    bsp_show_information();
}

/**
 * This function will initial STM32 board.
 */
void rt_hw_board_init()
{
    bsp_early_initialize();
}