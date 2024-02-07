# ATTiny85_CW_Keyer (now also for ATtiny45 (and ATtiny84))
Original description:  
```
A full-featured CW keyer for amateur radio use. The keyer is built around a cheap and tiny ATTINY85 microcontroller. The circuit boasts 4-100 character memories, beacon mode, multiple timing options, and a CW trainer for improving your Morse code speed.
```

## About this fork
In this fork, I didn't change hardly any of the functionality. I was stuck with only an ATtiny45 and found that the original code would just about fit into the smaller ROM, but the EEPROM data needed to be reduced in size. This is why I reduced the available 100-char messages from four to two.  
Also, I happened to have an ATtiny84A laying around, which offers a few more pins, and ported the code to that chip by tweaking the register settings, mainly for the timers. You can find that variant in the `t84` branch of this repository. Check `yack.h` to get the correct pin assignments.

Instead of connecting a piezo buzzer for the sidetone, I am personally filtering the PWM quasi-square wave signal through a narrow Sallen-Key bandpass and then listening to the resulting quasi-sinusoidal tone through headphones while powering the device on a 3V coin cell battery.

## A few details about timing in this code
- The code is organized around an inner software heartbeat called `yackbeat`. Every tick of this heartbeat takes 5 ms and during this time, the code checks if any tasks need to get done, then waits for the next tick to begin.
- The code uses `Timer0` for PWM generation (sidetone output) and `Timer1` for heartbeat generation.
- Although the code doesn't make use of interrupts (except for waking up from deep sleep on keypresses), it waits for an interrupt flag being set (upon `Timer1` compare match) before starting a new `yackbeat`.

## Acknowledgements
- Tracing back the origins of this code, it originally was designed by Jan Lategahn, DK3LJ. It can be found [on Sourceforge](https://yack.sourceforge.net/).
- It then was picked up by Jack Welsh, AI4SV, who improved on it, and posted it on his website. Blog articles [here](https://blog.templaro.com/a-tiny-and-open-source-cw-keyer/) and [here](https://blog.templaro.com/jackyack-rev-a/).
- Finally, Don Froula, ex WD9DMP, now K9CLF, posted it here on GitHub, from where I forked the repository. He made further refinements and [a video](https://www.youtube.com/watch?v=Ol6krttaOy0), accompanied by [files](https://projectmf.net/keyer/) on his web server.
- If I understand correctly, Jack and Don also created an appropriate PCB for the circuit, as seen in another [video](https://www.youtube.com/watch?v=odJTkdFPuio) of Don's.

Funnily enough, at its beginnings, this code was already meant for an ATtiny45! It was then ported to the ATtiny85, and I took it back again to the device it had started on.
