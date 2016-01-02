This is my personal copy of the original bully project.




# OVERVIEW

**Bully** is a new implementation of the WPS brute force attack, written in C. It is conceptually identical
to other programs, in that it exploits the (now well known) design flaw in the WPS specification. It has
several advantages over the original reaver code. These include fewer dependencies, improved memory and
cpu performance, correct handling of endianness, and a more robust set of options. It runs on Linux, and
was specifically developed to run on embedded Linux systems (OpenWrt, etc) regardless of architecture.

Bully provides several improvements in the detection and handling of anomalous scenarios. It has been
tested against access points from numerous vendors, and with differing configurations, with much success.

You **must** already have Wiire's Pixiewps installed.
The latest version can be found here: [https://github.com/wiire/pixiewps](https://github.com/wiire/pixiewps).

## -d --pixiewps
The -d option option performs an offline attack, Pixie Dust _(`pixiewps`)_, by automatically passing the **PKE**, **PKR**, **E-Hash1**, **E-Hash2**, **E-Nonce** and **Authkey** variables. `pixiewps` will then try to attack **Ralink**, **Broadcom** and **Realtek** detected chipset.
