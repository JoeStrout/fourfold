// Fourfold's proof-of-work solver (overview §5, "Proof of work").
//
// A form whose route asks for a challenge carries a hidden _pow input with
// a puzzle in data-ff-challenge. The answer is the puzzle, "~", and a
// number such that SHA-256 of that text starts with the puzzle's number of
// zero bits (its third dot-separated part). Solving starts when the visitor
// starts on the form, a slice at a time so the page stays responsive; if
// they submit first, the submission waits for the answer.
(function () {
  "use strict";
  if (window.fourfoldChallenge) return;   // loaded twice (with htmx.js and on its own)

  var K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2];
  var W = new Int32Array(64);

  // SHA-256 of ASCII text (puzzles and answers are ASCII): the first word
  // of the digest is all the check needs, but the rest are returned too.
  function sha256(text) {
    var n = text.length, blocks = ((n + 8) >> 6) + 1, words = new Int32Array(blocks * 16), i;
    for (i = 0; i < n; i++) words[i >> 2] |= text.charCodeAt(i) << (24 - (i & 3) * 8);
    words[n >> 2] |= 0x80 << (24 - (n & 3) * 8);
    words[blocks * 16 - 1] = n * 8;
    var h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a,
        h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    for (var b = 0; b < words.length; b += 16) {
      var a = h0, bb = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7, t;
      for (i = 0; i < 64; i++) {
        if (i < 16) {
          W[i] = words[b + i];
        } else {
          var x = W[i - 15], y = W[i - 2];
          W[i] = (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3)) + W[i - 7] +
                 (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10)) + W[i - 16];
        }
        t = h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))) +
            ((e & f) ^ (~e & g)) + K[i] + W[i];
        var t2 = (((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))) +
                 ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = (d + t) | 0; d = c; c = bb; bb = a; a = (t + t2) | 0;
      }
      h0 = (h0 + a) | 0; h1 = (h1 + bb) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
      h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
    }
    return [h0, h1, h2, h3, h4, h5, h6, h7];
  }

  function leadingZeros(digest) {
    var n = 0;
    for (var i = 0; i < digest.length; i++) {
      if (digest[i] === 0) { n += 32; continue; }
      return n + Math.clz32(digest[i]);
    }
    return n;
  }

  // Solves the puzzle of one _pow input, then calls each waiting callback.
  function solve(input) {
    if (input.value || input._ffSolving) return;
    input._ffSolving = true;
    var puzzle = input.getAttribute("data-ff-challenge");
    var bits = parseInt(puzzle.split(".")[2], 10) || 0;
    var nonce = 0;
    function slice() {
      var stop = Date.now() + 30;
      while (Date.now() < stop) {
        for (var i = 0; i < 2000; i++, nonce++) {
          var answer = puzzle + "~" + nonce;
          if (leadingZeros(sha256(answer)) >= bits) {
            input.value = answer;
            input._ffSolving = false;
            var waiting = input._ffWaiting || [];
            input._ffWaiting = [];
            waiting.forEach(function (f) { f(); });
            return;
          }
        }
      }
      setTimeout(slice, 0);
    }
    slice();
  }

  function powInput(form) {
    return form && form.querySelector && form.querySelector("input[name=_pow][data-ff-challenge]");
  }

  // Start when the visitor starts on the form.
  ["focusin", "input", "pointerdown"].forEach(function (type) {
    document.addEventListener(type, function (event) {
      var form = event.target && event.target.closest && event.target.closest("form");
      var input = powInput(form);
      if (input) solve(input);
    }, true);
  });

  // A submission before the answer is ready waits for it (htmx listens for
  // submit too; this runs first, in the capture phase).
  document.addEventListener("submit", function (event) {
    var form = event.target, input = powInput(form);
    if (!input || input.value) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    (input._ffWaiting = input._ffWaiting || []).push(function () {
      if (form.requestSubmit) form.requestSubmit(event.submitter || undefined);
      else form.submit();
    });
    solve(input);
  }, true);

  window.fourfoldChallenge = {sha256: sha256, solve: solve};
})();
