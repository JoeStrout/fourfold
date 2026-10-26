// The page's data, from the <script type="application/json"> the template
// filled: a safe way to hand values to JavaScript (no strings built in JS).
const stats = JSON.parse(document.getElementById("guestbook-stats").textContent);
console.log("guestbook:", stats.entries, "entries");
