
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/dma.h>
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

static uint8_t lineBuffer[52];
static uint8_t emptyBuffer[52];

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
    rcc_periph_clock_enable(RCC_SPI1);
    rcc_periph_clock_enable(RCC_DMA1);
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

    // Toggle the pin to prove it works
    gpio_set_mode(GPIOA,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL,
                  GPIO7);
    for (int repeat = 1; repeat < 100; repeat++)
    {
        gpio_toggle(GPIOA, GPIO7);
        for (int i = 1; i < 10000; i++)
        {
            __asm__("nop");
        }
    }

    // Setup data output pins
    //  Uses SPI DMA -> a MOSI pin
    //  PA7 is MOSI
    //  PA5 is SCK
    gpio_set_mode(GPIOA,
                  GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                  GPIO7 | GPIO5);
}

static void spiDma_setup(void)
{
    // Reset: CR1 cleared, SPI disabled
    spi_reset(SPI1);

    // Use 8 bit transfers
    spi_init_master(SPI1,
                    SPI_CR1_BAUDRATE_FPCLK_DIV_2, // 72MHz/2 = 36MHz required for pixel clock
                    SPI_CR1_CPOL_CLK_TO_0_WHEN_IDLE,
                    SPI_CR1_CPHA_CLK_TRANSITION_1,
                    SPI_CR1_DFF_8BIT,
                    SPI_CR1_MSBFIRST);

    // Set to software NSS
    // Must do, despite ignoring it entirely
    spi_enable_software_slave_management(SPI1);
    spi_set_nss_high(SPI1);

    // Tell SPI we are using DMA
    spi_enable_tx_dma(SPI1);

    // Enable SPI
    spi_enable(SPI1);

    // Setup DMA
    dma_channel_reset(DMA1, DMA_CHANNEL3);

    dma_set_peripheral_address(DMA1, DMA_CHANNEL3, (uint32_t)&SPI1_DR);
    dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL3);
    dma_set_peripheral_size(DMA1, DMA_CHANNEL3, DMA_CCR_PSIZE_8BIT);
    dma_set_memory_size(DMA1, DMA_CHANNEL3, DMA_CCR_MSIZE_8BIT);
    dma_set_read_from_memory(DMA1, DMA_CHANNEL3);
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
    spiDma_setup();

    for (int i = 1; i < 52; i++)
    {
        lineBuffer[i] = i;
        emptyBuffer[i] = 0;
    }

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
        // Clear compare interrupt flag.
        timer_clear_flag(TIM1, TIM_SR_CC2IF);

        // Increment line, and process
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
            // Send an empty line (default line buffer)
            dma_disable_channel(DMA1, DMA_CHANNEL3);
            dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)emptyBuffer);
            dma_set_number_of_data(DMA1, DMA_CHANNEL3, 52);
            dma_enable_channel(DMA1, DMA_CHANNEL3);
            break;

        case FRAME_OUTPUT_START ... FRAME_OUTPUT_END:
            // Send data
            // Send an empty line (default line buffer)
            dma_disable_channel(DMA1, DMA_CHANNEL3);
            dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)lineBuffer);
            dma_set_number_of_data(DMA1, DMA_CHANNEL3, 52);
            dma_enable_channel(DMA1, DMA_CHANNEL3);

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
            dma_disable_channel(DMA1, DMA_CHANNEL3);
            dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)emptyBuffer);
            dma_set_number_of_data(DMA1, DMA_CHANNEL3, 52);
            dma_enable_channel(DMA1, DMA_CHANNEL3);
            break;

        case FRAME_END:
            scanlineNumber = 0;
            break;
        }
    }
}