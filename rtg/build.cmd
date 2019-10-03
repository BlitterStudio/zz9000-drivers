@ECHO OFF
REM Windows specific vbcc build script
REM This implies that you have the following:
REM 1) vbcc already installed and in your path (check https://blitterstudio.com/) in D:\vbcc
REM 2) The NDK 3.9 included in your "aos68k" config
REM 
REM Please adapt the script accordingly if your environment is different
REM 
vc +aos68k -nostdlib -c99 -O2 -o ZZ9000.card mntgfx.c -lamiga -ldebug