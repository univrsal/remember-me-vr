# Deploying a dev build

For development: install a build straight into the game folder. (End users extract the release zip instead; see [installing-linux.md](installing-linux.md).)

Create a symlink named `Remember Me` in the repository root pointing at the game's install folder (it's gitignored), then:

```sh
meta/tools/deploy.sh install      # copies dinput8.dll (+ default ini if absent) into Binaries/Win32
meta/tools/deploy.sh status       # what's installed + last log lines
meta/tools/deploy.sh uninstall    # removes only what install put there
```

The script records what it installed in `Binaries/Win32/.vrmod-installed`, never overwrites a file it didn't install, and keeps an existing INI. Options: `--game <name>`, `--preset <preset>`. Paths come from `src/games/<name>/deploy.env`.
