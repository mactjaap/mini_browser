<?php
header('Content-Type: text/html; charset=UTF-8');

function h($value) {
    return htmlspecialchars((string)$value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

$method = $_SERVER['REQUEST_METHOD'] ?? '';
$content_type = $_SERVER['CONTENT_TYPE'] ?? '';
$raw = file_get_contents('php://input');

$user = $_POST['user'] ?? '';
$message = $_POST['message'] ?? '';
$token = $_POST['token'] ?? '';
$submit = $_POST['submit'] ?? '';
?>
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Phase 3 POST Result</title>
</head>
<body>
<h1>Phase 3 POST Result</h1>
<p>POST RESULT START</p>
<p>METHOD: <?= h($method) ?></p>
<p>CONTENT-TYPE: <?= h($content_type) ?></p>
<p>user: <?= h($user) ?></p>
<p>message: <?= h($message) ?></p>
<p>token: <?= h($token) ?></p>
<p>submit: <?= h($submit) ?></p>
<p>RAW: <?= h($raw) ?></p>
<p>POST RESULT END</p>
</body>
</html>
