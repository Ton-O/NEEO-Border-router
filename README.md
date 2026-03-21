# NEEO-Border-router
A border router to be used with MetaBrain
## *** Alpha status ***
This software is under development, please do NOT expect a working solution.
I do not take any issues on the Github repository as I'm simplhy still experimenting with the solution.

## What is this?
This is the first release of a border-router to be used by the Meta Brain container I developed.
It functions as a bridge between a JN516x (8 or 9) device and the Brain running in a docker container.
That way, it can connect normal wifi/ethernet woith 6Lowpan traffic as used by the NEEO-remote (TR2).  
## How to use
Just use the files as a PlatformIO project, compile and write to an ESP32 (current setup uses the WROOM32 aka Lolin32).
## How to integrate it in Brain
The border router is a standalone device, that is connected to the UART-pins of a JN516x device that runs a 6LowPan stack. With standalone I mean: it is NOT physically connected to the docker host; it interfaces with the Docker Meta Brain via your network (Wifi/ethernet).
It features a "GUI"(note the quotes;-)) to allow you to setup the ESP32 (Wifi) followed by setup of the   Border Router itself.
This 2-stage approach will go as follows once ESP32 is flashed and powered on:
1 Check wifi on your phone or laptop to find an unsecure Access Point named "NEEO-Border-Router" (the ESP32 waiting to be configured)
Once found, connect to the SSID which will (hopefully) present you with a popup shwoing the ESP32's WifiManager. If not, you can always use your browser to to the IP-address that the WifiManager has (probably 192.168.4.1). Here, click Configure Wifi, select your Wireless network SSID and fill in the wfi-password (this HAS to be the network that is used by your Meta Brain). The ESP32 will reboot and join your wifi-network; if unsuccesful, it will return to the initial situation allowing you to retry configuration of Wifi.
2 Assuming stage 1 has completed successfully, you will now associate this border-router with the Meta Brain. First, ytou need to find out the IP-address of the ESP32. If you still have it connected to your computer, you will find the ip-address that your router assigned to the ESP32 in the Serial log. If not, you need to check your router to find the address.
Once you have the ip-address, you can got to <ip-address found>:8080. This will show the GUI. 
Under config, you can fill in the dns-name of your Meta-Brain, click save and have the esp32 reboot again.
Note: the DNS-name, not its IP-address. The border router will register it self through mDNS as <name filled in as Meta brain>-jn5168.local
This should be enough, the latest firmware will detect the name <sMeta brain>-jn5168.local

## What do I need for this
I'm not going into too much details on hardware aspects, but this is wat I'm using:
- ESP32 (Wroom32, but most ESP32 devices will work)
- CeByte E75
- Breadboard (note that E75 uses a 1.27mm pitch!!)


