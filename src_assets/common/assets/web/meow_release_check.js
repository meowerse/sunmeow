/**
 * MEOW: update check against THIS fork's releases.
 *
 * Upstream queries `LizardByte/Sunshine/releases`, which on a fork means the web UI compares
 * our version against *Sunshine's* and announces "A new Stable Version is Available!" pointing
 * at a different product. That is the bug this file exists to fix.
 *
 * It also has to survive the state this fork is actually in: `meowerse/sunmeow` publishes **no
 * releases at all**, so both endpoints return 404. Upstream's inline code did not fail safe
 * there. A 404 body is a truthy object, so `new SunshineVersion(body)` does not throw -- it
 * yields `version: undefined`, and the computed guards happen to return false. But the
 * pre-release path calls `.find()` on the `/releases` body, and a 404 object has no `.find`,
 * so it throws a TypeError mid-`created()` and skips whatever follows it in the same `try`.
 *
 * The rule here: a missing or unreadable release feed is a NORMAL state, not an error. It
 * means "nothing to compare against" -- return null and let the caller's existing null guards
 * suppress the banner. Never throw, never announce an update we cannot substantiate.
 */

/** The repo whose releases describe THIS binary. */
export const MEOW_RELEASES_REPO = 'meowerse/sunmeow';

const API = `https://api.github.com/repos/${MEOW_RELEASES_REPO}/releases`;

/**
 * @param {string} url
 * @returns {Promise<any|null>} parsed body, or null on any failure
 */
async function fetchJsonOrNull(url) {
  try {
    const r = await fetch(url);
    if (!r.ok) {
      // 404 is expected until this fork cuts its first release; it is not an error worth
      // shouting about, and it must not look like "you are up to date" either.
      return null;
    }
    return await r.json();
  } catch (e) {
    // Offline, DNS-blocked, rate-limited (GitHub returns 403 with a JSON body). All the same
    // thing to us: no answer, so no claim.
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
  const body = await fetchJsonOrNull(API);
  // Guard the shape before calling .find -- this is the exact line that threw upstream.
  if (!Array.isArray(body)) {
    return null;
  }
  const found = body.find((release) => release && release.prerelease);
  return found && typeof found.tag_name === 'string' ? found : null;
}
