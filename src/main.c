
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>

#define RCCLEDPORT (RCC_GPIOC)
#define LEDPORT (GPIOC)
#define LEDPIN (GPIO13)

// Basic timing values in terms of scanlines
//  For 800x600 @ 56 Hz
//      As this resolution uses a pixel clock of 36 MHz (http://martin.hinner.info/vga/timing.html)
#define YOFFSET 10
#define XOFFSET 0
#define YEXTENT 576

#define YSTRETCH 1

#define FRAME_START 0
#define FRAME_BACKPORCH 2                                   // Start of backporch and blanking
#define FRAME_BACKPORCH_END 22                              // End of backporch
#define FRAME_OUTPUT_START (FRAME_BACKPORCH_END + YOFFSET)  // Start of data output
#define FRAME_OUTPUT_END (FRAME_OUTPUT_START + YEXTENT + 1) // End of data output
#define FRAME_END 624                                       //(FRAME_OUTPUT_END + 1)

#define LINEPERIOD (2048)         // 2048/72Mhz = 28.44us length of a line
#define HORIZSYNCPULSEWIDTH (144) // 144/72Mhz = 2us
#define SYNCPLUSPORCH (280)       // 280/72Mhz = 3.88889us

static void rcc_setup(void)
{
    // System clock
    rcc_clock_setup_in_hse_8mhz_out_72mhz();

    // GPIO
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_AFIO);

    // Timers for sync pulses
    rcc_periph_clock_enable(RCC_TIM1);

    // SPI & DMA for data
    //rcc_periph_clock_enable(RCC_SPI1);
    //rcc_periph_clock_enable(RCC_DMA1); // Only a single DMA -> don't need to stress like the F4's
}

static void gpio_setup(void)
{
    /* Enable GPIO clock. */
    /* Using API functions: */
    /* Set pin to 'output push-pull'. */
    /* Using API functions: */
    gpio_set_mode(LEDPORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, LEDPIN);

    // Sync pins are:
    //  PA1  as VSYNC
    //  PA8 (Timer 1 Channel 1) as HSYNC
    gpio_set_mode(GPIOA,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                  GPIO8);
    gpio_set_mode(GPIOA,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL,
                  GPIO1);

    // Setup data output pins
    //  Uses SPI DMA -> a MOSI pin
    //  PA7
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO7);
}

static void timer_setup(void)
{

    // Timer 1 is used
    //  Channel 1 makes the line sync pulse
    //  Channel 2 triggers the line state machine

    // Enable the interrupt
    nvic_enable_irq(NVIC_TIM1_CC_IRQ);

    // Reset
    rcc_periph_reset_pulse(RST_TIM1);

    // Timer 1
    timer_set_mode(TIM1,
                   TIM_CR1_CKD_CK_INT,
                   TIM_CR1_CMS_EDGE,
                   TIM_CR1_DIR_UP);
    timer_enable_preload(TIM1);
    timer_set_period(TIM1, LINEPERIOD); // Set auto-reload register so that we fire interrupts on timer matching only automaticlly without having to manually reload

    // Default prescaler value

    // Timer 1 Channel 1 is used for the HSYNC
    timer_set_oc_mode(TIM1, TIM_OC1, TIM_OCM_PWM1); // PWM Mode 1: Active until triggered
    timer_enable_oc_output(TIM1, TIM_OC1);
    timer_set_oc_polarity_high(TIM1, TIM_OC1);
    // Set time period to be on
    timer_enable_oc_preload(TIM1, TIM_OC1);
    timer_set_oc_value(TIM1, TIM_OC1, HORIZSYNCPULSEWIDTH);

    // Timer 1 Channel 2 is used to start the DMA request
    // Set when to trigger interrupt
    timer_enable_oc_preload(TIM1, TIM_OC2);
    timer_set_oc_value(TIM1, TIM_OC2, SYNCPLUSPORCH);
    // Interrupt on this channel to signal start of data transfer
    timer_enable_irq(TIM1, TIM_DIER_CC2IE);

    // But delay the trigger to sync properly
    timer_slave_set_mode(TIM1, TIM_SMCR_MSM);

    // Enable output on timer (required even if break or deadtime not used)
    timer_enable_break_main_output(TIM1);

    timer_generate_event(TIM1, TIM_EGR_UG);

    // Enable timer
    timer_enable_counter(TIM1);
}

int main(void)
{
    rcc_setup();
    gpio_setup();
    timer_setup();

    while (1)
    {
    }

    return 0;
}

void tim1_cc_isr(void)
{
    static uint16_t scanlineNumber = 0;

    static uint16_t stretchLine = 0;
    static uint16_t opLine = 0;
    static uint16_t readLine = 0;

    if (timer_get_flag(TIM1, TIM_SR_CC2IF))
    {
        scanlineNumber++;

        switch (scanlineNumber)
        {
        case FRAME_START ...(FRAME_BACKPORCH - 1):
            // The start of frame - turn on VSYNC
            gpio_set(GPIOA, GPIO1);
            break;

        case FRAME_BACKPORCH ...(FRAME_OUTPUT_START - 1):
            // Sync pulse finished - top blanking begins
            gpio_clear(GPIOA, GPIO1);
            // Initialise frame variables
            stretchLine = 0;
            opLine = 0;
            readLine = 0;
            //TODO: More logic
            break;

        case FRAME_OUTPUT_START ... FRAME_OUTPUT_END:
            // Send data
            gpio_set(GPIOA, GPIO7);
            gpio_clear(GPIOA, GPIO7);

            // Is it time for the next line?
            //  Remember we repeat lines
            if (stretchLine++ == YSTRETCH)
            {
                // Swap to newline
                readLine = !readLine; // Swap double buffered line
                stretchLine = 0;
            }
            else
            {
                // blah? TODO: Fix
            }
            break;

        case (FRAME_OUTPUT_END + 1)...(FRAME_END - 1):
            // Send blanking
            break;

        case FRAME_END:
            scanlineNumber = 0;
            break;
        }

        // Clear compare interrupt flag.
        timer_clear_flag(TIM1, TIM_SR_CC2IF);
    }
}