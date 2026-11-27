# Installing on Linux (Proton + SteamVR)

Developed and tested this way. Remember Me is Steam app 228300; Proton Experimental and SteamVR are what it was tested with.

## 1. Install the mod

Download the release zip and extract it into the game's install folder (the one that contains `Binaries/`, `Engine/` and `ExampleGame/`), usually `~/.local/share/Steam/steamapps/common/Remember Me`. The files go to:

```
Binaries/Win32/dinput8.dll
Binaries/Win32/RememberMeVR.ini
```

## 2. Steam launch options

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

## 3. 32-bit OpenXR runtime manifest (once per user)

SteamVR's default OpenXR runtime manifest points at its 64-bit library, which a 32-bit game's Proton OpenXR bridge can't load ("wrong ELF class"). Create `~/.config/openxr/1/active_runtime.i686.json`, adjusting the path to your SteamVR install:

```json
{
  "file_format_version": "1.0.0",
  "runtime": {
    "name": "SteamVR",
    "VALVE_runtime_is_steamvr": true,
    "library_path": "/home/<you>/.local/share/Steam/steamapps/common/SteamVR/bin/vrclient.so"
  }
}
```

From a repository checkout, `meta/tools/setup-openxr-i686.sh` writes this for you (`--remove` undoes it). 64-bit apps are unaffected.

## 4. Play

Start **SteamVR first**, then launch the game from Steam. DXVK asks the runtime for its Vulkan extensions when the game creates its device, so the order matters.

Then set a sensible window size: see [configuration.md](configuration.md#window-size).

## Troubleshooting

- The log is `Binaries/Win32/RememberMeVR.log`.
- Nothing happens / no mod log: the DLL override in step 2 is missing.
- No headset output, "wrong ELF class" in the log: step 3.
- Controllers only respond while the headset is worn (SteamVR unfocuses the session otherwise).
- `[Core] Enabled=0` in the INI turns the mod into a plain pass-through.

## Uninstall

Delete `Binaries/Win32/dinput8.dll`, `RememberMeVR.ini` and `RememberMeVR.log` (and remove the launch option).
