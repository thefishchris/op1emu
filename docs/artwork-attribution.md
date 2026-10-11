# GUI artwork attribution

This covers the two images in this repository that show the emulated
OP-1's front panel:
- `gui/ui.png`, the GUI background, whose control positions `gui/ui.json`
  describes;
- `screenshot.png`, the README screenshot.

## Not affiliated with Teenage Engineering

op1emu is an independent project. It is not affiliated with, endorsed by or
sponsored by Teenage Engineering. "Teenage Engineering" and "OP-1" are used
only to identify the hardware that is emulated; those names, the OP-1's
industrial design and its interface symbols belong to Teenage Engineering.

## Origin of `gui/ui.png`

`gui/ui.png` (3300×1188 pixels) has been in the project since upstream
op1emu's first commit. No record of how it was made was kept.

**What it is:** it is LIKELY a rendered capture of Shunya Koide's HTML/CSS
OP-1 design, as published on CodePen and used by Alex Ownejazayeri's
[op1kenobi](https://github.com/alexmandelshtam/op1kenobi) web app.
- **Evidence:** the panel of the rendered op1kenobi page measures 825×297
  CSS pixels, and `gui/ui.png` is exactly four times that size.
- **Match:** layout, symbols and colours match.

**Edits:** compared with that rendering, the display area is blank and the
"OP-1" wordmark at the right edge is absent.

**Correction:** earlier versions of the README described the image as
"OP-1 screenshot assets" from op1kenobi. op1kenobi contains no such image
files; it draws the panel in HTML/CSS.

## Origin of `screenshot.png`

A screenshot of the emulator's GUI window. It shows `gui/ui.png` as the
background, and in the display, the boot loader menu produced by the OP-1
firmware the emulator was running. The firmware is not part of this project.

## Licences of the design and code the artwork is based on

op1kenobi's `LICENSE.txt` grants the following MIT licences. Their notices
are reproduced unchanged, as those licences require:

```text
CSS & HTML License by Shunya Koide -- project inspiration and web design wizard. Check out their work yourself!

Copyright (c) 2022 by Shunya Koide (https://codepen.io/shunyadezain/pen/QWyVwep)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

MIT License

Copyright (c) 2022 Alex Ownejazayeri

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## What these licences do and do not cover

- **Covered:** the MIT licences above are granted by Shunya Koide (the
  HTML/CSS design) and Alex Ownejazayeri (op1kenobi). Their notices are
  reproduced here to meet the licences' condition.
- **Not explicit:** whether those licences extend to a rendered and edited
  raster image of the design is not stated in them. This project treats it
  as a reasonable reading, not a certainty.
- **Not covered at all:** the licences do not grant any rights held by
  Teenage Engineering:
  - the OP-1's industrial design and appearance;
  - its key and interface symbols;
  - its names and trademarks;
  - the output of its firmware shown in `screenshot.png`.

  No permission from Teenage Engineering has been sought or given.

## Status

These questions are open (see the project's release notes):
- permission for the edited render;
- Teenage Engineering's position.

A replacement drawn from `gui/ui.json` geometry, needing no third-party
artwork, is being considered. It would keep the same control positions,
keyboard mapping and tests.
