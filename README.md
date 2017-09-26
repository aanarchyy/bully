# OVERVIEW [![License](https://img.shields.io/badge/License-GPL%20v3%2B-blue.svg?style=flat-square)](https://github.com/aanarchyy/bully/blob/master/LICENSE.md)

**Bully** is a new implementation of the WPS brute force attack, written in C. It is conceptually identical
to other programs, in that it exploits the (now well known) design flaw in the WPS specification. It has
several advantages over the original reaver code. These include fewer dependencies, improved memory and
cpu performance, correct handling of endianness, and a more robust set of options. It runs on Linux, and
was specifically developed to run on embedded Linux systems (OpenWrt, etc) regardless of architecture.

Bully provides several improvements in the detection and handling of anomalous scenarios. It has been
tested against access points from numerous vendors, and with differing configurations, with much success.

You **must** already have Wiire's Pixiewps installed.
The latest version can be found here: [https://github.com/wiire/pixiewps](https://github.com/wiire/pixiewps).

# Requirements

`apt-get -y install build-essential libpcap-dev aircrack-ng pixiewps`

# Setup

**Download**

`git clone https://github.com/aanarchyy/bully`

or

`wget https://github.com/aanarchyy/bully/archive/master.zip && unzip master.zip`

**Build**

```bash
cd bully*/
cd src/
make
```

**Install**

```
sudo make install
```

# Usage

```
  usage: bully <options> interface

  Required arguments:

      interface      : Wireless interface in monitor mode (root required)

      -b, --bssid macaddr    : MAC address of the target access point
   Or
      -e, --essid string     : Extended SSID for the access point

  Optional arguments:

      -c, --channel N[,N...] : Channel number of AP, or list to hop [b/g]
      -i, --index N          : Starting pin index (7 or 8 digits)  [Auto]
      -l, --lockwait N       : Seconds to wait if the AP locks WPS   [43]
      -o, --outfile file     : Output file for messages          [stdout]
      -p, --pin N            : Starting pin number (7 or 8 digits) [Auto]
      -s, --source macaddr   : Source (hardware) MAC address      [Probe]
      -v, --verbosity N      : Verbosity level 1-4, 1 is quietest     [3]
      -w, --workdir path     : Location of pin/session files  [~/.bully/]
      -5, --5ghz             : Hop on 5GHz a/n default channel list  [No]
      -B, --bruteforce       : Bruteforce the WPS pin checksum digit [No]
      -F, --force            : Force continue in spite of warnings   [No]
      -S, --sequential       : Sequential pins (do not randomize)    [No]
      -T, --test             : Test mode (do not inject any packets) [No]

  Advanced arguments:

      -d, --pixiewps         : Attempt to use pixiewps               [No]
      -a, --acktime N        : Deprecated/ignored                  [Auto]
      -r, --retries N        : Resend packets N times when not acked  [2]
      -m, --m13time N        : Deprecated/ignored                  [Auto]
      -t, --timeout N        : Deprecated/ignored                  [Auto]
      -1, --pin1delay M,N    : Delay M seconds every Nth nack at M5 [0,1]
      -2, --pin2delay M,N    : Delay M seconds every Nth nack at M7 [5,1]
      -A, --noacks           : Disable ACK check for sent packets    [No]
      -C, --nocheck          : Skip CRC/FCS validation (performance) [No]
      -D, --detectlock       : Detect WPS lockouts unreported by AP  [No]
      -E, --eapfail          : EAP Failure terminate every exchange  [No]
      -L, --lockignore       : Ignore WPS locks reported by the AP   [No]
      -M, --m57nack          : M5/M7 timeouts treated as WSC_NACK's  [No]
      -N, --nofcs            : Packets don't contain the FCS field [Auto]
      -P, --probe            : Use probe request for nonbeaconing AP [No]
      -R, --radiotap         : Assume radiotap headers are present [Auto]
      -W, --windows7         : Masquerade as a Windows 7 registrar   [No]
      -Z, --suppress         : Suppress packet throttling algorithm  [No]
      -V, --version          : Print version info and exit
      -h, --help             : Display this help information
```

## -d // --pixiewps
The -d option performs an offline attack, Pixie Dust _(`pixiewps`)_, by automatically passing the **PKE**, **PKR**, **E-Hash1**, **E-Hash2**, **E-Nonce** and **Authkey**. `pixiewps` will then try to attack **Ralink**, **Broadcom** and **Realtek** chipsets.

## -v // --verbosity
The -v option specifies the verbosity of **bully**.
-v 4 now prints all the collected hashes and outputs the pixiewps command run.
Default runlevel is 3.

# Acknowledgements
None of this would have happened without the rest of the great team I am proud to credit with helping me:
`t6_x`, `DataHead`, `Soxrok2212`, `Wiire`
Huge thanks to `wiire` for helping remove the openssl dependancy!
