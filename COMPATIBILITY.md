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
| DRAGON BALL FighterZ                     | `00F385F6` | **Menu**     | Unreal Engine 4. The lobby, a 3D scene, with the right picture                                                               |
| Forza Motorsport 6 Demo                  | `132D50F5` | **Menu**     | Direct3D 11.X. Main menu after about a minute; its text and logo are right, the background behind them is striped            |
| Minecraft: Xbox One Edition              | `149E11AE` | **Menu**     | Direct3D 11.X. Main menu with the right picture                                                                              |
| Terraria                                 | `1738CA2A` | **Playable** | Main menu with the right picture; writes its save                                                                            |
| Puyo Puyo Champions                      | `62B3A350` | **Menu**     | Direct3D 11.X. The autosave notice, waiting for A; reads the gamepad                                                         |
| Crash Bandicoot N. Sane Trilogy          | `044D0F74` | **Menu**     | Title screen; the text under its logo is drawn without its transparency                                                      |
| Mega Man 11                              | `1F337CFB` | **Menu**     | Direct3D 11.X. Main menu ("New Game / Load Game / Options / Extra Modes") after intro; reads the gamepad and writes saves     |
| Overcooked                               | `145207D1` | **Menu**     | Unity. 3D main menu ("Campaign / Versus / Options / Credits") with shadows, gamepad input, and connected storage             |
| Rock Band 4                              | `2DBB840F` | **Intro**    | Direct3D 11.X. ESRB rating screen with the right picture; reads the gamepad and writes saves                                 |
| SPACE JAM: A NEW LEGACY - THE GAME       | `622B018E` | **Menu**     | GDK title on Direct3D 12. Title screen with the right picture; A goes on into its opening story                              |
| LEGO CITY UNDERCOVER                     | `27D78EC2` | **Menu**     | Main menu, reads the gamepad; the 3D scene behind it is black                                                                |
| Minecraft (Bedrock)                      | `6CFA0C1E` | **Menu**     | Direct3D 12. Main menu at 1080p with 3D character, UI, audio, and saves                                                      |
| Puyo Puyo Tetris 2                       | `6AA460A3` | **Menu**     | Direct3D 12. Main menu with the right picture; reads the gamepad                                                             |
| Rocket League                            | `214E91AC` | **Intro**    | Unreal Engine 3. Renders the 3D `MENU_Beach_p` scene, reads the gamepad, and opens its save container; UI overlay is missing  |
| 1001 Spikes                              | `6B2E4F1B` | **Menu**     | Main menu with the right picture; reads the gamepad                                                                          |
| 7 Days to Die                            | `039D30F6` | **Menu**     | Unity. 1080p title screen ("7 Days to Die / Press Menu"); 1,000+ frames                                                      |
| A KING'S TALE: FINAL FANTASY XV          | `05401907` | **Menu**     | Direct3D 11.X. Pixel-art title screen at 1080p after intro logos; 1,000+ frames, audio, reads the gamepad                    |
| Cyberpunk 2077                           | `0D42AD14` | **Nothing**  | Launcher package (`cpLauncher.exe`) starts CoreApplication, then stops with missing `engine/` and `r6/` data archives         |
| Dance Central Spotlight                  | `295F3C44` | **In-game**  | First kits. Harmonix intro, loading screen, and in-game pause menu; 1,000+ frames, reads the gamepad and writes saves        |
| DARK SOULS: REMASTERED NETWORK TEST VER. | `27EDC966` | **Menu**     |                                                                                                                              |
| FIFA 15                                  | `64B02253` | **Intro**    | First kits. Its opening screen with the right picture                                                                        |
| FRU                                      | `3C646A12` | **Intro**    | Unity. Studio logo ("Through Games!") and loading animation; the game is played with the Kinect sensor                       |
| FUSER Demo                               | `61BEB2FD` | **Boots**    | Unreal Engine 4 on Direct3D 12. One frame, then a fault                                                                      |
| Just Dance 2020                          | `13E02891` | **Menu**     | Direct3D 11.X. Interactive mode-selection menu with the right picture, 1,000+ frames, and saves                              |
| Minecraft Story Mode                     | `781521BD` | **In-game**  | Direct3D 11.X. 3D title screen, interactive main menu, saves, and enters Episode 1                                           |
| Peggle 2                                 | `32966611` | **Playable** | Launch kit, Direct3D 11. Plays, with the right picture                                                                       |
| Pixel Piracy                             | `30D1E0F7` | **Menu**     | Unity. Animated main menu ("Start Game / Settings") with audio and gamepad input                                             |
| Project Spark                            | `60D19E14` | **Menu**     | Title screen with the right picture, after its screen calibration; not online                                                |
| Promenade (Demo)                         | `6DBB4B4C` | **Intro**    | GDK title on Direct3D 12. Loads FMOD Studio plugins, writes XGameSave, and presents its 4K splash screen without device loss |
| Rayman Legends                           | `27BDEA18` | **Menu**     | Launch kit, Direct3D 11. Title screen with the right picture                                                                 |
| Saints Row Boss Factory                  | `65D9BC13` | **Nothing**  | GDK title on Direct3D 12. Faults while it waits for its files, before its device                                             |
| Seasons after Fall                       | `30AD5A99` | **Menu**     | Unity. Main menu ("Start Game / Change profile / Settings / Credits / Extra") at 1080p                                       |
| Shadow of the Tomb Raider                | `36773C0D` | **Boots**    | A few frames, then it faults: it sizes texture memory by the console's tiled layouts, which are not known here               |
| Slender - The Arrival                    | `458E04D7` | **Playable** | Unity. Plays from its opening on                                                                                             |
| SONIC FORCES                             | `07B5597E` | **Playable** | Gets in game and plays; some textures of its HUD lack their transparency                                                     |
| Infinity Blade Saga                      | `6346E6A1` | **Nothing**  | Unreal Engine 3. Finds its package configs and shader binaries, then stops on missing global shader platform                 |
| Plants vs. Zombies Garden Warfare        | `334CB9F9` | **Menu**     | Direct3D 11.X. 3D main menu at 1080p ("Garden Ops / Multiplayer / Split Screen"); 1,000+ frames, reads the gamepad            |
| Sonic Mania                              | `34BA1DE0` | **Playable** |                                                                                                                              |
| Stardew Valley                           | `1240B5DA` | **Nothing**  | Faults in its own garbage collector before its device                                                                        |
| Titanfall                                | `4D046B58` | **Boots**    | First kits, Source engine. Presents frames, the picture is black or one flat colour                                          |
| Trackmania                               | `700107ED` | **Menu**     | GDK title on Direct3D 12. Intro and title screen with the right picture; stops at "Checking online services"                 |
| UNDERTALE                                | `62F67ADD` | **Playable** | Plays from its opening on, with the right picture                                                                            |

## Xbox Series X|S

| Title                                             | ID         | State        | Notes                                                                                                       |
| ------------------------------------------------- | ---------- | ------------ | ----------------------------------------------------------------------------------------------------------- |
| Minecraft Preview                                 | `639AA8DD` | **Playable** | Main menu with the right picture                                                                            |
| Unicorn Overlord                                  | `6897AC80` | **Playable** |                                                                                                             |
| Fortnite                                          | `0FF4B5BD` | **Menu**     | Unreal Engine. Loading screen with the right picture, at "connecting"; it needs its servers to go on        |
| Psychonauts 2                                     | `68076383` | **Intro**    | Unreal Engine 4. First frames, then the host drops the Direct3D 12 device                                   |
| XDefiant                                          | `799B973E` | **Boots**    | Sustains `PresentX` without dropping the Direct3D 12 device; 2,039 of its 2,040 shaders translate; picture is black         |
| Concept Destruction                               | `72A634A4` | **Boots**    | Unity. Presents its first frames, then the host drops the Direct3D 12 device                                |
| Dynacat                                           | `6322C08F` | **Nothing**  |                                                                                                             |
| The Matrix Awakens: An Unreal Engine 5 Experience | `7D3CD3FE` | **Boots**    |                                                                                                             |

The 40 public GDK samples that build for Series all draw their picture; they are samples, not
games, and are not listed.

## Reporting a title

Open an issue with the title's name and ID, the version of XValkyrie, and the log of the run.
Say what is on screen when it stops.
