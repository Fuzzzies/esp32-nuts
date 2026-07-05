#pragma once

// Minimal Network UPS Tools (NUT) protocol server on TCP 3493.
// Supports the read-only subset that upsc / Home Assistant / Synology use:
// LIST UPS, LIST VAR, GET VAR, GET UPSDESC, USERNAME/PASSWORD/LOGIN/LOGOUT.
void nut_server_start(void);
