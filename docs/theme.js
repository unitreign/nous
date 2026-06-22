// Early init: runs synchronously before first render to prevent theme flash.
(function () {
  var saved = localStorage.getItem('theme');
  var prefersDark = matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.setAttribute('data-theme', saved || (prefersDark ? 'dark' : 'light'));
})();

// Wire up the toggle button once the DOM is ready.
document.addEventListener('DOMContentLoaded', function () {
  var toggle = document.getElementById('theme-toggle');
  if (!toggle) return;
  function sync() {
    var t = document.documentElement.getAttribute('data-theme');
    toggle.textContent = t === 'dark' ? '☀︎' : '🌙︎';
    toggle.setAttribute('aria-label', 'Switch to ' + (t === 'dark' ? 'light' : 'dark') + ' mode');
  }
  sync();
  toggle.addEventListener('click', function () {
    var cur = document.documentElement.getAttribute('data-theme');
    var next = cur === 'dark' ? 'light' : 'dark';
    localStorage.setItem('theme', next);
    document.documentElement.setAttribute('data-theme', next);
    sync();
  });
});
