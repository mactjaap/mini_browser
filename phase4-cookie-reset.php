<?php
header('Content-Type: text/html; charset=UTF-8');

/* Deterministic Phase 4 test reset.
 * Delete each test cookie using the same Path with which it is created.
 */
header('Set-Cookie: mb_session=reset; Path=/; Secure; Max-Age=0', false);
header('Set-Cookie: mb_path=reset; Path=/phase4-private; Secure; Max-Age=0', false);
?>
<!doctype html>
<html>
<head><title>Phase 4 Cookie Reset</title></head>
<body>
<h1>COOKIE RESET PAGE</h1>
<p>Phase 4 test cookies cleared.</p>
<p>END COOKIE RESET</p>
</body>
</html>
