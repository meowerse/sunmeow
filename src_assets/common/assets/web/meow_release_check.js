/**
 * MEOW: update check against THIS fork's releases.
 *
 * Upstream queries `LizardByte/Sunshine/releases`, which on a fork means the web UI compares
 * our version against *Sunshine's* and announces "A new Stable Version is Available!" pointing
 * at a different product. That is the bug this file exists to fix.
 *
 * It also has to survive the state this fork is actually in: `meowerse/sunmeow` has published no
 * releases. Verified against the live API rather than assumed -- the two endpoints do NOT behave
 * the same way, which is the whole subtlety:
 *
 *   GET /repos/meowerse/sunmeow/releases         -> 200, body `[]`
 *   GET /repos/meowerse/sunmeow/releases/latest  -> 404, body `{"message":"Not Found",...}`
 *
 * So upstream's inline code broke on the LIST endpoint, not the 404 one. `.find()` ran fine on
 * the empty array and returned `undefined`; `new SunshineVersion(undefined, null)` then hit the
 * constructor's else-branch and threw `Error('Either release or version must be provided')`,
 * mid-`created()`, skipping whatever followed in the same `try` (on Windows, the virtual-input
 * status and licence fetches). The `/releases/latest` 404 was the harmless half: its body is a
 * truthy object, so the constructor took the `if (release)` branch, yielded `version: undefined`,
 * and the computed guards suppressed the banner by accident rather than by design.
 *
 * (A `.find()`-on-a-non-array TypeError IS reachable -- but from a 403 rate-limit body, not from
 * the no-releases state. `Array.isArray` below covers it.)
 *
 * The rule here: a missing or unreadable release feed is a NORMAL state, not an error. It
 * means "nothing to compare against" -- return null and let the caller's existing null guards
 * suppress the banner. Never throw, never announce an update we cannot substantiate.
 *
 * NOTE ON TRUST: `release.body` is rendered through `marked` with `sanitize: false` into a
 * `v-html` sink in index.html. That is upstream's pre-existing pipeline, unchanged here -- but
 * pointing it at our own repo narrows rather than widens it: the content author moves from
 * LizardByte's release managers to whoever can publish a release to meowerse/sunmeow.
 */

/** The repo whose releases describe THIS binary. */
export const MEOW_RELEASES_REPO = 'meowerse/sunmeow';

const API = `https://api.github.com/repos/${MEOW_RELEASES_REPO}/releases`;
// GitHub defaults to per_page=30. Once 30 stable releases exist, an older pre-release would
// silently fall off the first page and stop being offered.
const LIST_API = `${API}?per_page=100`;

/**
 * @param {string} url
 * @returns {Promise<any|null>} parsed body, or null on any failure
 */
async function fetchJsonOrNull(url) {
  try {
    const r = await fetch(url);
    if (!r.ok) {
      // 404 is expected until this fork cuts its first release. Logged, not silent: collapsing
      // 404 / 403-rate-limit / CORS into one null would otherwise destroy the only diagnostic a
      // user could paste into a bug report.
      console.debug(`release check: ${url} -> HTTP ${r.status}`);
      return null;
    }
    return await r.json();
  } catch (e) {
    // Offline or DNS-blocked. Same outcome as above: no answer, so no claim.
    console.debug(`release check: ${url} -> ${e && e.message ? e.message : e}`);
    return null;
  }
}

/**
 * Latest published stable release, or null when there is none.
 * @returns {Promise<object|null>}
 */
export async function fetchLatestRelease() {
  const body = await fetchJsonOrNull(`${API}/latest`);
  // A release object always carries a tag_name; a 404/error body does not. Checking the field
  // we actually consume is stricter than checking the status alone.
  return body && typeof body.tag_name === 'string' ? body : null;
}

/**
 * Newest pre-release, or null.
 * @returns {Promise<object|null>}
 */
export async function fetchLatestPreRelease() {
  const body = await fetchJsonOrNull(LIST_API);
  // Guard the shape before calling .find -- this is the exact line that threw upstream.
  if (!Array.isArray(body)) {
    return null;
  }
  const found = body.find((release) => release && release.prerelease);
  return found && typeof found.tag_name === 'string' ? found : null;
}
