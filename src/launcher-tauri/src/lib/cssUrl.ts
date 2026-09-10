/** A CSS `url()` value for an arbitrary path-derived URL. Always quoted:
 * convertFileSrc() percent-encodes with encodeURIComponent, which leaves
 * "(", ")", "'", "!" and "*" literal, and an unquoted CSS url-token may not
 * contain parentheses, so a game folder like ".../Worms (2024) (EU)/" makes
 * the whole background-image declaration a parse error. React assigns style
 * through CSSOM, which silently ignores an invalid value, so the element
 * keeps whatever image it had, which is how the PREVIOUS game's hero art
 * ended up stuck on screen (real bug, 2026-09-10). */
export function cssUrl(url: string): string {
  return `url("${url.replace(/[\\"]/g, "\\$&")}")`;
}
