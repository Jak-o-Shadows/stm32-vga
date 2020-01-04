import os

Import("env")

platform = env.PioPlatform()

TOOLDIR = 'E:\\Tools\\arm-none-eabi\\8 2019-q3-update\\bin\\'

# Flash the elf, as it has offset information
env.Replace(
    UPLOADER='"%s"' % os.path.join(TOOLDIR, "openocd"),
    UPLOADCMD='$UPLOADER -s "%s" -c "interface hla; hla_layout stlink" -f interface/stlink-v2.cfg -f target/stm32f1x_stlink.cfg -c "program {{.pio\\build\\bluepill_f103c8\\firmware.elf}} reset; shutdown"' % (
        os.path.join(TOOLDIR, "scripts"))
)
