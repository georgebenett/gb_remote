#include "led.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "hw_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "LED";
static uint8_t current_duty = 0;
static TaskHandle_t transition_task_handle = NULL;

esp_err_t led_init(void)
{
    // Configure timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_PWM_RESOLUTION,
        .timer_num = LED_PWM_TIMER,
        .freq_hz = LED_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Configure channel
    ledc_channel_config_t ledc_channel = {
        .gpio_num = LED_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LED_PWM_CHANNEL,
        .timer_sel = LED_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // Set initial state (disconnected)
    led_set_connection_state(false);

    ESP_LOGI(TAG, "LED initialized");
    return ESP_OK;
}

void led_set_duty(uint8_t duty)
{
    uint32_t duty_scaled = (duty * ((1 << LED_PWM_RESOLUTION) - 1)) / 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CHANNEL, duty_scaled);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CHANNEL);
    current_duty = duty;
}

static void led_transition_task(void *pvParameters)
{
    uint8_t target_duty = (uint8_t)((uint32_t)pvParameters);
    float start_duty = current_duty;
    float duty_diff = target_duty - start_duty;

    for (int i = 0; i < LED_TRANSITION_STEPS; i++) {
        // Use sine curve for smoother transition
        float progress = (float)i / (LED_TRANSITION_STEPS - 1);
        float smooth_progress = (1 - cosf(progress * M_PI)) / 2;
        uint8_t new_duty = start_duty + (duty_diff * smooth_progress);
        
        led_set_duty(new_duty);
        vTaskDelay(pdMS_TO_TICKS(LED_TRANSITION_STEP_MS));
    }
    
    // Ensure we reach exactly the target value
    led_set_duty(target_duty);
    
    transition_task_handle = NULL;
    vTaskDelete(NULL);
}

void led_set_connection_state(bool connected)
{
    uint8_t target_duty = connected ? LED_PWM_CONNECTED : LED_PWM_DISCONNECTED;
    
    // If a transition is already running, stop it
    if (transition_task_handle != NULL) {
        vTaskDelete(transition_task_handle);
        transition_task_handle = NULL;
    }

    // Create new transition task
    xTaskCreate(led_transition_task,
                "led_transition",
                2048,
                (void*)((uint32_t)target_duty),
                1,
                &transition_task_handle);
} 