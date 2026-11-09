<h1>Nightfire Recomp</h1>

A native Windows port of **007: Nightfire** for the original Xbox, made by static
recompilation with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp). The game code is
translated to C and built on your own PC from your own disc. This is not an emulator.

Both parts of the game run: the on-foot missions and the driving missions.

**No game files are included.** You need your own copy of the game.

<h2>What you need</h2>

- Windows 10 or 11, 64-bit, and a DirectX 11 graphics card
- An extracted Xbox disc of 007: Nightfire, European (PAL) or North-American (NTSC).
  [extract-xiso](https://github.com/xboxdev/extract-xiso) can extract it.
- About 5 GB of free disk space

<h2>Install</h2>

1. Download `Nightfire-Setup.exe` from the [Releases](../../releases) page.
2. Run it, choose the `default.xbe` of your extracted disc and a folder to install to
   (not inside Program Files).
3. Wait a few minutes. The setup builds the game on your PC. It brings everything it needs
   and installs nothing else.

When it is done, start the game with **Nightfire Launcher** on your desktop.

<h2>Play</h2>

The launcher has two modes:

- **Original**: the game as on the console, with a controller only.
- **Enhanced**: adds mouse and keyboard, higher resolution, widescreen and other options.

<h2>Build from source</h2>

For developers. You need Visual Studio 2022 or newer with the C++ workload and CMake, and
Python 3.10+ with `py -3 -m pip install capstone pefile`.

```powershell
git clone --recursive https://github.com/stefredcore/Nightfire-Recomp.git
cd Nightfire-Recomp
# copy your extracted disc into a folder named "game" (game\default.xbe must exist)
powershell -ExecutionPolicy Bypass -File scripts\recompile.ps1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

The game is then in `build\Release`. Start it with `Nightfire Launcher.exe` in the project
folder.

<h2>Legal</h2>

This is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts,
Eurocom, MGM, Danjaq or Microsoft. James Bond, 007 and Nightfire are trademarks of their
owners.

The project's own code is under the [MIT License](LICENSE). The toolkit in
`external/xboxrecomp` has its own licences.
