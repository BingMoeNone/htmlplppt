import { readdirSync } from 'node:fs';
import path from 'node:path';

// Windows ships no fontconfig, so fc-match is unavailable there. Map the
// requested family straight to the font file the renderer will pick; the
// default "Microsoft YaHei" is msyh.ttc on every zh-CN install.
export const WINDOWS_FONT_FAMILIES = [
  ['microsoft yahei', [/^msyh(?:bd)?\.ttc$/i]],
  ['dengxian', [/^deng(?:xian|b|i)?\.ttf$/i]],
  ['simhei', [/^simhei\.ttf$/i]],
  ['simsun', [/^simsun\.ttc$/i]],
  ['noto sans cjk', [/^notosanscjk/i]],
  ['noto serif cjk', [/^notoserifcjk/i]],
  ['source han', [/^sourcehan/i]]
];

export function supportedWindowsFonts() {
  return WINDOWS_FONT_FAMILIES.map(([name]) => name);
}

// Matches a Required_FONT against the Windows Fonts directory. `files` and
// `fontsDir` are injectable so the failure modes can be unit-tested without a
// Windows install. An unknown family fails closed: falling back to "any CJK
// font" silently renders the video in the wrong typeface.
export function matchWindowsFont(required, { fontsDir, files } = {}) {
  const dir = fontsDir ?? path.join(process.env.WINDIR ?? 'C:\\Windows', 'Fonts');
  let entries = files;
  if (!entries) {
    try {
      entries = readdirSync(dir);
    } catch (error) {
      throw new Error(`font preflight failed: cannot read ${dir} (${error.message})`);
    }
  }
  const wanted = required.toLowerCase();
  const family = WINDOWS_FONT_FAMILIES.find(
    ([name]) => wanted === name || wanted.startsWith(`${name} `)
  );
  if (!family) {
    throw new Error(
      `font preflight failed: unsupported REQUIRED_FONT "${required}". ` +
        `Supported: ${supportedWindowsFonts().join(', ')}`
    );
  }
  for (const pattern of family[1]) {
    const hit = entries.find((file) => pattern.test(file));
    if (hit) return path.join(dir, hit);
  }
  throw new Error(
    `font preflight failed: "${required}" not found under ${dir}. ` +
      `Install the family or set REQUIRED_FONT to one of: ` +
      supportedWindowsFonts().join(', ')
  );
}
