<?php
/* Mini Browser 4.5 regression test page: behaves like an Anubis gate.
 * A client that calls itself "Mozilla" gets the challenge page; a client
 * with its own plain name gets the content. */
header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');
$ua = isset($_SERVER['HTTP_USER_AGENT']) ? $_SERVER['HTTP_USER_AGENT'] : '';
if (strpos($ua, 'Mozilla') !== false) {
?><!doctype html>
<html><head><title>Making sure you&#39;re not a bot!</title></head>
<body>
<script id="anubis_challenge" type="application/json">{"challenge":"test","rules":{"difficulty":4}}</script>
<h1>Making sure you're not a bot!</h1>
<img src="/.within.website/x/cmd/anubis/static/img/pensive.webp" alt="">
<p>Loading...</p>
<p>Protected by <a href="https://github.com/TecharoHQ/anubis">Anubis</a> From <a href="https://techaro.lol">Techaro</a>.</p>
</body></html>
<?php
    exit;
}
?><!doctype html>
<html><head><title>Mini Browser 4.5 Anubis test</title></head>
<body>
<p>START ANUBIS</p>
<p>The gate let this client in: <?php echo htmlspecialchars($ua); ?></p>
<p>END ANUBIS</p>
</body></html>
