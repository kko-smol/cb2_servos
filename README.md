# cb2_servos
Cubieboard2 servo control kernel module.

Test video:
https://www.youtube.com/watch?v=Z9xsIgXQXy8

Development description:

http://we.easyelectronics.ru/electro-and-pc/upravlenie-servomashinkoy-iz-cubieboard2.html 

http://we.easyelectronics.ru/electro-and-pc/hello-world-dlya-yadra-linux-na-cubieboard2.html

This module allow to control up to 28 servos via /sys/class/servos/servo_X interface.
Write string value from 0 to 1000 for set pulse width from 500us to 2500us

Module use Timer4 and interrupt for timings, and GPIO port for output.
I used this module with old 3.x linux kernel on Archlinux.
