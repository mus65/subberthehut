subberthehut
============

##### subberthehut is a command-line based OpenSubtitles.org downloader, written in C.

It is inspired by [subdl](http://code.google.com/p/subdl/), which is not developed anymore.
The foremost reason I wrote this is because subdl only supports hash-based search.
subberthehut can also do a name-based search in case the hash-based searched returns no results.

Also program upload information (imdb_id, movie hash, fps, duration) to opensubtitles.org when it founds imdb number from .nfo files.

##### Dependencies:
 * xmlrpc-c
 * glib2
 * zlib
 * ffmpeg
 * bash-completion (make only)

AUR package: https://aur.archlinux.org/packages/subberthehut/
