# Compatibility

How far each title that was tried gets when it is started. The ID is the title ID the
launcher shows next to the name.

| State        | Meaning                                                                  |
| ------------ | ------------------------------------------------------------------------ |
| **Playable** | Can be played from start to end without problems that stop it            |
| **In-game**  | Gets into gameplay, with problems that keep it from being played through |
| **Menu**     | Reaches its main menu or title screen with the right picture             |
| **Intro**    | Shows its logos, loading or first screens, and gets no further           |
| **Boots**    | Starts and runs, with no picture or a wrong one                          |
| **Nothing**  | Ends with an error before it shows anything                              |
| **Untested** | In the library, not run yet                                              |

States come from short runs (under a minute, nobody at the gamepad), so a title may get
further than its state says. Logs of a run are in `%APPDATA%\xvalkyrie\logs`.

## Xbox One

| Title                                    | ID         | State        | Notes                                                                                                                        |
| ---------------------------------------- | ---------- | ------------ | ---------------------------------------------------------------------------------------------------------------------------- |
| DRAGON BALL FighterZ                     | `00F385F6` | **Menu**     | Unreal Engine 4. Reaches main menu, then crashes on accepting the notice (`CreateDerivedGraphicsPipelineStateX` `E_INVALIDARG`) |
| Forza Motorsport 6 Demo                  | `132D50F5` | **Menu**     | Direct3D 11.X. Intro video has a green tint and textures are glitched/mis-decoded; reaches main menu                          |
| Minecraft: Xbox One Edition              | `149E11AE` | **Menu**     | Direct3D 11.X. Both legacy Xbox One Edition builds work properly with the right picture                                       |
| Terraria                                 | `1738CA2A` | **Nothing**  | Regression: exits with stowed exception `0xC000027B` after unimplemented `Windows.Kinect.KinectSensor` method 17 (`kinect.dll+0x16a8`) |
| Puyo Puyo Champions                      | `62B3A350` | **Menu**     | Direct3D 11.X. Occasional sprite flicker on main menu; crashes leaving the menu (`CreateTexture2D` `1024x2610` BC3 `E_INVALIDARG`) |
| Crash Bandicoot N. Sane Trilogy          | `044D0F74` | **In-game**  | Intro video has a green tint; menu lacks background/button sprites; cutscenes and in-game 3D are black, while audio, controls, and pause menu work |
| Mega Man 11                              | `1F337CFB` | **Menu**     | Direct3D 11.X. Flickering/font-atlas artifacts on startup, clean main menu and boss select, no audio, drops to ~0.1 FPS on boss info |
| Overcooked                               | `145207D1` | **Intro**    | Unity. Shows splash screen, then textures glitch and go dark while audio and menu controls continue working blind             |
| Rock Band 4                              | `2DBB840F` | **Boots**    | Direct3D 11.X. Presents frames and reads the gamepad; stays on a black screen                                                 |
| SPACE JAM: A NEW LEGACY - THE GAME       | `622B018E` | **Playable** | GDK title on Direct3D 12. Plays with the right picture                                                                        |
| LEGO CITY UNDERCOVER                     | `27D78EC2` | **Menu**     | Main menu, reads the gamepad; textures flicker during loading and on the menu, and the 3D scene behind it is black            |
| Minecraft (Bedrock)                      | `6CFA0C1E` | **Menu**     | Direct3D 12. Black intro; reaches main menu at ~8 FPS without panorama background, and player model loads slowly              |
| Puyo Puyo Tetris 2                       | `6AA460A3` | **Menu**     | Direct3D 12. Main menu with the right picture; reads the gamepad                                                             |
| Rocket League                            | `214E91AC` | **Intro**    | Unreal Engine 3. Initial 3D scene renders broken at very low FPS without UI; pressing A crashes before reaching the main menu |
| 1001 Spikes                              | `6B2E4F1B` | **Playable** | Plays, with the right picture; reads the gamepad                                                                             |
| 7 Days to Die                            | `039D30F6` | **Intro**    | Unity. Intro has graphical glitches and the screen glitches when entering the main menu                                       |
| A KING'S TALE: FINAL FANTASY XV          | `05401907` | **Playable** | Direct3D 11.X. Plays with the right picture, audio, and gamepad input                                                        |
| Cyberpunk 2077                           | `0D42AD14` | **Nothing**  | Launcher package (`cpLauncher.exe`) starts CoreApplication, then stops with missing `engine/` and `r6/` data archives         |
| Dance Central Spotlight                  | `295F3C44` | **Intro**    | First kits. Stuck in a recurring pause/controller prompt before the main menu that reappears when dismissed                   |
| DARK SOULS: REMASTERED NETWORK TEST VER. | `27EDC966` | **Menu**     |                                                                                                                              |
| FIFA 15                                  | `64B02253` | **Intro**    | First kits. Flashes the main menu briefly after loading, then goes blank and softlocks                                        |
| FRU                                      | `3C646A12` | **Intro**    | Unity. Initial loading screen is graphically glitched and does not advance (Kinect title)                                     |
| FUSER Demo                               | `61BEB2FD` | **Nothing**  | Unreal Engine 4 on Direct3D 12. Crashes on startup after initializing FMOD audio (1 blank frame)                              |
| Just Dance 2020                          | `13E02891` | **Menu**     | Direct3D 11.X. Black intro after loading; reaches menu with missing button glyphs and does not respond to gamepad input       |
| Minecraft Story Mode                     | `781521BD` | **Intro**    | Direct3D 11.X. Title screen and DLC check modal without audio, then goes black when entering the main menu                    |
| Peggle 2                                 | `32966611` | **Playable** | Launch kit, Direct3D 11. Plays, with the right picture                                                                       |
| Pixel Piracy                             | `30D1E0F7` | **Intro**    | Unity. Minor glitch on loading screen; main menu is unreadable due to erratic primitive strokes and rectangles                |
| Project Spark                            | `60D19E14` | **Playable** | Playable with minor glitches (some features want fuller online/network emulation)                                             |
| Promenade (Demo)                         | `6DBB4B4C` | **Nothing**  | GDK title on Direct3D 12. Driver faults on a pipeline (`image+0x7d3cb5`), then guest code faults jumping to `0x0`             |
| Rayman Legends                           | `27BDEA18` | **Playable** | Launch kit, Direct3D 11. Playable at 60 FPS with intro/cutscene audio; real-time gameplay lacks BGM/SFX                       |
| Saints Row Boss Factory                  | `65D9BC13` | **Nothing**  | GDK title on Direct3D 12. Faults while it waits for its files, before its device                                             |
| Seasons after Fall                       | `30AD5A99` | **Intro**    | Unity. Intro textures flicker and the main menu renders blank                                                                 |
| Shadow of the Tomb Raider                | `36773C0D` | **Nothing**  | Access violation writing `0x31aba0000` on startup (sizes texture memory by the console's tiled layouts)                       |
| Slender - The Arrival                    | `458E04D7` | **Playable** | Unity. Plays from its opening on                                                                                             |
| SONIC FORCES                             | `07B5597E` | **Playable** | Plays well; the in-game HUD is glitched with alpha/blending issues                                                            |
| Infinity Blade Saga                      | `6346E6A1` | **Boots**    | Unreal Engine 3. Presents 1 frame with partial intro audio, then faults (`Fetch4PCF` global shader missing / `image+0x145e38`) |
| Plants vs. Zombies Garden Warfare        | `334CB9F9` | **Menu**     | Direct3D 11.X. Loading screen is right; welcome screen glitches black and the 3D background behind the main menu is black (regression after `15ae175`) |
| Sonic Mania                              | `34BA1DE0` | **Playable** |                                                                                                                              |
| Stardew Valley                           | `1240B5DA` | **Nothing**  | Faults in its own garbage collector before its device                                                                        |
| Titanfall                                | `4D046B58` | **Boots**    | First kits, Source engine. Flashing color glitches on boot and goes no further                                                |
| Trackmania                               | `700107ED` | **Menu**     | GDK title on Direct3D 12. Intro video is right, main menu runs at ~1.5 FPS (9 stand-in shaders), freezes on `PresentX` `0x80070057` when starting a track while audio continues |
| UNDERTALE                                | `62F67ADD` | **Playable** | Plays from its opening on, with the right picture                                                                            |

## Xbox Series X|S

| Title                                             | ID         | State        | Notes                                                                                                       |
| ------------------------------------------------- | ---------- | ------------ | ----------------------------------------------------------------------------------------------------------- |
| Minecraft Preview                                 | `639AA8DD` | **Nothing**  | Regression: faults in `vcruntime140.dll+0x1a95` writing placed texture memory (`0x26e5e620000`) before showing anything     |
| Unicorn Overlord                                  | `6897AC80` | **Playable** |                                                                                                             |
| Fortnite                                          | `0FF4B5BD` | **Intro**    | Unreal Engine. Regression: crashes during intro (previously reached the "connecting" loading screen)         |
| Psychonauts 2                                     | `68076383` | **Intro**    | Unreal Engine 4. First frames, then the host drops the Direct3D 12 device                                   |
| XDefiant                                          | `799B973E` | **Boots**    | Sustains `PresentX` without dropping the Direct3D 12 device; 2,039 of its 2,040 shaders translate; picture is black         |
| Concept Destruction                               | `72A634A4` | **In-game**  | Unity. Gets into gameplay with intact 3D geometry; 2D and surface textures are corrupted                    |
| Dynacat                                           | `6322C08F` | **Nothing**  | Exits with code `0x1` after `0.5 s` right after resolving `xgameruntime.dll` exports                         |
| The Matrix Awakens: An Unreal Engine 5 Experience | `7D3CD3FE` | **Boots**    |                                                                                                             |

The 40 public GDK samples that build for Series all draw their picture; they are samples, not
games, and are not listed.

## Reporting a title

Open an issue with the title's name and ID, the version of XValkyrie, and the log of the run.
Say what is on screen when it stops.
