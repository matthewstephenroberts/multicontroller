# Third-party notices

This project bundles the following third-party open-source software. Both licenses below are
permissive (no copyleft, no royalty, free for commercial use, no UI attribution required) — kept
here so the terms travel with the repo rather than living only inside `node_modules`, which isn't
committed to git.

## lucide-react (icons)

- Source: https://lucide.dev/license · https://github.com/lucide-icons/lucide
- License: ISC (package + original Lucide icons), plus MIT for a subset derived from Feather

```
ISC License

Copyright (c) 2026 Lucide Icons and Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

The following Lucide icons are derived from the Feather project — includes several we use
(`search`, `monitor`, `moon`, `smartphone`, `x`) — and additionally carry the Feather project's
MIT License below:

airplay, alert-circle, alert-octagon, alert-triangle, aperture, arrow-down-circle,
arrow-down-left, arrow-down-right, arrow-down, arrow-left-circle, arrow-left, arrow-right-circle,
arrow-right, arrow-up-circle, arrow-up-left, arrow-up-right, arrow-up, at-sign, calendar, cast,
check, chevron-down, chevron-left, chevron-right, chevron-up, chevrons-down, chevrons-left,
chevrons-right, chevrons-up, circle, clipboard, clock, code, columns, command, compass,
corner-down-left, corner-down-right, corner-left-down, corner-left-up, corner-right-down,
corner-right-up, corner-up-left, corner-up-right, crosshair, database, divide-circle,
divide-square, dollar-sign, download, external-link, feather, frown, hash, headphones,
help-circle, info, italic, key, layout, life-buoy, link-2, link, loader, lock, log-in, log-out,
maximize, meh, minimize, minimize-2, minus-circle, minus-square, minus, monitor, moon,
more-horizontal, more-vertical, move, music, navigation-2, navigation, octagon, pause-circle,
percent, plus-circle, plus-square, plus, power, radio, rss, search, server, share, shopping-bag,
sidebar, smartphone, smile, square, table-2, tablet, target, terminal, trash-2, trash, triangle,
tv, type, upload, x-circle, x-octagon, x-square, x, zoom-in, zoom-out

```
The MIT License (MIT)

Copyright (c) 2013-present Cole Bemis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

Neither license requires attribution to be shown in the app UI itself; this file satisfies
"include the copyright notice" for redistribution of the library.

## Fredoka & Nunito (fonts, via @fontsource)

- Fredoka source: https://github.com/hafontia/Fredoka-One · https://fonts.google.com/specimen/Fredoka
- Nunito source: https://github.com/googlefonts/nunito · https://fonts.google.com/specimen/Nunito
- Distributed as self-hosted static files via `@fontsource/fredoka` and `@fontsource/nunito`
  (imported in `src/main.tsx`) rather than a Google Fonts CDN `<link>` — no external request at
  runtime, works offline, and both packages ship their own copy of this license.
- License: SIL Open Font License, Version 1.1 (full text: http://scripts.sil.org/OFL) — free to
  use, modify, and embed/bundle with the app, including commercially, with no UI attribution
  requirement. The only restriction (OFL §1) is that the font files themselves can't be sold on
  their own, separate from the app — irrelevant here since they're bundled into the build, not
  redistributed standalone.

```
Copyright 2016 The Fredoka Project Authors (https://github.com/hafontia/Fredoka-One)
Copyright 2014 The Nunito Project Authors (https://github.com/googlefonts/nunito)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
