<?php
header('Content-Type: text/html; charset=UTF-8');
$s = $_COOKIE['mb_session'] ?? '(missing)';
$p = $_COOKIE['mb_path'] ?? '(missing)';
?><!doctype html><html><head><title>Phase 4 Cookie Check</title></head><body>
<h1>COOKIE CHECK PAGE</h1>
<p>mb_session: <?= htmlspecialchars($s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8') ?></p>
<p>mb_path: <?= htmlspecialchars($p, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8') ?></p>
<p>END COOKIE CHECK</p></body></html>