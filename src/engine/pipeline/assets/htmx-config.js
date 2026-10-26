
// Fourfold's htmx settings (overview §7, §8 "Content Security Policy"),
// applied as soon as the library has loaded, before it starts.
//
// - No eval: hx-on, js: values and trigger filters can't run code. (The CSP
//   forbids eval anyway; this makes htmx not try.)
// - No <script> elements run from swapped-in content: a fragment's script
//   wouldn't have the page's nonce.
// - No inline indicator styles; the app styles .htmx-indicator itself.
// - Requests only to this site.
// - 422 responses are swapped, so a form can come back with its errors.
htmx.config.allowEval = false;
htmx.config.allowScriptTags = false;
htmx.config.includeIndicatorStyles = false;
htmx.config.selfRequestsOnly = true;
htmx.config.responseHandling = [
    {code: "204", swap: false},
    {code: "[23]..", swap: true},
    {code: "422", swap: true},
    {code: "[45]..", swap: false, error: true},
    {code: "...", swap: false}
];
