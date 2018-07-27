/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "button_hal.h"
#include "nrfx_timer.h"
#include "platform_config.h"
#include "interrupts_hal.h"
#include "pinmap_impl.h"
#include "nrfx_gpiote.h"
#include "gpio_hal.h"
#include "core_hal.h"
#include "logging.h"

#define TIMERx_ID               2
#define TIMERx_IRQ_PRIORITY     APP_IRQ_PRIORITY_MID

// 16bit counter, 125KHz clock, interrupt interval: 10ms
#define TIMERx_BIT_WIDTH        NRF_TIMER_BIT_WIDTH_16
#define TIMERx_FREQUENCY        NRF_TIMER_FREQ_125kHz

const nrfx_timer_t m_button_timer = NRFX_TIMER_INSTANCE(TIMERx_ID);


button_config_t HAL_Buttons[] = {
    {
        .active         = false,
        .pin            = BUTTON1_PIN,
        .interrupt_mode = BUTTON1_INTERRUPT_MODE,
        .debounce_time  = 0,
    },
    {
        .active         = false,
        .pin            = BUTTON1_MIRROR_PIN,
        .interrupt_mode = BUTTON1_MIRROR_INTERRUPT_MODE,
        .debounce_time  = 0
    }
};

static void mode_button_reset(uint16_t button)
{
    HAL_Buttons[button].debounce_time = 0x00;

    if (!HAL_Buttons[BUTTON1].active && !HAL_Buttons[BUTTON1_MIRROR].active) 
    {
        nrfx_timer_disable(&m_button_timer);
        nrfx_timer_clear(&m_button_timer);
    }

    HAL_Notify_Button_State((Button_TypeDef)button, false); 

    /* Enable Button Interrupt */
    BUTTON_EXTI_Config((Button_TypeDef)button, ENABLE);
}

void button_timer_event_handler(nrf_timer_event_t event_type, void* p_context)
{
    if (event_type == NRF_TIMER_EVENT_COMPARE0)
    {
        if (HAL_Buttons[BUTTON1].active && (BUTTON_GetState(BUTTON1) == BUTTON1_PRESSED))
        {
            if (!HAL_Buttons[BUTTON1].debounce_time)
            {
                HAL_Buttons[BUTTON1].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
                HAL_Notify_Button_State(BUTTON1, true); 
            }
            HAL_Buttons[BUTTON1].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
        }
        else if (HAL_Buttons[BUTTON1].active)
        {
            HAL_Buttons[BUTTON1].active = false;
            mode_button_reset(BUTTON1);
        }

        if ((HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) && HAL_Buttons[BUTTON1_MIRROR].active &&
            BUTTON_GetState(BUTTON1_MIRROR) == (HAL_Buttons[BUTTON1_MIRROR].interrupt_mode == RISING ? 1 : 0)) 
        {
            if (!HAL_Buttons[BUTTON1_MIRROR].debounce_time)
            {
                HAL_Buttons[BUTTON1_MIRROR].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
                HAL_Notify_Button_State(BUTTON1_MIRROR, true);
            }
            HAL_Buttons[BUTTON1_MIRROR].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
        }
        else if ((HAL_Buttons[BUTTON1_MIRROR].pin != PIN_INVALID) && HAL_Buttons[BUTTON1_MIRROR].active)
        {
            HAL_Buttons[BUTTON1_MIRROR].active = false;
            mode_button_reset(BUTTON1_MIRROR);
        }
    }
}

void BUTTON_Interrupt_Handler(void *data)
{
    uint32_t button = (uint32_t)data;

    HAL_Buttons[button].debounce_time = 0x00;
    HAL_Buttons[button].active = true;

    /* Disable button Interrupt */
    BUTTON_EXTI_Config(button, DISABLE);

    /* Start timer */
    nrfx_timer_enable(&m_button_timer);
}

/**
 * @brief  Configures Button GPIO, EXTI Line and DEBOUNCE Timer.
 * @param  Button: Specifies the Button to be configured.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON1: Button1
 * @param  Button_Mode: Specifies Button mode.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON_MODE_GPIO: Button will be used as simple IO
 *     @arg BUTTON_MODE_EXTI: Button will be connected to EXTI line with interrupt
 *                     generation capability
 * @retval None
 */
void BUTTON_Init(Button_TypeDef Button, ButtonMode_TypeDef Button_Mode)
{
    HAL_Pin_Mode(HAL_Buttons[Button].pin, BUTTON1_PIN_MODE);

    if (Button_Mode == BUTTON_MODE_EXTI)
    {
        nrfx_timer_config_t timer_cfg = {
            .frequency          = TIMERx_FREQUENCY,
            .mode               = NRF_TIMER_MODE_TIMER,          
            .bit_width          = TIMERx_BIT_WIDTH,
            .interrupt_priority = TIMERx_IRQ_PRIORITY,                    
            .p_context          = NULL                                                       
        };
        SPARK_ASSERT(nrfx_timer_init(&m_button_timer, &timer_cfg, button_timer_event_handler) == NRF_SUCCESS);

        uint32_t time_ticks = nrfx_timer_ms_to_ticks(&m_button_timer, BUTTON_DEBOUNCE_INTERVAL);
        nrfx_timer_extended_compare(&m_button_timer, NRF_TIMER_CC_CHANNEL0, time_ticks, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);

        /* Attach GPIOTE Interrupt */
        BUTTON_EXTI_Config(Button, ENABLE);
    }
}

void BUTTON_EXTI_Config(Button_TypeDef Button, FunctionalState NewState)
{
    HAL_InterruptExtraConfiguration config = {0};
    config.version = HAL_INTERRUPT_EXTRA_CONFIGURATION_VERSION;
    config.keepHandler = false;
    config.flags = HAL_DIRECT_INTERRUPT_FLAG_NONE;

    if (NewState == ENABLE)
    {
        HAL_Interrupts_Attach(HAL_Buttons[Button].pin, BUTTON_Interrupt_Handler, (void *)((int)Button), FALLING, &config); 
    }
    else
    {
        HAL_Interrupts_Detach(HAL_Buttons[Button].pin);
    }
}

/**
 * @brief  Returns the selected Button non-filtered state.
 * @param  Button: Specifies the Button to be checked.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON1: Button1
 * @retval Actual Button Pressed state.
 */
uint8_t BUTTON_GetState(Button_TypeDef Button)
{
    return HAL_GPIO_Read(HAL_Buttons[Button].pin);
}

/**
 * @brief  Returns the selected Button Debounced Time.
 * @param  Button: Specifies the Button to be checked.
 *   This parameter can be one of following parameters:
 *     @arg BUTTON1: Button1
 * @retval Button Debounced time in millisec.
 */
uint16_t BUTTON_GetDebouncedTime(Button_TypeDef Button)
{
    return HAL_Buttons[Button].debounce_time;
}

void BUTTON_ResetDebouncedState(Button_TypeDef Button)
{
    HAL_Buttons[Button].debounce_time = 0;
}

void BUTTON_Check_State(uint16_t button, uint8_t pressed)
{
    if (BUTTON_GetState(button) == pressed)
    {
        if (!HAL_Buttons[button].active)
            HAL_Buttons[button].active = true;
        HAL_Buttons[button].debounce_time += BUTTON_DEBOUNCE_INTERVAL;
    }
    else if (HAL_Buttons[button].active)
    {
        HAL_Buttons[button].active = false;
        /* Enable button Interrupt */
        BUTTON_EXTI_Config(button, ENABLE);
    }
}