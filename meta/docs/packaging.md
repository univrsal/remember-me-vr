# Packaging and CI

```sh
meta/tools/package.sh             # -> dist/RememberMeVR-<git describe>.zip
```

The zip mirrors the game folder (`Binaries/Win32/dinput8.dll`, `RememberMeVR.ini`), plus `INSTALL.txt`, `LICENSE.txt` and `VERSION.txt` at the root, so users extract it straight into the game's install folder. The packaged DLL has its debug info stripped; the copy in `build/` keeps it. What goes into the zip is configured per game in `src/games/<name>/package.env`.

## CI

`.github/workflows/build.yml` runs on every push, pull request and manual trigger. It builds with Ubuntu's mingw-w64, runs the host unit test, packages, and uploads the zip as a workflow artifact. To build more games in CI, add them to the workflow's `matrix.game` list.

## Releasing

Push a tag starting with `v`:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

That creates a GitHub pre-release named after the tag, with the zip attached and auto-generated notes.
