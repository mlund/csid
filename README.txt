                             cSID-light by Hermit
                             ====================

 This version of cSID is more closely based on jsSID, using the same sampling
frequency paced (non-cycleexact) approach to cause far lower CPU load...
(Peaks at 15% on an EEEPC900 with Celeron CPU at 630Mhz power-saving mode 
 playing single SID tune, 2SID and 3SID eat more of course. On the backside it
 means improper ADSR handling might happen in peculiar cases, and the combined
 waveforms don't sound as clean as in the 1MHz 'oversampling' 'csid' version.)

cSID is a cross-platform commandline SID-player based on the code of my other
project called jsSID. Some people asked at CSDB if I could make a C/C++ port
to run on native machine, and DefleMask guys started to port jsSID to C++, and
got involved by helping them, actually finishing the work as I know this
engine the best as the original author. This was last year, and beside other 
projects, I ported the CPU emulation and player engine as well to C. Some guys
also asked for a shared library (.dll/.so) that they can use in their non-GPL
licensed or closed-source programs. I haven't made a shared library yet but
it wouldn't be hard to make one from this C code if someone wants. Actually
this program is nothing more than CPU and a SID functions that are periodically
called by the audio-callback. I used SDL-1.2 audio routines for this task now.
That means you may easily compile cSID on platforms other than Linux if you
want... (To my knowledge, some sceners already made OSX & Windows shared library
and executable versions of the cycle-based 'csid' version.)

Usage is simple, you can get a clue about required commandline parameters if
you run 'csidl' without any. If you write irrelevant values for a SID they get
ignored. (E.g. subtune-number larger than present in .sid file).
You can type - in the place of SID model if you don't want to explicitly set
it but want to tell the playtime to cSID. Otherwise, type 8580 or 6581, and
it will have precedence over the SID-models originally asked by the .sid file.
If you want to play the tune infinitely, don't give playtime and it will play
till you press Enter. 
 As you can give the playtime in seconds, you can play more SID files from a
simple bash/batch script after each other, or you can use cSID as a backend for
a GUI program you develop... (This is the gnu/unix idea of apps I guess...)
(Check 'csida' bash script I created to play all .sid/.SID files in a folder.)
 When a tune plays you can see some important information about played .sid.

Some 'extra' features in cSID-light:
-2SID and 3SID tunes are playable (in monaural mode)
-Illegal opcodes are now fully supported (except nonsense NOP variants and JAM)
-closer 6581 filter emulation with distortion and comments on workings of filter
-playlist file support (subtune and playtime can be given for each tune)
 (Playlist is any file with other than .sid extension, the format of titles in
  it are like that of jsSID's: <filepath+name> [minutes:seconds[:subtune]] )
-If you don't want to create and use playlist-files, you can play all the
 .sid files in a directory by linux bash script 'csida'

Proper cycle-based CIA and VIC IRQ emulation is still not implemented, but got
mimiced in a way that most of the SID files can be played, except digi-tunes,
where more complex CIA routines are used. So the SID() function doesn't contain
digi-playback related code either. Maybe in later versions if there will be any.

So don't expect a full SID-playback environment from cSID, but think of it as
a fairly good sounding but really small (30kbyte) standalone player, which can
run without dependencies (except SDL_audio). Who knows, it might be your only
option sometimes, when you don't have GUI and bloatware on your system.

      2017 Hermit Software (Mihaly Horvath) - http://hermit.sidrip.com
