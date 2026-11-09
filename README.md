Nightfire Recomp
A native Windows port of the original Xbox version of 007: Nightfire, created through static recompilation using xboxrecomp.

Unlike an emulator, Nightfire Recomp converts the original Xbox executable into native code that runs directly on Windows.

Features
Native Windows executable
No emulator required
Supports both PAL and NTSC Xbox releases
Full campaign playable
On-foot missions
Driving missions
Enhanced mode with modern PC features
Original mode for an authentic Xbox experience
⚠️ No game files are included. You must own a legitimate copy of the Xbox version of 007: Nightfire.

Requirements
Operating System
Windows 10 (64-bit)
Windows 11 (64-bit)
Hardware
DirectX 11 compatible GPU
Approximately 5 GB of free disk space
Game Files
You need an extracted copy of the Xbox version of 007: Nightfire:

PAL (Europe)
NTSC (North America)
To extract your game disc or ISO, you can use:

extract-xiso
Installation
1. Download the Installer
Download Nightfire-Setup.exe from the project's Releases page.

2. Run the Installer
When prompted:

Select the game's default.xbe
Choose an installation directory
Recommended: Do not install inside Program Files.

3. Build Process
The installer will automatically:

Verify the game files
Recompile the game on your PC
Create a standalone installation
No additional software is required.

4. Launch the Game
After installation, start the game using:

Nightfire Launcher

Playing
Nightfire Recomp offers two gameplay modes.

Original Mode
Experience the game as it was on the original Xbox:

Controller-only gameplay
Original controls
Original game behavior
Enhanced Mode
Adds modern PC functionality:

Mouse and keyboard support
Higher resolutions
Widescreen support
Additional graphics and gameplay options
Keyboard Shortcuts
Key	Function
F9	Toggle mouse & keyboard controls
F11	Toggle fullscreen
Alt + Enter	Toggle fullscreen
Building From Source
Prerequisites
Visual Studio
Install:

Visual Studio 2022 or newer
Desktop Development with C++
CMake support
Python
Python 3.10 or newer
Install required packages:

py -3 -m pip install capstone pefile
Clone the Repository
git clone --recursive https://github.com/stefredcore/Nightfire-Recomp.git
cd Nightfire-Recomp
Prepare Game Files
Create a folder named:

game
Copy your extracted Xbox game files into it so that:

game\default.xbe
exists.

Recompile
powershell -ExecutionPolicy Bypass -File scripts\recompile.ps1
Build
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
Run
The compiled game will be located in:

build\Release
Launch:

Nightfire Launcher.exe
FAQ
Is this an emulator?
No.

Nightfire Recomp uses static recompilation to translate the original Xbox executable into native code. The game runs directly on Windows without emulating Xbox hardware.

Are game files included?
No.

This project contains only original project code. Users must provide their own copy of the Xbox game.

Which versions are supported?
Currently supported:

Xbox PAL
Xbox NTSC
Other platform versions are not supported.

Legal
Nightfire Recomp is an unofficial fan project.

It is not affiliated with, endorsed by, or associated with:

Electronic Arts (EA)
Eurocom
MGM
Danjaq
Microsoft
James Bond, 007, and Nightfire are trademarks of their respective owners.

License
The source code of this project is licensed under the MIT License.

Components included in:

external/xboxrecomp
may be subject to separate licenses. See their respective documentation for details.

Made for preserving and enjoying the original Xbox version of 007: Nightfire on modern PCs.
