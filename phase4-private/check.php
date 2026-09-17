<?php
header('Content-Type: text/html; charset=UTF-8');
$p = $_COOKIE['mb_path'] ?? '(missing)';
?><!doctype html><html><head><title>Phase 4 Private Check</title></head><body>
<h1>PRIVATE COOKIE CHECK</h1>
<p>mb_path: <?= htmlspecialchars($p, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8') ?></p>
<p>END PRIVATE COOKIE CHECK</p></body></html>