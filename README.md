<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Edge - Xbox 360 Emulator</h1>

Xenia Edge is yet another experimental fork of the Xenia emulator, originally based on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). Edge builds have evolved far beyond Canary in many areas, starting from the UI and ranging to kernel emulation, networking, graphics backend features, JIT improvements, arm64 and macOS support, and many others. Exact differences are hard to pin down any longer due to how extensive they are and how much has been ported back into Canary over time to various degrees. In general Edge will run more games, and run them better, but this code is under active development and there are always regressions so YMMV. If something isn't working on Edge it's worth testing on Canary.

While there is no official compatibility tracker for Xenia Edge at the moment, all known games at least get to gameplay and should render more or less correctly and run at their target framerates or better. At least on sufficiently decent hardware (generally RTX 2xxx class and above). Some games require game-specific config changes, so make sure to check the [Canary compatibility page](https://github.com/xenia-canary/game-compatibility/issues) if something is wrong, the issue is likely already known and has a documented solution.

## Status

Build (Windows / Linux / macOS): [![CI](https://github.com/has207/xenia-edge/actions/workflows/CI.yml/badge.svg?branch=edge)](https://github.com/has207/xenia-edge/actions/workflows/CI.yml) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/has207/xenia-edge/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

Releases
--------
[Latest](https://github.com/has207/xenia-edge/releases/latest) ◦ [All](https://github.com/has207/xenia-edge/releases)

FAQ
---

- Q: How to use systemlink?<br>
  A: Systemlink is enabled by default and will work without further configuration with other Edge instances on the same network. If that's all you care about you can stop reading here. To connect to retail consoles you need an instance of [Xlink Kai](https://www.teamxlink.co.uk/) running somewhere on your network, most likely the same PC you're running Xenia but it's not necessary. On RGH/JTAG consoles you have to disable the Nova plugin in Aurora as that replaces normal systemlink on the console. If you plan to connect to console or users over the internet you will need to set ```systemlink_transport = "kai"``` in the Network settings. For remote connections you will need to add them as a friend in the Kai Engine interface once you started it, and they must do the same, or all of you need to be in the same Kai arena for the game. No other configuration should be needed. It's possible to use Kai locally with multiple Edge instances but it requires setting "Allow remote DDS connections" in the Kai settings and pointing the other Edge instances at your machine using ```systemlink_kai_engine = $IP_OF_PC_RUNNING_KAI```, but again you don't *need* Kai for local Xenia-only connections, those will work without any configuration changes using the default ```systemlink_transport = "tunnel"```.

- Q: How to run original Xbox games?<br>
  A: OG Xbox games are supported in the same manner they are on a real console, more precisely an RGH/JTAG console, through ```xefu```. You will want to search for how to run them there, the same information applies here. Start [here](https://consolemods.org/wiki/Xbox_360:Original_Xbox_Games_Compatibility_List). In other words, you'll likely want the "patched" xefu builds that remove restrictions on what games they support. Keep in mind xefu is not a single executable but a "family" where ```xbox.xex``` is the console's dispatcher that selects the appropriately whitelisted ```xefuN.xex```. These have all been community patched to run other games, so you'll want to figure out how it's done on RGH/JTAG setups and mostly follow those instructions. Do not expect compatibility/performance to be quite as good as a real console quite yet, it is still a work in progress, but many games should be playable.

- Q: How to run Xbox Live Indie (XBLIG) games?<br>
  A: XBLIG titles require an XNA runtime and launcher. The launcher comes with the system files, which you can install with ```Tools/Install System Files```. As for the runtime, it's an update for the launcher and you will need to get it the same place you find XBLIG titles themselves. Install the update with ```Tools/Install Content...```. The titles are not proper game packages like everything else and instead install as DLC content for the launcher. You can install them the same way or add the packages where they are with ```File/Add Games...```. Either way each one gets its own entry in the game list and starts from there or from the dashboard's My Games.
  
- Q: How to tell what options a particular game might need?<br>
  A: Many games that are not running well by default require one or two simple config changes that make them (near) perfect. To find out, check the Compatibility section in game details pane. If there is an existing compatibility page there will be a link to Master, Canary, or both. Prefer information on the Canary page, you will often find the answer there.

- Q: Why are translations into language X so bad?<br>
  A: They're AI-generated, if you speak the language and want to help edit the relevant .po file in the assets directory and submit it. If you don't know how to use git then just open a bug and attach the fixed .po file. Best way to edit those is with a program called Poedit. Or just open a bug and tell me what translation is bad and how to improve it.

- Q: Should I use the Windows build with wine/proton on Linux?<br>
  A: The native build for Linux is quite competitive with Windows now and is recommended over wine/proton.

- Q: Why is macOS build not as capable as Windows / Linux?<br>
  A: macOS is the newest port, with a completely different CPU and GPU backends that have not had nearly the amount of testing of the other platforms. Many games will have issues on macOS that run and render fine on Windows and Linux. This is working as intended for now. No need to tell me about issues you find with the macOS port. I know.

- Q: Can you add ARM64 Linux or Windows builds?<br>
  A: Yes, and I choose not to, there is no need to send me PRs or requests for these builds. They will be made available when I'm ready to support them, in the meantime you can build from source if you really want to run them.
  
- Q: Will you accept donations of money or games/hardware for testing?<br>
  A: No, this project is strictly a hobby and I would like to keep it that way. I pay for all games/hardware I test out of my own pocket and do not want to receive any compensation for my work on Xenia.

