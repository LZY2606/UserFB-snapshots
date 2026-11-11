# version
v0.8
Matthew Szatmary m3u8@twitch.tv / matt@szatmary.org

# libcaption

libcaption is a library written in C to aid in the creating and parsing of closed caption data, open sourced under the MIT license to use within community developed broadcast tools. To maintain consistency across platforms libcaption aims to implement a subset of EIA608, CEA708 as supported by the Apple iOS platform.

608 support is currently limited to encoding and decoding the necessary control and preamble codes as well as support for the Basic North American, Special North American and Extended Western European character sets.

708 support is limited to encoding the 608 data in NTSC field 1 user data type structure.

## EIA608 roll-up window semantics
The screen is 15 rows by 32 columns and PAC row numbers are zero based, matching the `eia608_row_column_pramble`/`eia608_row_style_pramble` API. Roll-up 2/3/4 owns a window of 2/3/4 rows. With baseline row `b` and window size `N` the active range is `max(0, b-N+1)` through `b`, so a window near the top of the screen is simply clipped to the visible rows.

* **Carriage return** rolls only the active window up one row: the top row is discarded, the baseline row is cleared and the column resets to zero. Every cell outside the window, including its style and underline, is left untouched. Clipped windows still roll using the rows that are actually visible, so a carriage return is never dropped at the top of the screen.
* **PAC in roll-up mode** treats the current window as one block of cells and moves it, characters and styles together, by the difference between the old and new baselines. Rows moved off screen are discarded, rows the old window vacates are cleared, cells the moved block does not cover in the new window stay empty, and content outside both windows is unchanged. The PAC column and style then apply to whatever is written next.
* **Switching between RU2/RU3/RU4** keeps the baseline and preserves the lowest `min(old rows, new rows)` rows. Shrinking clears the rows that leave the window; growing exposes new empty rows.
* **Entering roll-up from paint-on or pop-on** does not move existing content. The roll-up control only selects the mode; the first roll-up PAC opens an empty window on its own baseline. Displayed content outside that window stays in place and text waiting in the non-displayed (pop-on) buffer is never pulled in.
* Repeated roll-up and carriage-return controls execute only once, matching `caption_frame_decode` duplicate suppression; a carriage return separated from the previous one by ordinary text still executes. Paint-on direct writing and pop-on background writing with end-of-caption publishing never trigger window scrolling.

A player can therefore feed the character, style and underline returned by `caption_frame_read_char` straight into its overlay renderer.

## Verification
`verification/check_rollup.sh` is self contained: it compiles the library, the roll-up window tests and the existing text wrapping test in a temporary directory and runs them. The tests cover the mid-screen baseline case, top clipping, PAC relocation in both directions, styled text, window grow/shrink, repeated carriage returns, off-screen row discard, mode entry from paint-on/pop-on, duplicate control handling, and cell-by-cell agreement between decoding directly with `caption_frame_decode` and decoding the same sequence wrapped as valid field-1 CEA708 data through `cea708_to_caption_frame` (invalid entries and field-2 entries are ignored). On failure it exits non-zero and reports the first row/column that differs.

```
bash verification/check_rollup.sh
```

`verification/baseline.sh` runs the smaller standalone witness used while fixing the window rebuild.


In addition, utility functions to create h.264 SEI (Supplementary enhancement information) NALUs (Network Abstraction Layer Unit) for inclusion into an h.264 elementary stream are provided.

H.264 utility functions are limited to wrapping the 708 payload into a SEI NALU. This is accomplished by prepending the 708 payload with 3 bytes (nal_unit_type = 6, payloadType = 4, and PayloadSize = variable), and appending a stop bit encoded into a full byte (with a value of 127). In addition if the 708 payload contains an emulated start code, a three byte sequence equaling 0,0,1 an emulation prevention byte (3) is inserted. Functions to reverse this operation are also provided.

## Characters
| | | | | | | | | | | | | | | | | |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|BNA| |!|"|#|$|%|&|’|(|)|á|+|,|-|.|/|
|BNA|0|1|2|3|4|5|6|7|8|9|:|;|<|=|>|?|
|BNA|@|A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|
|BNA|P|Q|R|S|T|U|V|W|X|Y|Z|[|é|]|í|ó|
|BNA|ú|a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|
|BNA|p|q|r|s|t|u|v|w|x|y|z|ç|÷|Ñ|ñ|█|
|SNA|®|°|½|¿|™|¢|£|♪|à|&nbsp;|è|â|ê|î|ô|û|
|WES|Á|É|Ó|Ú|Ü|ü|‘|¡|*|'|—|©|℠|•|“|”|
|WEF|À|Â|Ç|È|Ê|Ë|ë|Î|Ï|ï|Ô|Ù|ù|Û|«|»|
|WEP|Ã|ã|Í|Ì|ì|Ò|ò|Õ|õ|{|}|&#92;|^|_|&#124;|~|
|WEG|Ä|ä|Ö|ö|ß|¥|¤|¦|Å|å|Ø|ø|┌|┐|└|┘|

* BNA = Basic North American character set
* SNA = Special North American character set
* WES = Extended Western European character set : Extended Spanish/Miscellaneous
* WEF = Extended Western European character set : Extended French
* WEP = Extended Western European character set : Portuguese
* WEG = Extended Western European character set : German/Danish

## Limitations
Current B-frame support for caption creation is minimal. libcaption ensures no re-ordering of captions is required
on playback.

## Build Directions
# Mac Os/Linux
Install build dependencies (git, cmake, a compiler such as xcode, gcc or clang and optionally re2c and ffmpeg)
* run `cmake . && make`
* or to compile without re2c `cmake -DENABLE_RE2C=OFF . && make`
* finally `sudo make install` to install
# Windows
I have never tested libcaption in windows. It is written in pure C with no dependencies,
so there is no reason it would not work.
