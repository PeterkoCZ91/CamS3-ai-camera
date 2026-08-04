// CamS3 common helpers — CSRF-aware fetch, i18n alias, HTML escaping, toasts.
// Loaded WITHOUT defer (see every page's <head>) so that window.T / window.esc /
// window.showToast / window.apiFetch already exist when the inline page scripts
// are parsed. i18n.js may still be deferred: T() resolves window.I18N lazily.
(function () {
    // Set <html lang> before anything renders. i18n.js is deferred, so without this
    // a Czech user gets lang="en" on every page until the deferred script runs — bad
    // for screen readers and for hyphenation, and it is the same choice i18n.js will
    // make a moment later. Kept deliberately duplicated (rather than imported) so it
    // costs nothing and cannot be delayed.
    // Must mirror detectLang() in i18n.js, including the Slovak -> Czech mapping.
    try {
        const stored = localStorage.getItem('cams3_lang');
        let lang;
        if (stored === 'cs' || stored === 'en') {
            lang = stored;
        } else {
            const nav = (navigator.language || 'en').toLowerCase();
            lang = (nav.indexOf('cs') === 0 || nav.indexOf('sk') === 0) ? 'cs' : 'en';
        }
        document.documentElement.lang = lang;
    } catch (e) { /* private mode / storage disabled — leave the markup default */ }

    let csrfToken = null;
    let csrfFetchInFlight = null;

    function needsCsrf(method) {
        const m = (method || 'GET').toUpperCase();
        return m === 'POST' || m === 'PUT' || m === 'DELETE' || m === 'PATCH';
    }

    function fetchCsrf() {
        if (csrfToken) return Promise.resolve(csrfToken);
        if (csrfFetchInFlight) return csrfFetchInFlight;
        csrfFetchInFlight = fetch('/api/csrf', { credentials: 'same-origin' })
            .then(r => r.ok ? r.json() : Promise.reject(new Error('csrf ' + r.status)))
            .then(j => { csrfToken = j.token; return csrfToken; })
            .catch(e => { csrfToken = null; throw e; })
            .finally(() => { csrfFetchInFlight = null; });
        return csrfFetchInFlight;
    }

    // Drop-in fetch replacement: attaches X-CSRF-Token on mutating calls,
    // retries once if the server replies 403 csrf_required (token rotated).
    function apiFetch(url, opts) {
        opts = opts || {};
        opts.credentials = opts.credentials || 'same-origin';
        if (!needsCsrf(opts.method)) return fetch(url, opts);

        return fetchCsrf().then(token => {
            const headers = new Headers(opts.headers || {});
            headers.set('X-CSRF-Token', token);
            return fetch(url, Object.assign({}, opts, { headers }));
        }).then(resp => {
            if (resp.status !== 403) return resp;
            // Token likely rotated (reboot / restart). Refresh and retry once.
            csrfToken = null;
            return fetchCsrf().then(token => {
                const headers = new Headers(opts.headers || {});
                headers.set('X-CSRF-Token', token);
                return fetch(url, Object.assign({}, opts, { headers }));
            });
        });
    }

    // Pre-warm the CSRF token on load so first POST has it ready.
    document.addEventListener('DOMContentLoaded', () => {
        fetchCsrf().catch(() => { /* server may have csrf disabled — ignore */ });
    });

    // ── Shared UI helpers (previously copy-pasted into every page) ───────────

    // Localized string lookup. Falls back to the key itself so a missing
    // translation is visible instead of rendering "undefined".
    function T(key) {
        return (window.I18N && window.I18N.t(key)) || key;
    }

    // HTML-escape for the few places that still build markup as a string.
    function esc(s) {
        return String(s).replace(/[&<>"']/g, c => ({
            '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
        }[c]));
    }

    // Toast. Uses #toast when the page provides one, otherwise creates it, so a
    // page can call showToast() before/without declaring the element.
    let toastTimer = null;
    function showToast(text, type, ms) {
        let t = document.getElementById('toast');
        if (!t) {
            t = document.createElement('div');
            t.id = 'toast';
            t.className = 'toast';
            (document.body || document.documentElement).appendChild(t);
        }
        // Screen readers announce the text because the node is a live region.
        t.setAttribute('role', 'status');
        t.setAttribute('aria-live', 'polite');
        t.textContent = text;
        t.className = 'toast ' + (type || 'info') + ' show';
        if (toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(() => { t.className = 'toast'; }, ms || 3000);
    }

    window.apiFetch = apiFetch;
    window.fetchCsrfToken = fetchCsrf;
    window.T = T;
    window.esc = esc;
    window.escapeHtml = esc;   // legacy alias used by wifi.html
    window.showToast = showToast;
})();
