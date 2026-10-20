# vip_jumps (Plugify)

Extra jumps for VIPs: the 'jumps' feature of groups.ini is the total number of jumps per airtime (1 = none). A per-frame timer watches the IN_JUMP edge of every living VIP with the feature on and gives the extra jumps (vertical velocity 300), with the original's fix for the tick where the engine has already launched but FL_ONGROUND is still set. A port of the Metamod:Source module 'VIP_Jumps' of the Pisex cs2-vip plugin.

Plugify port (fork) of **VIP_Jumps** by Pisex && ABKAM (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_jumps-1.0.3.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_jumps`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

